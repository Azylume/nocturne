#include "output/h264_stream.hpp"

#include "media/ffmpeg_util.hpp"

#include <cstddef>
#include <cstring>

namespace azl {

// libavcodec's H.264 decoder on one thread, without frame delay: each access unit in, its picture out. Packets go in
// as Annex B (return_video::annexB), parameter sets first on key frames.
struct H264Decoder::Session {
    std::vector<uint8_t> config;
    ff::Codec codec;
    ff::Packet packet{av_packet_alloc()};
    ff::Frame frame{av_frame_alloc()};
    ff::Scale scale;
    ff::Frame scaled;

    bool open(const std::vector<uint8_t>& sets)
    {
        codec.reset();
        config.clear();
        const AVCodec* h264 = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!h264)
            return false;
        codec.reset(avcodec_alloc_context3(h264));
        codec->thread_count = 1;
        codec->flags |= AV_CODEC_FLAG_LOW_DELAY;
        if (avcodec_open2(codec.get(), h264, nullptr) < 0) {
            codec.reset();
            return false;
        }
        config = sets;
        return true;
    }

    std::optional<Picture> bgra(const AVFrame& in)
    {
        scale.reset(sws_getCachedContext(scale.release(), in.width, in.height, AVPixelFormat(in.format), in.width,
                                         in.height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!scale)
            return std::nullopt;
        const int* bt709 = sws_getCoefficients(SWS_CS_ITU709);
        sws_setColorspaceDetails(scale.get(), bt709, in.color_range == AVCOL_RANGE_JPEG, bt709, 1, 0, 1 << 16,
                                 1 << 16);
        if (!sizeScaled(in.width, in.height) ||
            sws_scale(scale.get(), in.data, in.linesize, 0, in.height, scaled->data, scaled->linesize) <= 0)
            return std::nullopt;
        Picture picture{uint32_t(in.width), uint32_t(in.height), {}};
        const size_t row = size_t(in.width) * 4;
        picture.bgra.resize(row * size_t(in.height));
        for (int y = 0; y < in.height; ++y)
            std::memcpy(picture.bgra.data() + size_t(y) * row, scaled->data[0] + ptrdiff_t(y) * scaled->linesize[0],
                        row);
        return picture;
    }

    // swscale writes whole SIMD blocks, past the end of a row as tight as the picture's, so it scales into a frame
    // with FFmpeg's own aligned and padded rows.
    bool sizeScaled(int width, int height)
    {
        if (scaled && scaled->width == width && scaled->height == height)
            return true;
        scaled.reset(av_frame_alloc());
        if (!scaled)
            return false;
        scaled->format = AV_PIX_FMT_BGRA;
        scaled->width = width;
        scaled->height = height;
        if (av_frame_get_buffer(scaled.get(), 0) >= 0)
            return true;
        scaled.reset();
        return false;
    }
};

H264Decoder::H264Decoder() : session_(std::make_unique<Session>()) {}

H264Decoder::~H264Decoder() = default;

std::optional<H264Decoder::Picture> H264Decoder::decode(const return_video::Packet& packet)
{
    Session& s = *session_;
    if (!packet.config.empty() && packet.config != s.config && !s.open(packet.config))
        return std::nullopt;
    if (!s.codec || packet.data.empty() || !s.packet || !s.frame)
        return std::nullopt;
    std::vector<uint8_t> stream = return_video::annexB(packet);
    const size_t size = stream.size();
    stream.resize(size + AV_INPUT_BUFFER_PADDING_SIZE);
    av_packet_unref(s.packet.get());
    s.packet->data = stream.data();
    s.packet->size = int(size);
    if (avcodec_send_packet(s.codec.get(), s.packet.get()) < 0)
        return std::nullopt;
    std::optional<Picture> picture;
    while (avcodec_receive_frame(s.codec.get(), s.frame.get()) == 0) {
        picture = s.bgra(*s.frame);
        av_frame_unref(s.frame.get());
    }
    return picture;
}

} // namespace azl
