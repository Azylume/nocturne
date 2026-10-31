#include "output/h264_stream.hpp"

#include "output/picture_pool_ffmpeg.hpp"

extern "C" {
#include <libavutil/opt.h>
}

#include <algorithm>
#include <climits>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace azl {

namespace {

constexpr AVRational kTimeBase{1, 90000};
// The pictures the service paints into before the encoder takes them.
constexpr int kPictures = 4;

struct Candidate {
    const char* name;
    bool hardware;
    // Private options of the encoder.
    std::vector<std::pair<const char*, const char*>> options;
};

// In the order they are tried: NVENC, then Media Foundation's encoder on the GPU, then Microsoft's software one.
const std::vector<Candidate>& candidates()
{
    static const std::vector<Candidate> list{
        {"h264_nvenc", true,
         {{"preset", "p4"}, {"tune", "ll"}, {"profile", "main"}, {"rc", "cbr"}, {"delay", "0"}, {"zerolatency", "1"},
          {"forced-idr", "1"}}},
        {"h264_mf", true, {{"hw_encoding", "1"}}},
        {"h264_mf", false, {}},
    };
    return list;
}

ff::Codec openEncoder(const Candidate& candidate, uint32_t width, uint32_t height, double fps, uint32_t kbps)
{
    const AVCodec* codec = avcodec_find_encoder_by_name(candidate.name);
    ff::Codec context(codec ? avcodec_alloc_context3(codec) : nullptr);
    if (!context)
        return {};
    AVCodecContext* c = context.get();
    c->width = int(width);
    c->height = int(height);
    c->pix_fmt = AV_PIX_FMT_NV12;
    c->time_base = kTimeBase;
    c->framerate = av_d2q(fps, 100000);
    c->gop_size = std::max(1, int(std::lround(fps * return_video::kKeySeconds)));
    c->max_b_frames = 0;
    c->profile = AV_PROFILE_H264_MAIN;
    c->bit_rate = int64_t(kbps) * 1000;
    c->rc_max_rate = int64_t(kbps) * 1000;
    c->rc_buffer_size = int(kbps) * 1000;
    c->color_primaries = AVCOL_PRI_BT709;
    c->color_trc = AVCOL_TRC_BT709;
    c->colorspace = AVCOL_SPC_BT709;
    c->color_range = AVCOL_RANGE_MPEG;
    c->flags |= AV_CODEC_FLAG_LOW_DELAY;
    for (const auto& [key, value] : candidate.options)
        av_opt_set(c->priv_data, key, value, 0);
    if (avcodec_open2(c, codec, nullptr) < 0)
        return {};
    return context;
}

void appendUnit(std::vector<uint8_t>& avcc, const uint8_t* bytes, size_t size)
{
    for (int shift = 24; shift >= 0; shift -= 8)
        avcc.push_back(uint8_t(size >> shift));
    avcc.insert(avcc.end(), bytes, bytes + size);
}

// The NAL units of an Annex B stream, each without its start code.
std::vector<std::pair<const uint8_t*, size_t>> annexBUnits(const uint8_t* data, size_t size)
{
    std::vector<std::pair<const uint8_t*, size_t>> units;
    auto startCode = [&](size_t i) { return i + 3 <= size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1; };
    size_t begin = SIZE_MAX;
    for (size_t i = 0; i + 3 <= size;) {
        if (!startCode(i)) {
            ++i;
            continue;
        }
        if (begin != SIZE_MAX) {
            size_t end = i;
            while (end > begin && data[end - 1] == 0)
                --end;
            units.emplace_back(data + begin, end - begin);
        }
        i += 3;
        begin = i;
    }
    if (begin != SIZE_MAX && begin < size)
        units.emplace_back(data + begin, size - begin);
    return units;
}

// Parameter sets (SPS, PPS) go to `config`, the picture's other units to `data`; access unit delimiters are dropped.
void sortUnits(const uint8_t* bytes, size_t size, return_video::Packet& packet)
{
    for (const auto& [unit, length] : annexBUnits(bytes, size)) {
        if (length == 0)
            continue;
        const int type = unit[0] & 0x1f;
        if (type == 7 || type == 8)
            appendUnit(packet.config, unit, length);
        else if (type != 9)
            appendUnit(packet.data, unit, length);
    }
}

} // namespace

struct H264Encoder::Session {
    ff::Codec codec;
    std::unique_ptr<ff::PicturePool> pool;
    ff::Packet packet{av_packet_alloc()};
    int64_t lastPts = INT64_MIN;
};

H264Encoder::H264Encoder(uint32_t width, uint32_t height, double fps, uint32_t kbps)
    : session_(std::make_unique<Session>())
{
    for (const Candidate& candidate : candidates()) {
        if ((session_->codec = openEncoder(candidate, width, height, fps, kbps))) {
            hardware_ = candidate.hardware;
            break;
        }
    }
    if (!session_->codec || !session_->packet)
        throw std::runtime_error("no H.264 encoder");
    session_->pool = std::make_unique<ff::PicturePool>(YCbCrLayout::NV12, width, height, kPictures);
}

H264Encoder::~H264Encoder() = default;

std::shared_ptr<YCbCrPicture> H264Encoder::newBuffer()
{
    return session_->pool->take();
}

std::optional<return_video::Packet> H264Encoder::encode(const YCbCrPicture& picture, double seconds, bool key)
{
    Session& s = *session_;
    AVFrame* frame = ff::PicturePool::frameOf(picture);
    frame->pts = std::max<int64_t>(std::llround(seconds * kTimeBase.den), s.lastPts + 1);
    s.lastPts = frame->pts;
    frame->pict_type = key ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    frame->color_primaries = AVCOL_PRI_BT709;
    frame->color_trc = AVCOL_TRC_BT709;
    frame->colorspace = AVCOL_SPC_BT709;
    frame->color_range = AVCOL_RANGE_MPEG;
    if (avcodec_send_frame(s.codec.get(), frame) < 0)
        return std::nullopt;
    std::optional<return_video::Packet> out;
    while (avcodec_receive_packet(s.codec.get(), s.packet.get()) >= 0) {
        return_video::Packet packet;
        packet.key = s.packet->flags & AV_PKT_FLAG_KEY;
        sortUnits(s.packet->data, size_t(s.packet->size), packet);
        if (packet.key && packet.config.empty()) {
            return_video::Packet header;
            sortUnits(s.codec->extradata, size_t(s.codec->extradata_size), header);
            packet.config = std::move(header.config);
        }
        av_packet_unref(s.packet.get());
        out = std::move(packet);
    }
    return out;
}

} // namespace azl
