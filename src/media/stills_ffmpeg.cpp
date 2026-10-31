#include "media/stills.hpp"

#include "media/ffmpeg_util.hpp"
#include "media/hap_movie.hpp"

extern "C" {
#include <libavutil/display.h>
}

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wall"
#pragma clang diagnostic ignored "-Wextra"
#endif
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../third_party/stb/stb_image_write.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace azl {

namespace {

// Straight-alpha RGBA pixels, rows packed.
struct Picture {
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
};

Picture scaled(const Picture& in, int width, int height)
{
    if (in.width == width && in.height == height)
        return in;
    Picture out{std::vector<uint8_t>(size_t(width) * height * 4), width, height};
    const ff::Scale scale(sws_getContext(in.width, in.height, AV_PIX_FMT_RGBA, width, height, AV_PIX_FMT_RGBA,
                                         SWS_AREA, nullptr, nullptr, nullptr));
    const uint8_t* source[] = {in.rgba.data()};
    const int sourceStride[] = {in.width * 4};
    uint8_t* target[] = {out.rgba.data()};
    const int targetStride[] = {width * 4};
    if (!scale || sws_scale(scale.get(), source, sourceStride, 0, in.height, target, targetStride) <= 0)
        return {};
    return out;
}

// Clockwise quarter turns.
Picture turned(const Picture& in, int turns)
{
    if (turns == 0)
        return in;
    const bool sideways = turns % 2 == 1;
    Picture out{std::vector<uint8_t>(in.rgba.size()), sideways ? in.height : in.width, sideways ? in.width : in.height};
    for (int y = 0; y < in.height; ++y)
        for (int x = 0; x < in.width; ++x) {
            int tx = x, ty = y;
            if (turns == 1)
                tx = in.height - 1 - y, ty = x;
            else if (turns == 2)
                tx = in.width - 1 - x, ty = in.height - 1 - y;
            else
                tx = y, ty = in.width - 1 - x;
            std::copy_n(&in.rgba[(size_t(y) * in.width + x) * 4], 4, &out.rgba[(size_t(ty) * out.width + tx) * 4]);
        }
    return out;
}

// The pixels premultiplied by their alpha, as BGRA (CoreGraphics' kCGImageAlphaPremultipliedFirst, little-endian).
std::vector<uint8_t> premultipliedBgra(const uint8_t* rgba, size_t pixels)
{
    std::vector<uint8_t> out(pixels * 4);
    for (size_t i = 0; i < pixels; ++i) {
        const uint8_t* p = rgba + i * 4;
        const int a = p[3];
        uint8_t* q = &out[i * 4];
        q[0] = uint8_t((p[2] * a + 127) / 255);
        q[1] = uint8_t((p[1] * a + 127) / 255);
        q[2] = uint8_t((p[0] * a + 127) / 255);
        q[3] = uint8_t(a);
    }
    return out;
}

// Decodes single pictures of a video on the CPU, at points in its time.
class Grabber {
public:
    explicit Grabber(const std::filesystem::path& file)
    {
        int error = 0;
        format_ = ff::openFormat(file, error);
        const int index = format_ ? ff::videoStream(format_.get()) : -1;
        if (index < 0)
            throw std::runtime_error("no video in " + file.string());
        stream_ = format_->streams[index];
        const AVCodec* codec = avcodec_find_decoder(stream_->codecpar->codec_id);
        codec_.reset(codec ? avcodec_alloc_context3(codec) : nullptr);
        if (!codec_ || avcodec_parameters_to_context(codec_.get(), stream_->codecpar) < 0)
            throw std::runtime_error("no decoder for the video in " + file.string());
        codec_->pkt_timebase = stream_->time_base;
        codec_->thread_count = 0;
        if (avcodec_open2(codec_.get(), codec, nullptr) < 0)
            throw std::runtime_error("cannot decode the video in " + file.string());
        start_ = stream_->start_time != AV_NOPTS_VALUE ? stream_->start_time : 0;
        duration_ = stream_->duration != AV_NOPTS_VALUE ? double(stream_->duration) * av_q2d(stream_->time_base)
                                                        : double(format_->duration) / AV_TIME_BASE;
        if (!(duration_ > 0.0))
            throw std::runtime_error("no video in " + file.string());
        const AVPacketSideData* side = av_packet_side_data_get(stream_->codecpar->coded_side_data,
                                                               stream_->codecpar->nb_coded_side_data,
                                                               AV_PKT_DATA_DISPLAYMATRIX);
        if (side && side->size >= sizeof(int32_t) * 9) {
            const double counterclockwise = av_display_rotation_get(reinterpret_cast<const int32_t*>(side->data));
            turns_ = std::isnan(counterclockwise) ? 0 : int(std::lround(-counterclockwise / 90.0)) & 3;
        }
    }

    double duration() const { return duration_; }
    int turns() const { return turns_; }
    // The stored size, before the turns.
    int width() const { return stream_->codecpar->width; }
    int height() const { return stream_->codecpar->height; }

    // The first frame at most `tolerance` seconds before `seconds` or later, decoded from the keyframe before it, at
    // `width` x `height` as stored (the caller turns it); empty when there is none.
    Picture grab(double seconds, double tolerance, int width, int height)
    {
        const double base = av_q2d(stream_->time_base);
        avcodec_flush_buffers(codec_.get());
        av_seek_frame(format_.get(), stream_->index, start_ + int64_t(seconds / base), AVSEEK_FLAG_BACKWARD);
        ff::Packet packet(av_packet_alloc());
        ff::Frame frame(av_frame_alloc());
        bool draining = false;
        while (true) {
            const int received = avcodec_receive_frame(codec_.get(), frame.get());
            if (received == 0) {
                const int64_t pts = frame->best_effort_timestamp;
                if (pts == AV_NOPTS_VALUE || double(pts - start_) * base >= seconds - tolerance)
                    return converted(*frame, width, height);
                continue;
            }
            if (received != AVERROR(EAGAIN) || draining)
                return {};
            const int read = av_read_frame(format_.get(), packet.get());
            if (read < 0) {
                draining = true;
                avcodec_send_packet(codec_.get(), nullptr);
            } else if (packet->stream_index == stream_->index) {
                avcodec_send_packet(codec_.get(), packet.get());
            }
            av_packet_unref(packet.get());
        }
    }

private:
    Picture converted(const AVFrame& frame, int width, int height)
    {
        scale_.reset(sws_getCachedContext(scale_.release(), frame.width, frame.height, AVPixelFormat(frame.format),
                                          width, height, AV_PIX_FMT_RGBA, SWS_AREA, nullptr, nullptr, nullptr));
        if (!scale_)
            return {};
        AVColorSpace space = frame.colorspace;
        if (space == AVCOL_SPC_UNSPECIFIED || space == AVCOL_SPC_RGB || space == AVCOL_SPC_RESERVED)
            space = frame.height > 576 ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
        const int* coefficients = sws_getCoefficients(space);
        sws_setColorspaceDetails(scale_.get(), coefficients, frame.color_range == AVCOL_RANGE_JPEG, coefficients, 1, 0,
                                 1 << 16, 1 << 16);
        Picture out{std::vector<uint8_t>(size_t(width) * height * 4), width, height};
        uint8_t* target[] = {out.rgba.data()};
        const int stride[] = {width * 4};
        if (sws_scale(scale_.get(), frame.data, frame.linesize, 0, frame.height, target, stride) <= 0)
            return {};
        return out;
    }

    ff::Format format_;
    AVStream* stream_ = nullptr;
    ff::Codec codec_;
    ff::Scale scale_;
    int64_t start_ = 0;
    double duration_ = 0.0;
    int turns_ = 0;
};

// A HAP movie's frame `fraction` of the way in, decoded on the CPU, scaled to fit `maxWidth`; empty for any other file.
Picture hapPicture(const std::filesystem::path& file, double fraction, int maxWidth)
{
    Picture full;
    full.rgba = readHapPicture(file, fraction, full.width, full.height);
    if (full.rgba.empty())
        return {};
    const double scale = std::min(1.0, double(maxWidth) / full.width);
    return scaled(full, std::max(1, int(std::lround(full.width * scale))),
                  std::max(1, int(std::lround(full.height * scale))));
}

std::vector<uint8_t> jpegOf(const Picture& picture, double quality)
{
    const std::vector<uint8_t> bgra = premultipliedBgra(picture.rgba.data(), picture.rgba.size() / 4);
    return encodeJpeg(bgra.data(), uint32_t(picture.width), uint32_t(picture.height), size_t(picture.width) * 4,
                      quality);
}

// Seconds either side of a point a sampled picture may come from: half of the one-second GOP the show's transcodes
// have, so every picture can be a keyframe even on a short clip.
constexpr double kKeyframeReach = 0.5;

} // namespace

std::vector<uint8_t> encodeJpeg(const uint8_t* bgra, uint32_t width, uint32_t height, size_t bytesPerRow,
                                double quality)
{
    std::vector<uint8_t> rgb(size_t(width) * height * 3);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* p = bgra + y * bytesPerRow + x * 4;
            uint8_t* q = &rgb[(size_t(y) * width + x) * 3];
            q[0] = p[2], q[1] = p[1], q[2] = p[0];
        }
    std::vector<uint8_t> jpeg;
    const auto append = [](void* context, void* data, int size) {
        auto* out = static_cast<std::vector<uint8_t>*>(context);
        out->insert(out->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
    };
    const int percent = std::clamp(int(std::lround(quality * 100.0)), 1, 100);
    if (!stbi_write_jpg_to_func(append, &jpeg, int(width), int(height), 3, rgb.data(), percent))
        return {};
    return jpeg;
}

std::vector<uint8_t> videoStill(const std::filesystem::path& file, uint32_t width)
{
    Picture still = hapPicture(file, 1.0 / 3.0, int(width));
    if (still.rgba.empty()) {
        Grabber grabber(file);
        const bool sideways = grabber.turns() % 2 == 1;
        const double shownWidth = sideways ? grabber.height() : grabber.width();
        const double shownHeight = sideways ? grabber.width() : grabber.height();
        const double scale = std::min({1.0, width / shownWidth, width / shownHeight});
        still = turned(grabber.grab(grabber.duration() / 3.0, 0.5,
                                    std::max(1, int(std::lround(grabber.width() * scale))),
                                    std::max(1, int(std::lround(grabber.height() * scale)))),
                       grabber.turns());
    }
    if (still.rgba.empty())
        throw std::runtime_error("no still in " + file.string());
    std::vector<uint8_t> jpeg = jpegOf(still, 0.75);
    if (jpeg.empty())
        throw std::runtime_error("cannot encode a still of " + file.string());
    return jpeg;
}

struct VideoSampler::Impl {
    std::filesystem::path file;
    std::unique_ptr<Grabber> grabber;
    uint32_t width = 0;
    uint32_t height = 0;
    int count = 1;
    double tolerance = 0.0;
};

VideoSampler::VideoSampler(const std::filesystem::path& file, uint32_t width, uint32_t height, int count)
    : impl_(std::make_unique<Impl>())
{
    impl_->file = file;
    impl_->width = width;
    impl_->height = height;
    impl_->count = std::max(1, count);
    if (hapCodecOfFile(file) != HapCodec::None)
        return;
    impl_->grabber = std::make_unique<Grabber>(file);
    impl_->tolerance = std::max(impl_->grabber->duration() / (2.0 * impl_->count), kKeyframeReach);
}

VideoSampler::~VideoSampler() = default;

std::vector<uint8_t> VideoSampler::picture(int i)
{
    const double fraction = (2.0 * i + 1.0) / (2.0 * impl_->count);
    Picture picture;
    if (!impl_->grabber) {
        picture = hapPicture(impl_->file, fraction, int(2 * std::max(impl_->width, impl_->height)));
    } else {
        Grabber& g = *impl_->grabber;
        const bool sideways = g.turns() % 2 == 1;
        const double scale = std::max(impl_->width / double(sideways ? g.height() : g.width()),
                                      impl_->height / double(sideways ? g.width() : g.height()));
        picture = turned(g.grab(g.duration() * fraction, impl_->tolerance,
                                std::max(1, int(std::ceil(g.width() * scale))),
                                std::max(1, int(std::ceil(g.height() * scale)))),
                         g.turns());
    }
    if (picture.rgba.empty())
        return {};
    const double scale = std::max(impl_->width / double(picture.width), impl_->height / double(picture.height));
    picture = scaled(picture, std::max(int(impl_->width), int(std::ceil(picture.width * scale))),
                     std::max(int(impl_->height), int(std::ceil(picture.height * scale))));
    if (picture.rgba.empty())
        return {};
    const int left = (picture.width - int(impl_->width)) / 2;
    const int top = (picture.height - int(impl_->height)) / 2;
    std::vector<uint8_t> cropped(size_t(impl_->width) * impl_->height * 4);
    for (uint32_t y = 0; y < impl_->height; ++y)
        std::copy_n(&picture.rgba[(size_t(top + int(y)) * picture.width + size_t(left)) * 4], impl_->width * 4,
                    &cropped[size_t(y) * impl_->width * 4]);
    return premultipliedBgra(cropped.data(), size_t(impl_->width) * impl_->height);
}

} // namespace azl
