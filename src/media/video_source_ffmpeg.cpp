#include "media/ffmpeg_util.hpp"
#include "media/picture_upload.hpp"
#include "media/video_source.hpp"

extern "C" {
#include <libavutil/display.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>

namespace azl {

namespace {

std::atomic<int64_t> gLiveReaders{0};

// A file nothing recognises is unsupported; one a demuxer recognises but cannot open is corrupt.
MediaStatus openFailureStatus(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    std::vector<uint8_t> head(4096 + AVPROBE_PADDING_SIZE);
    in.read(reinterpret_cast<char*>(head.data()), 4096);
    const std::u8string name = file.u8string();
    AVProbeData probe{reinterpret_cast<const char*>(name.c_str()), head.data(), int(in.gcount()), nullptr};
    return av_probe_input_format(&probe, 1) ? MediaStatus::Corrupt : MediaStatus::Unsupported;
}

int turnsOf(const AVStream* stream)
{
    const AVPacketSideData* side = av_packet_side_data_get(stream->codecpar->coded_side_data,
                                                           stream->codecpar->nb_coded_side_data,
                                                           AV_PKT_DATA_DISPLAYMATRIX);
    if (!side || side->size < sizeof(int32_t) * 9)
        return 0;
    const double counterclockwise = av_display_rotation_get(reinterpret_cast<const int32_t*>(side->data));
    return std::isnan(counterclockwise) ? 0 : int(std::lround(-counterclockwise / 90.0)) & 3;
}

YuvMatrix matrixOf(const AVCodecParameters* p)
{
    switch (p->color_space) {
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: return YuvMatrix::Rec601;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: return YuvMatrix::Rec2020;
    case AVCOL_SPC_UNSPECIFIED:
    case AVCOL_SPC_RGB:
    case AVCOL_SPC_RESERVED: return p->height > 576 ? YuvMatrix::Rec709 : YuvMatrix::Rec601;
    default: return YuvMatrix::Rec709;
    }
}

bool fullRangeOf(const AVCodecParameters* p)
{
    switch (p->format) {
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_YUVJ444P: return true;
    default: return p->color_range == AVCOL_RANGE_JPEG;
    }
}

// Decoded pictures keep more than 8 bits as P010.
bool deepOf(const AVCodecParameters* p)
{
    const AVPixFmtDescriptor* format = av_pix_fmt_desc_get(AVPixelFormat(p->format));
    return format && format->comp[0].depth > 8;
}

// The hardware decoder every clip shares: D3D11VA (NVDEC on NVIDIA, the vendor's decoder elsewhere), then CUDA. Null
// when there is none or NOCTURNE_SOFTWARE_DECODE is set, and every clip decodes on the CPU.
struct Hardware {
    AVBufferRef* device = nullptr;
    AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
};

const Hardware& hardware()
{
    static const Hardware shared = [] {
        Hardware h;
        if (const char* software = std::getenv("NOCTURNE_SOFTWARE_DECODE"); software && *software && *software != '0')
            return h;
        for (const AVHWDeviceType type : {AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_CUDA}) {
            if (av_hwdevice_ctx_create(&h.device, type, nullptr, nullptr, 0) == 0) {
                h.type = type;
                std::fprintf(stderr, "nocturne: video decodes on the GPU through %s\n", av_hwdevice_get_type_name(type));
                return h;
            }
        }
        std::fprintf(stderr, "nocturne: no hardware video decoder; video decodes on the CPU\n");
        return h;
    }();
    return shared;
}

AVPixelFormat hardwareFormat(const AVCodec* codec, AVHWDeviceType type)
{
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (!config)
            return AV_PIX_FMT_NONE;
        if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && config->device_type == type)
            return config->pix_fmt;
    }
}

// Takes the hardware format when the decoder offers it for this stream, else the decoder's first software format.
AVPixelFormat pickFormat(AVCodecContext* context, const AVPixelFormat* offered)
{
    const auto wanted = AVPixelFormat(reinterpret_cast<intptr_t>(context->opaque));
    for (const AVPixelFormat* f = offered; *f != AV_PIX_FMT_NONE; ++f)
        if (*f == wanted)
            return *f;
    for (const AVPixelFormat* f = offered; *f != AV_PIX_FMT_NONE; ++f)
        if (!(av_pix_fmt_desc_get(*f)->flags & AV_PIX_FMT_FLAG_HWACCEL))
            return *f;
    return AV_PIX_FMT_NONE;
}

// What every reader of a file needs to know about its track, beyond SourceTrack.
struct Stream {
    std::filesystem::path path;
    int index = 0;
    AVRational timeBase{1, 1};
    int64_t start = 0;
    double fps = 0.0;
    HapCodec hap = HapCodec::None;
    bool deep = false;
    YuvMatrix matrix = YuvMatrix::Rec709;
    bool fullRange = false;
    // The demuxer's timestamp of each keyframe by frame index, where readers seek to start there.
    std::vector<std::pair<int, int64_t>> seekPoints;

    int frameIndex(int64_t pts) const
    {
        return int(std::floor(double(pts - start) * av_q2d(timeBase) * fps + 0.25));
    }
};

// Reads and decodes forward with a demuxer and decoder of its own, so two readers of one file never share a position.
class FfmpegReader final : public SourceReader {
public:
    FfmpegReader(std::shared_ptr<const Stream> stream, int fromFrame) : stream_(std::move(stream)), from_(fromFrame)
    {
        int error = 0;
        format_ = ff::openFormat(stream_->path, error);
        if (!format_)
            throw MediaError(MediaStatus::Corrupt, ff::describe(error));
        if (stream_->hap == HapCodec::None)
            openDecoder();
        if ((error = seek()) < 0)
            throw MediaError(MediaStatus::Corrupt, "cannot seek: " + ff::describe(error));
    }

    SourceRead read() override
    {
        while (true) {
            SourceRead sample = stream_->hap == HapCodec::None ? decodeNext() : packetNext();
            if (!sample || sample->index >= from_)
                return sample;
        }
    }

    void cancel() override {}

    std::optional<MediaError> failure() const override { return failure_; }

private:
    void openDecoder()
    {
        const AVCodecParameters* parameters = format_->streams[stream_->index]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
        if (!codec)
            throw MediaError(MediaStatus::Unsupported, std::string("no decoder for ") +
                                                           avcodec_get_name(parameters->codec_id));
        codec_.reset(avcodec_alloc_context3(codec));
        if (!codec_ || avcodec_parameters_to_context(codec_.get(), parameters) < 0)
            throw MediaError(MediaStatus::Corrupt, "cannot set up the decoder");
        codec_->pkt_timebase = stream_->timeBase;
        const Hardware& h = hardware();
        if (const AVPixelFormat wanted = h.device ? hardwareFormat(codec, h.type) : AV_PIX_FMT_NONE;
            wanted != AV_PIX_FMT_NONE) {
            codec_->hw_device_ctx = av_buffer_ref(h.device);
            codec_->opaque = reinterpret_cast<void*>(intptr_t(wanted));
            codec_->get_format = pickFormat;
            hardwareFormat_ = wanted;
        } else {
            codec_->thread_count = 0;
        }
        if (const int error = avcodec_open2(codec_.get(), codec, nullptr); error < 0)
            throw MediaError(MediaStatus::Unsupported, "cannot open the decoder: " + ff::describe(error));
    }

    int seek()
    {
        const auto& points = stream_->seekPoints;
        auto it = std::upper_bound(points.begin(), points.end(), std::pair{from_, INT64_MAX});
        if (it == points.begin())
            return 0;
        return av_seek_frame(format_.get(), stream_->index, std::prev(it)->second, AVSEEK_FLAG_BACKWARD);
    }

    int64_t timestampOf(const AVPacket& packet) const
    {
        return packet.pts != AV_NOPTS_VALUE ? packet.pts : packet.dts;
    }

    // The next packet of the video stream; null at the end, or on an error, which failure() then reports.
    ff::Packet nextPacket()
    {
        ff::Packet packet(av_packet_alloc());
        while (true) {
            const int error = av_read_frame(format_.get(), packet.get());
            if (error == AVERROR_EOF)
                return nullptr;
            if (error < 0) {
                failure_ = MediaError(MediaStatus::Corrupt, ff::describe(error));
                return nullptr;
            }
            if (packet->stream_index == stream_->index)
                return packet;
            av_packet_unref(packet.get());
        }
    }

    SourceRead packetNext()
    {
        ff::Packet packet = nextPacket();
        if (!packet)
            return std::nullopt;
        SourceSample sample;
        sample.index = stream_->frameIndex(timestampOf(*packet));
        sample.bytes = packet->data;
        sample.size = size_t(packet->size);
        sample.owner = std::shared_ptr<AVPacket>(packet.release(), ff::PacketFree());
        return sample;
    }

    SourceRead decodeNext()
    {
        ff::Frame frame(av_frame_alloc());
        while (true) {
            int error = avcodec_receive_frame(codec_.get(), frame.get());
            if (error == 0)
                return pictureOf(std::move(frame));
            if (error == AVERROR_EOF)
                return std::nullopt;
            if (error != AVERROR(EAGAIN)) {
                failure_ = MediaError(MediaStatus::Corrupt, ff::describe(error));
                return std::nullopt;
            }
            ff::Packet packet = nextPacket();
            if (!packet && failure_)
                return std::nullopt;
            error = avcodec_send_packet(codec_.get(), packet.get());
            if (error < 0 && error != AVERROR_EOF) {
                failure_ = MediaError(MediaStatus::Corrupt, ff::describe(error));
                return std::nullopt;
            }
        }
    }

    // A decoded frame as NV12 or P010 in system memory: copied off the GPU, kept as it is, or converted.
    SourceSample pictureOf(ff::Frame frame)
    {
        SourceSample sample;
        const int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
        sample.index = stream_->frameIndex(pts);
        if (frame->format == hardwareFormat_) {
            ff::Frame system(av_frame_alloc());
            if (av_hwframe_transfer_data(system.get(), frame.get(), 0) < 0)
                return sample;
            frame = std::move(system);
        }
        const AVPixelFormat target = stream_->deep ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
        if (frame->format != target && !(frame = converted(*frame, target)))
            return sample;
        auto picture = std::make_shared<DecodedPicture>();
        const bool deep = target == AV_PIX_FMT_P010;
        picture->planes[0] = {deep ? PlaneFormat::R16 : PlaneFormat::R8, frame->width, frame->height,
                              size_t(frame->linesize[0]), frame->data[0]};
        picture->planes[1] = {deep ? PlaneFormat::RG16 : PlaneFormat::RG8, (frame->width + 1) / 2,
                              (frame->height + 1) / 2, size_t(frame->linesize[1]), frame->data[1]};
        picture->planeCount = 2;
        picture->seconds = double(pts - stream_->start) * av_q2d(stream_->timeBase);
        picture->matrix = stream_->matrix;
        picture->fullRange = stream_->fullRange;
        picture->storage = std::shared_ptr<AVFrame>(frame.release(), ff::FrameFree());
        sample.picture = std::move(picture);
        return sample;
    }

    // The picture in `target`, in the stream's own range and matrix; null when it cannot be converted.
    ff::Frame converted(const AVFrame& frame, AVPixelFormat target)
    {
        scale_.reset(sws_getCachedContext(scale_.release(), frame.width, frame.height, AVPixelFormat(frame.format),
                                          frame.width, frame.height, target, SWS_BILINEAR, nullptr, nullptr, nullptr));
        ff::Frame out(av_frame_alloc());
        if (!scale_ || !out)
            return nullptr;
        const int range = stream_->fullRange ? 1 : 0;
        const int* coefficients = sws_getCoefficients(SWS_CS_DEFAULT);
        sws_setColorspaceDetails(scale_.get(), coefficients, range, coefficients, range, 0, 1 << 16, 1 << 16);
        out->format = target;
        out->width = frame.width;
        out->height = frame.height;
        if (av_frame_get_buffer(out.get(), 0) < 0 ||
            sws_scale(scale_.get(), frame.data, frame.linesize, 0, frame.height, out->data, out->linesize) <= 0)
            return nullptr;
        return out;
    }

    struct Live {
        Live() { ++gLiveReaders; }
        ~Live() { --gLiveReaders; }
    };

    Live live_;
    std::shared_ptr<const Stream> stream_;
    int from_ = 0;
    ff::Format format_;
    ff::Codec codec_;
    ff::Scale scale_;
    AVPixelFormat hardwareFormat_ = AV_PIX_FMT_NONE;
    std::optional<MediaError> failure_;
};

class FfmpegSource final : public VideoSource {
public:
    // Without a device frames keep their pictures in system memory.
    FfmpegSource(const MediaDevice* device, const std::string& path) : device_(device ? *device : gpu::Device{})
    {
        auto stream = std::make_shared<Stream>();
        stream->path = std::filesystem::path(path);
        int error = 0;
        const ff::Format format = ff::openFormat(stream->path, error);
        if (!format)
            throw MediaError(openFailureStatus(stream->path), "cannot open: " + ff::describe(error));
        // The pixel format and the colour a stream's own headers carry (HEVC, ProRes) are known once a frame is read.
        if ((error = avformat_find_stream_info(format.get(), nullptr)) < 0)
            throw MediaError(MediaStatus::Corrupt, "cannot read the stream: " + ff::describe(error));
        stream->index = ff::videoStream(format.get());
        if (stream->index < 0)
            throw MediaError(MediaStatus::Unsupported, "no video track");
        AVStream* video = format->streams[stream->index];
        const AVCodecParameters* parameters = video->codecpar;
        stream->timeBase = video->time_base;
        stream->start = video->start_time != AV_NOPTS_VALUE ? video->start_time : 0;
        stream->hap = hapCodecOf(ff::fourccOfTag(parameters->codec_tag));
        AVRational rate = video->avg_frame_rate.num > 0 ? video->avg_frame_rate : video->r_frame_rate;
        stream->fps = rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 0.0;
        if (!(stream->fps > 0.0))
            throw MediaError(MediaStatus::Unsupported, "no frame rate");
        const double duration = video->duration != AV_NOPTS_VALUE ? double(video->duration) * av_q2d(video->time_base)
                                                                    : double(format->duration) / AV_TIME_BASE;
        track_.frameCount = int(std::llround(duration * stream->fps));
        if (track_.frameCount < 1)
            throw MediaError(MediaStatus::Corrupt, "no frames");
        if (indexEnd(video) > std::filesystem::file_size(stream->path))
            throw MediaError(MediaStatus::Corrupt, "truncated: the file is smaller than the samples its index lists");
        if (!pictureSizeOk(parameters->width, parameters->height))
            throw MediaError(MediaStatus::Unsupported, "a picture of " + std::to_string(parameters->width) + "x" +
                                                           std::to_string(parameters->height));
        stream->deep = deepOf(parameters);
        stream->matrix = matrixOf(parameters);
        stream->fullRange = fullRangeOf(parameters);
        track_.fps = stream->fps;
        track_.width = parameters->width;
        track_.height = parameters->height;
        track_.turns = turnsOf(video);
        track_.hap = stream->hap;
        track_.pictureBytes = int64_t(track_.width) * track_.height * 3 / 2 * (stream->deep ? 2 : 1);
        indexKeyframes(*format, *stream);
        yuvToRgb_ = yuvToRgbRows(stream->matrix, stream->fullRange);
        stream_ = std::move(stream);
    }

    const SourceTrack& track() const override { return track_; }

    std::shared_ptr<SourceReader> startReading(int fromFrame) override
    {
        return std::make_shared<FfmpegReader>(stream_, fromFrame);
    }

    VideoFrameRef frame(const std::shared_ptr<const void>& picture, int index) override
    {
        auto frame = std::make_shared<VideoFrame>();
        frame->index = index;
        frame->yuvToRgb = yuvToRgb_;
        frame->turns = track_.turns;
        return withPicture(std::move(frame), std::static_pointer_cast<const DecodedPicture>(picture));
    }

    VideoFrameRef hapFrame(const std::vector<HapImage>& images, PictureFormat format, int index) override
    {
        auto held = std::make_shared<std::vector<HapImage>>(images);
        auto picture = std::make_shared<DecodedPicture>();
        for (const HapImage& image : *held) {
            const PlaneFormat plane = image.texture == HapTexture::BC1   ? PlaneFormat::BC1
                                      : image.texture == HapTexture::BC4 ? PlaneFormat::BC4
                                                                         : PlaneFormat::BC3;
            const size_t rows = size_t(track_.height + 3) / 4;
            picture->planes[size_t(picture->planeCount++)] = {plane, track_.width, track_.height,
                                                              image.blocks.size() / rows, image.blocks.data()};
        }
        picture->storage = std::move(held);
        auto frame = std::make_shared<VideoFrame>();
        frame->index = index;
        frame->format = format;
        frame->turns = track_.turns;
        return withPicture(std::move(frame), std::move(picture));
    }

private:
    // With a device the picture becomes the frame's textures here, on the decode thread, and its memory is freed; a
    // single plane (HAP without alpha) serves as both. Null when the device cannot make them.
    VideoFrameRef withPicture(std::shared_ptr<VideoFrame> frame, std::shared_ptr<const DecodedPicture> picture) const
    {
        if (!device_) {
            frame->picture = std::move(picture);
            return frame;
        }
        auto textures = std::make_shared<std::array<gpu::Texture, 2>>(uploadPicture(device_, *picture));
        if (!(*textures)[0] || (picture->planeCount > 1 && !(*textures)[1]))
            return nullptr;
        frame->luma = (*textures)[0];
        frame->chroma = (*textures)[1] ? (*textures)[1] : (*textures)[0];
        frame->resources = std::move(textures);
        return frame;
    }

    // Where the last sample the index lists ends.
    static uintmax_t indexEnd(AVStream* video)
    {
        uintmax_t end = 0;
        for (int i = 0; i < avformat_index_get_entries_count(video); ++i) {
            const AVIndexEntry* entry = avformat_index_get_entry(video, i);
            if (entry->pos >= 0)
                end = std::max(end, uintmax_t(entry->pos) + uintmax_t(entry->size));
        }
        return end;
    }

    // Keyframes from the index. Its timestamps are decode times: a keyframe shows the reorder delay later, which the
    // first packet tells.
    void indexKeyframes(AVFormatContext& format, Stream& stream)
    {
        AVStream* video = format.streams[stream.index];
        int64_t delay = 0;
        AVPacket* packet = av_packet_alloc();
        while (av_read_frame(&format, packet) >= 0) {
            const bool ours = packet->stream_index == stream.index;
            if (ours && packet->pts != AV_NOPTS_VALUE && packet->dts != AV_NOPTS_VALUE)
                delay = packet->pts - packet->dts;
            av_packet_unref(packet);
            if (ours)
                break;
        }
        av_packet_free(&packet);
        std::vector<int>& keys = track_.keyframes;
        keys = {0};
        for (int i = 0; i < avformat_index_get_entries_count(video); ++i) {
            const AVIndexEntry* entry = avformat_index_get_entry(video, i);
            if (!(entry->flags & AVINDEX_KEYFRAME))
                continue;
            const int index = stream.frameIndex(entry->timestamp + delay);
            stream.seekPoints.emplace_back(std::max(0, index), entry->timestamp);
            if (index > 0 && index < track_.frameCount)
                keys.push_back(index);
        }
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        std::sort(stream.seekPoints.begin(), stream.seekPoints.end());
    }

    std::shared_ptr<const Stream> stream_;
    SourceTrack track_;
    std::array<float, 12> yuvToRgb_{};
    gpu::Device device_;
};

} // namespace

int64_t liveAssetReaders()
{
    return gLiveReaders.load();
}

std::unique_ptr<VideoSource> openVideoSource(const MediaDevice* device, const std::string& path)
{
    return std::make_unique<FfmpegSource>(device, path);
}

void runRead(std::function<void()> read)
{
    std::thread(std::move(read)).detach();
}

// Decode threads keep the default priority, below the frame threads.
void becomeDecodeThread() {}

FrameworkScope::FrameworkScope() = default;

FrameworkScope::~FrameworkScope() = default;

} // namespace azl
