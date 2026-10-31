#include "media/hap_movie.hpp"

#include "media/ffmpeg_util.hpp"
#include "media/hap.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace azl {

namespace {

struct Track {
    ff::Format format;
    AVStream* stream = nullptr;
    HapCodec codec = HapCodec::None;
};

// The file's video track and its HAP codec (None for any other track); no format when the file does not open.
Track openTrack(const std::filesystem::path& file)
{
    Track t;
    int error = 0;
    t.format = ff::openFormat(file, error);
    if (!t.format)
        return t;
    const int index = ff::videoStream(t.format.get());
    if (index < 0)
        return t;
    t.stream = t.format->streams[index];
    t.codec = hapCodecOf(ff::fourccOfTag(t.stream->codecpar->codec_tag));
    return t;
}

// The next packet of the track as stored; null at the end.
ff::Packet nextPacket(const Track& t)
{
    ff::Packet packet(av_packet_alloc());
    while (av_read_frame(t.format.get(), packet.get()) >= 0) {
        if (packet->stream_index == t.stream->index)
            return packet;
        av_packet_unref(packet.get());
    }
    return nullptr;
}

} // namespace

// Writes the frames as the movie's samples with the QuickTime muxer: a track of the HAP fourcc, the same sample layout
// AVAssetWriter makes on Apple, so a movie written on one platform plays on the other.
struct HapMovieWriter::Impl {
    AVFormatContext* format = nullptr;
    AVStream* stream = nullptr;
    AVRational frameTime{1, 30};
    int64_t frames = 0;
    bool finished = false;
    std::filesystem::path file;

    ~Impl()
    {
        if (!format)
            return;
        if (format->pb)
            avio_closep(&format->pb);
        avformat_free_context(format);
        if (!finished) {
            std::error_code ignored;
            std::filesystem::remove(file, ignored);
        }
    }
};

HapMovieWriter::HapMovieWriter(const std::filesystem::path& file, HapCodec codec, int width, int height, int fpsNum,
                               int fpsDen)
    : impl_(std::make_unique<Impl>())
{
    if (codec == HapCodec::None || width <= 0 || height <= 0 || fpsNum <= 0 || fpsDen <= 0)
        throw std::runtime_error("invalid HAP movie settings");
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
    impl_->file = file;
    const std::u8string path = file.u8string();
    const char* name = reinterpret_cast<const char*>(path.c_str());
    if (avformat_alloc_output_context2(&impl_->format, nullptr, "mov", name) < 0)
        throw std::runtime_error("cannot write " + file.string());
    impl_->stream = avformat_new_stream(impl_->format, nullptr);
    if (!impl_->stream)
        throw std::runtime_error("the movie cannot take a HAP track");
    AVCodecParameters* parameters = impl_->stream->codecpar;
    parameters->codec_type = AVMEDIA_TYPE_VIDEO;
    parameters->codec_id = AV_CODEC_ID_HAP;
    parameters->codec_tag = ff::fourccOfTag(fourccOf(codec));
    parameters->width = width;
    parameters->height = height;
    parameters->bits_per_coded_sample = codec == HapCodec::HapAlpha || codec == HapCodec::HapQAlpha ? 32 : 24;
    impl_->frameTime = {fpsDen, fpsNum};
    impl_->stream->time_base = impl_->frameTime;
    impl_->stream->avg_frame_rate = {fpsNum, fpsDen};
    int error = avio_open(&impl_->format->pb, name, AVIO_FLAG_WRITE);
    if (error >= 0)
        error = avformat_write_header(impl_->format, nullptr);
    if (error < 0)
        throw std::runtime_error("cannot start writing " + file.string() + ": " + ff::describe(error));
}

HapMovieWriter::~HapMovieWriter() = default;

void HapMovieWriter::append(const std::vector<uint8_t>& frame)
{
    ff::Packet packet(av_packet_alloc());
    if (!packet || av_new_packet(packet.get(), int(frame.size())) < 0)
        throw std::runtime_error("cannot buffer a HAP frame");
    std::copy(frame.begin(), frame.end(), packet->data);
    packet->stream_index = impl_->stream->index;
    packet->pts = packet->dts = impl_->frames;
    packet->duration = 1;
    packet->flags |= AV_PKT_FLAG_KEY;
    av_packet_rescale_ts(packet.get(), impl_->frameTime, impl_->stream->time_base);
    if (const int error = av_write_frame(impl_->format, packet.get()); error < 0)
        throw std::runtime_error("cannot write a HAP frame: " + ff::describe(error));
    ++impl_->frames;
}

void HapMovieWriter::finish()
{
    const int error = av_write_trailer(impl_->format);
    avio_closep(&impl_->format->pb);
    impl_->finished = error >= 0;
    if (error < 0)
        throw std::runtime_error("cannot finish the movie: " + ff::describe(error));
}

HapCodec hapCodecOfFile(const std::filesystem::path& file)
{
    return openTrack(file).codec;
}

std::vector<uint8_t> readHapPicture(const std::filesystem::path& file, double fraction, int& width, int& height)
{
    const Track t = openTrack(file);
    if (t.codec == HapCodec::None)
        return {};
    const AVCodecParameters* parameters = t.stream->codecpar;
    if (!pictureSizeOk(parameters->width, parameters->height))
        return {};
    const int64_t start = t.stream->start_time != AV_NOPTS_VALUE ? t.stream->start_time : 0;
    const int64_t duration = t.stream->duration != AV_NOPTS_VALUE ? t.stream->duration : 0;
    const auto at = start + int64_t(double(duration) * std::clamp(fraction, 0.0, 0.999));
    av_seek_frame(t.format.get(), t.stream->index, at, AVSEEK_FLAG_BACKWARD);
    const ff::Packet packet = nextPacket(t);
    if (!packet)
        return {};
    std::vector<HapImage> images;
    if (!decodeHapFrame(packet->data, size_t(packet->size), parameters->width, parameters->height, images).empty())
        return {};
    width = parameters->width;
    height = parameters->height;
    return hapToRgba(images, width, height);
}

std::vector<std::vector<uint8_t>> readHapFrames(const std::filesystem::path& file, int count)
{
    const Track t = openTrack(file);
    if (!t.format)
        throw std::runtime_error("cannot read " + file.string());
    if (!t.stream)
        throw std::runtime_error(file.string() + " has no video track");
    std::vector<std::vector<uint8_t>> frames;
    while (int(frames.size()) < count) {
        const ff::Packet packet = nextPacket(t);
        if (!packet)
            break;
        frames.emplace_back(packet->data, packet->data + packet->size);
    }
    return frames;
}

} // namespace azl
