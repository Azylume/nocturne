#include "output/movie_encoder_ffmpeg.hpp"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace azl::recording {

namespace {

// Bounds the pictures between the render thread and the file: those being filled, queued and held by the encoder.
constexpr int kPictures = 8;
constexpr AVRational kTimeBase{1, 90000};
// A movie cut off before it is finished still plays up to its last fragment.
constexpr const char* kFragmentMicros = "2000000";

struct Candidate {
    const char* name;
    YCbCrLayout layout;
    bool hardware;
    // Private options of the encoder.
    std::vector<std::pair<const char*, const char*>> options;
};

// In the order they are tried. Without NVENC, Media Foundation's encoder: the GPU's own (hardware) where the driver
// has one, else Microsoft's software one where the HEVC extension is installed.
std::vector<Candidate> candidates(const std::string& codec, bool precise)
{
    if (codec == "prores")
        return {{"prores_ks", YCbCrLayout::Planar422P10, false, {{"profile", "standard"}, {"vendor", "apl0"}}}};
    const YCbCrLayout layout = precise ? YCbCrLayout::P010 : YCbCrLayout::NV12;
    const char* profile = precise ? "main10" : "main";
    std::vector<Candidate> list{
        {"hevc_nvenc", layout, true,
         {{"preset", "p4"}, {"profile", profile}, {"rc", "vbr"}, {"delay", "0"}, {"zerolatency", "1"},
          {"forced-idr", "1"}}},
        {"hevc_mf", layout, true, {{"hw_encoding", "1"}}},
    };
    if (precise)
        list.push_back({"hevc_mf", YCbCrLayout::NV12, true, {{"hw_encoding", "1"}}});
    list.push_back({"hevc_mf", YCbCrLayout::NV12, false, {}});
    return list;
}

ff::Codec openEncoder(const Candidate& candidate, uint32_t width, uint32_t height, double fps)
{
    const AVCodec* codec = avcodec_find_encoder_by_name(candidate.name);
    if (!codec)
        return {};
    ff::Codec context(avcodec_alloc_context3(codec));
    if (!context)
        return {};
    AVCodecContext* c = context.get();
    c->width = int(width);
    c->height = int(height);
    c->pix_fmt = ff::pixelFormatOf(candidate.layout);
    c->time_base = kTimeBase;
    c->framerate = av_d2q(fps, 100000);
    c->color_primaries = AVCOL_PRI_BT709;
    c->color_trc = AVCOL_TRC_BT709;
    c->colorspace = AVCOL_SPC_BT709;
    c->color_range = AVCOL_RANGE_MPEG;
    c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    c->max_b_frames = 0;
    if (codec->id == AV_CODEC_ID_HEVC) {
        c->bit_rate = int64_t(double(width) * height * fps * 0.2);
        c->gop_size = std::max(1, int(std::lround(fps * 2.0)));
    } else {
        c->thread_type = FF_THREAD_SLICE;
        c->thread_count = 0;
    }
    for (const auto& [key, value] : candidate.options)
        av_opt_set(c->priv_data, key, value, 0);
    if (avcodec_open2(c, codec, nullptr) < 0)
        return {};
    return context;
}

struct OutputFree {
    void operator()(AVFormatContext* f) const
    {
        if (f->pb)
            avio_closep(&f->pb);
        avformat_free_context(f);
    }
};

// One movie file: a QuickTime muxer fragmented every 2 s, taking the recording encoder's packets as they are and
// encoding its own AAC track.
class FfmpegMovie final : public MovieFile {
public:
    FfmpegMovie(const std::filesystem::path& partial, const AVCodecParameters* video, AVRational timeBase,
                double audioRate)
    {
        const std::u8string path = partial.u8string();
        const char* name = reinterpret_cast<const char*>(path.c_str());
        AVFormatContext* raw = nullptr;
        if (avformat_alloc_output_context2(&raw, nullptr, "mov", name) < 0)
            throw std::runtime_error("cannot write " + partial.string());
        format_.reset(raw);
        video_ = avformat_new_stream(raw, nullptr);
        if (!video_ || avcodec_parameters_copy(video_->codecpar, video) < 0)
            throw std::runtime_error("cannot write " + partial.string() + ": no video track");
        video_->time_base = timeBase;
        // QuickTime plays HEVC tagged 'hvc1', with its parameter sets in the track's header.
        if (video->codec_id == AV_CODEC_ID_HEVC && video->extradata_size > 0)
            video_->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
        if (audioRate > 0.0)
            openAudio(int(audioRate));
        // Each fragment reaches the disk whole as soon as it is written, so a killed engine leaves it playable.
        raw->flush_packets = 1;
        int error = avio_open(&raw->pb, name, AVIO_FLAG_WRITE);
        if (error < 0)
            throw std::runtime_error("cannot write " + partial.string() + ": " + ff::describe(error));
        AVDictionary* options = nullptr;
        av_dict_set(&options, "frag_duration", kFragmentMicros, 0);
        // The moov waits for the first fragment, so its edit lists can trim the AAC priming instead of the muxer
        // delaying every track by it.
        av_dict_set(&options, "movflags", "+delay_moov", 0);
        error = avformat_write_header(raw, &options);
        av_dict_free(&options);
        if (error < 0)
            throw std::runtime_error("cannot start the movie: " + ff::describe(error));
        bytes_ = uint64_t(std::max<int64_t>(avio_tell(raw->pb), 0));
    }

    bool hasAudio() const override { return audio_ != nullptr; }
    bool audioReady() const override { return true; }

    // Silence fills a gap before `at`, so the track stays continuous on the frames' clock.
    bool appendAudio(const float* samples, size_t count, int64_t at) override
    {
        std::lock_guard lock(mutex_);
        if (!writingLocked())
            return false;
        if (at > audioNext_)
            queued_.insert(queued_.end(), size_t(at - audioNext_), 0.0f);
        queued_.insert(queued_.end(), samples, samples + count);
        audioNext_ = at + int64_t(count);
        const auto frameSize = size_t(aac_->frame_size);
        size_t used = 0;
        for (; queued_.size() - used >= frameSize; used += frameSize)
            if (!encodeAudio(queued_.data() + used, frameSize))
                return false;
        queued_.erase(queued_.begin(), queued_.begin() + ptrdiff_t(used));
        return true;
    }

    bool finish() override
    {
        std::unique_lock lock(mutex_);
        drained_.wait(lock, [&] { return pending_ == 0; });
        if (!writingLocked())
            return false;
        if (audio_ && ((!queued_.empty() && !encodeAudio(queued_.data(), queued_.size())) || !encodeAudio(nullptr, 0)))
            return false;
        int error = av_write_trailer(format_.get());
        if (error >= 0)
            error = avio_closep(&format_->pb);
        if (error < 0)
            return fail(error);
        finished_ = true;
        return true;
    }

    void cancel() override
    {
        std::lock_guard lock(mutex_);
        closeFile();
    }

    bool writing() const override
    {
        std::lock_guard lock(mutex_);
        return writingLocked();
    }

    bool diskFull() const override
    {
        std::lock_guard lock(mutex_);
        return error_ == AVERROR(ENOSPC);
    }

    std::string error() const override
    {
        std::lock_guard lock(mutex_);
        return error_ < 0 ? ff::describe(error_) : "unknown error";
    }

    std::optional<uint64_t> bytes() const override { return bytes_.load(); }

    // A frame is on its way to this file; finish() waits for it.
    void hold()
    {
        std::lock_guard lock(mutex_);
        pending_++;
    }

    // The frame held for this file arrived (`packet`) or was lost (null). False when the write failed.
    bool arrived(AVPacket* packet, AVRational timeBase)
    {
        std::lock_guard lock(mutex_);
        const bool ok = !packet || (writingLocked() && write(packet, video_, timeBase));
        pending_--;
        drained_.notify_all();
        return ok;
    }

private:
    void openAudio(int rate)
    {
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        ff::Codec aac(codec ? avcodec_alloc_context3(codec) : nullptr);
        if (!aac)
            return;
        aac->sample_fmt = AV_SAMPLE_FMT_FLTP;
        aac->sample_rate = rate;
        av_channel_layout_default(&aac->ch_layout, 1);
        aac->bit_rate = 128000;
        aac->time_base = {1, rate};
        aac->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        ff::Frame frame(av_frame_alloc());
        if (!frame || avcodec_open2(aac.get(), codec, nullptr) < 0)
            return;
        frame->format = aac->sample_fmt;
        frame->sample_rate = rate;
        frame->nb_samples = aac->frame_size;
        av_channel_layout_copy(&frame->ch_layout, &aac->ch_layout);
        AVStream* stream = avformat_new_stream(format_.get(), nullptr);
        if (av_frame_get_buffer(frame.get(), 0) < 0 || !stream ||
            avcodec_parameters_from_context(stream->codecpar, aac.get()) < 0)
            return;
        stream->time_base = aac->time_base;
        audio_ = stream;
        aac_ = std::move(aac);
        audioFrame_ = std::move(frame);
        audioPacket_.reset(av_packet_alloc());
    }

    // Under mutex_. Encodes `count` samples, or with none drains the encoder at the end. A failure closes the file.
    bool encodeAudio(const float* samples, size_t count)
    {
        AVFrame* frame = nullptr;
        if (samples) {
            frame = audioFrame_.get();
            if (const int error = av_frame_make_writable(frame); error < 0)
                return fail(error);
            frame->nb_samples = int(count);
            std::memcpy(frame->data[0], samples, count * sizeof(float));
            frame->pts = audioPts_;
            audioPts_ += int64_t(count);
        }
        if (const int error = avcodec_send_frame(aac_.get(), frame); error < 0)
            return fail(error);
        for (;;) {
            const int error = avcodec_receive_packet(aac_.get(), audioPacket_.get());
            if (error == AVERROR(EAGAIN) || error == AVERROR_EOF)
                return true;
            if (error < 0)
                return fail(error);
            if (!write(audioPacket_.get(), audio_, aac_->time_base))
                return false;
        }
    }

    // Under mutex_.
    bool write(AVPacket* packet, AVStream* stream, AVRational from)
    {
        packet->stream_index = stream->index;
        av_packet_rescale_ts(packet, from, stream->time_base);
        const int size = packet->size;
        if (const int error = av_interleaved_write_frame(format_.get(), packet); error < 0)
            return fail(error);
        bytes_ += uint64_t(size);
        return true;
    }

    // Under mutex_. The file is closed at once, so it can be removed. Always false.
    bool fail(int error)
    {
        error_ = error;
        closeFile();
        return false;
    }

    void closeFile()
    {
        if (format_ && format_->pb)
            avio_closep(&format_->pb);
    }

    bool writingLocked() const { return format_ && format_->pb && error_ == 0 && !finished_; }

    mutable std::mutex mutex_;
    std::condition_variable drained_;
    int pending_ = 0;
    std::unique_ptr<AVFormatContext, OutputFree> format_;
    AVStream* video_ = nullptr;
    AVStream* audio_ = nullptr;
    ff::Codec aac_;
    ff::Frame audioFrame_;
    ff::Packet audioPacket_;
    // Samples waiting for a whole AAC frame, and the movie sample the next one handed in lands at.
    std::vector<float> queued_;
    int64_t audioNext_ = 0;
    int64_t audioPts_ = 0;
    int error_ = 0;
    bool finished_ = false;
    std::atomic<uint64_t> bytes_{0};
};

} // namespace

FfmpegEncoder::FfmpegEncoder(const std::string& codec, uint32_t width, uint32_t height, double fps, bool precise,
                             Counts& counts, const std::string& only)
    : counts_(counts), packet_(av_packet_alloc())
{
    checkCodec(codec);
    for (const Candidate& candidate : candidates(codec, precise)) {
        if (!only.empty() && only != candidate.name)
            continue;
        if ((codec_ = openEncoder(candidate, width, height, fps))) {
            layout_ = candidate.layout;
            hardware_ = candidate.hardware;
            break;
        }
    }
    if (!codec_)
        throw std::runtime_error("this machine cannot encode " + codec + " at " + std::to_string(width) + "x" +
                                 std::to_string(height));
    parameters_.reset(avcodec_parameters_alloc());
    if (!parameters_ || avcodec_parameters_from_context(parameters_.get(), codec_.get()) < 0)
        throw std::runtime_error("cannot set up the " + codec + " encoder");
    pool_ = std::make_unique<ff::PicturePool>(layout_, width, height, kPictures);
}

FfmpegEncoder::~FfmpegEncoder() = default;

std::string FfmpegEncoder::name() const
{
    return codec_->codec->name;
}

std::unique_ptr<MovieFile> FfmpegEncoder::open(const std::filesystem::path& partial, double audioRate)
{
    return std::make_unique<FfmpegMovie>(partial, parameters_.get(), kTimeBase, audioRate);
}

bool FfmpegEncoder::encode(const Picture& picture, double seconds, double origin, bool key, MovieFile& file)
{
    AVFrame* frame = ff::PicturePool::frameOf(*static_cast<const YCbCrPicture*>(picture.get()));
    frame->pts = std::llround(seconds * kTimeBase.den);
    frame->duration = std::llround(kTimeBase.den / av_q2d(codec_->framerate));
    frame->pict_type = key ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    frame->color_primaries = codec_->color_primaries;
    frame->color_trc = codec_->color_trc;
    frame->colorspace = codec_->colorspace;
    frame->color_range = codec_->color_range;
    auto& movie = static_cast<FfmpegMovie&>(file);
    movie.hold();
    routes_.push_back({frame->pts, &file, std::llround(origin * kTimeBase.den)});
    if (avcodec_send_frame(codec_.get(), frame) < 0) {
        routes_.pop_back();
        movie.arrived(nullptr, kTimeBase);
        return false;
    }
    return drain() && !counts_.failed;
}

void FfmpegEncoder::complete()
{
    if (avcodec_send_frame(codec_.get(), nullptr) >= 0)
        drain();
    for (const Route& route : routes_)
        lost(route);
    routes_.clear();
}

// Writes every packet the encoder has ready into the file its frame was handed for. A frame the encoder skipped
// counts as dropped.
bool FfmpegEncoder::drain()
{
    for (;;) {
        const int error = avcodec_receive_packet(codec_.get(), packet_.get());
        if (error == AVERROR(EAGAIN) || error == AVERROR_EOF)
            return true;
        if (error < 0)
            return false;
        while (!routes_.empty() && routes_.front().pts < packet_->pts) {
            lost(routes_.front());
            routes_.pop_front();
        }
        if (routes_.empty() || routes_.front().pts != packet_->pts) {
            av_packet_unref(packet_.get());
            continue;
        }
        const Route route = routes_.front();
        routes_.pop_front();
        packet_->pts -= route.origin;
        packet_->dts -= route.origin;
        if (packet_->duration <= 0)
            packet_->duration = std::llround(kTimeBase.den / av_q2d(codec_->framerate));
        if (static_cast<FfmpegMovie*>(route.file)->arrived(packet_.get(), codec_->time_base))
            counts_.frames++;
        else
            counts_.failed = true;
        av_packet_unref(packet_.get());
    }
}

void FfmpegEncoder::lost(const Route& route)
{
    counts_.dropped++;
    static_cast<FfmpegMovie*>(route.file)->arrived(nullptr, kTimeBase);
}

bool moviePlays(const std::filesystem::path& file)
{
    int error = 0;
    const ff::Format format = ff::openFormat(file, error);
    if (!format || avformat_find_stream_info(format.get(), nullptr) < 0)
        return false;
    const int stream = ff::videoStream(format.get());
    ff::Packet packet(av_packet_alloc());
    if (stream < 0 || !packet)
        return false;
    while (av_read_frame(format.get(), packet.get()) >= 0) {
        const bool video = packet->stream_index == stream;
        av_packet_unref(packet.get());
        if (video)
            return true;
    }
    return false;
}

} // namespace azl::recording
