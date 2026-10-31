#include "output/h264_stream.hpp"

#include "media/ffmpeg_util.hpp"

namespace azl {

// libavcodec's H.264 decoder on one thread, without frame delay: each access unit in, its picture out. Packets go in
// as Annex B (return_video::annexB), parameter sets first on key frames.
struct H264Decoder::Session {
    std::vector<uint8_t> config;
    ff::Codec codec;
    ff::Packet packet{av_packet_alloc()};
    ff::Frame frame{av_frame_alloc()};
    ff::Scale scale;

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
        Picture picture{uint32_t(in.width), uint32_t(in.height), {}};
        picture.bgra.resize(size_t(in.width) * in.height * 4);
        uint8_t* target[4] = {picture.bgra.data()};
        const int stride[4] = {in.width * 4};
        if (sws_scale(scale.get(), in.data, in.linesize, 0, in.height, target, stride) <= 0)
            return std::nullopt;
        return picture;
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
