// The return video's encoder off Apple: the twins of return_video_test.cpp's encode checks, decoded with FFmpeg here.
#include "ffmpeg_movies.hpp"

#include "output/h264_stream.hpp"

#include <cmath>
#include <optional>
#include <random>

namespace {

namespace rv = azl::return_video;

// Decodes return video packets the way a viewer does: each packet as a raw Annex B access unit.
class Viewer {
public:
    Viewer() : codec_(avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264))), packet_(av_packet_alloc()),
               frame_(av_frame_alloc())
    {
        REQUIRE(codec_);
        REQUIRE(avcodec_open2(codec_.get(), codec_->codec, nullptr) >= 0);
    }

    std::optional<Rgb> decode(const rv::Packet& packet)
    {
        const std::vector<uint8_t> bytes = rv::annexB(packet);
        REQUIRE(av_new_packet(packet_.get(), int(bytes.size())) >= 0);
        std::copy(bytes.begin(), bytes.end(), packet_->data);
        const int sent = avcodec_send_packet(codec_.get(), packet_.get());
        av_packet_unref(packet_.get());
        if (sent < 0 || avcodec_receive_frame(codec_.get(), frame_.get()) < 0)
            return std::nullopt;
        Rgb rgb = toRgb(frame_.get());
        av_frame_unref(frame_.get());
        return rgb;
    }

private:
    azl::ff::Codec codec_;
    azl::ff::Packet packet_;
    azl::ff::Frame frame_;
};

std::vector<int> unitTypes(const std::vector<uint8_t>& avcc)
{
    std::vector<int> types;
    for (const auto& unit : rv::nalUnits(avcc))
        types.push_back(unit.empty() ? -1 : unit[0] & 0x1f);
    return types;
}

} // namespace

TEST_CASE("off Apple, return video is H.264 with its parameter sets on key frames and decodes back to the pictures")
{
    const rv::Plan plan = rv::plan({640, 360}, 1);
    REQUIRE(plan.width == 426);
    azl::H264Encoder encoder(plan.width, plan.height, plan.fps, plan.kbps);
    MESSAGE("hardware ", encoder.hardware().value_or(false));
    CHECK(encoder.hardware().has_value());
    Viewer viewer;
    const int keyEvery = int(std::lround(plan.fps * rv::kKeySeconds));
    constexpr int kFrames = 45;
    constexpr int kAskedKey = 25;
    double worst = 0.0;
    int lastKey = 0;
    for (int i = 0; i < kFrames; ++i) {
        CAPTURE(i);
        std::shared_ptr<azl::YCbCrPicture> picture = encoder.newBuffer();
        REQUIRE(picture);
        CHECK(picture->layout == azl::YCbCrLayout::NV12);
        const Rgb shown = card(plan.width, plan.height, i);
        fill(*picture, shown);
        const std::optional<rv::Packet> packet = encoder.encode(*picture, i / plan.fps, i == 0 || i == kAskedKey);
        REQUIRE(packet);
        CHECK_FALSE(packet->data.empty());
        // A key frame when asked for, and at least every kKeySeconds after the last one.
        CHECK(packet->key == (i == 0 || i == kAskedKey || i - lastKey >= keyEvery));
        if (packet->key)
            lastKey = i;
        if (packet->key)
            CHECK(unitTypes(packet->config) == std::vector<int>{7, 8});
        else
            CHECK(packet->config.empty());
        for (int type : unitTypes(packet->data))
            CHECK((type != 7 && type != 8 && type != 9));
        const std::optional<Rgb> decoded = viewer.decode(*packet);
        REQUIRE(decoded);
        worst = std::max(worst, meanError(*decoded, shown));
        if (i == kFrames - 1)
            CHECK(meanError(*decoded, shown) < meanError(*decoded, card(plan.width, plan.height, i - 20)));
    }
    MESSAGE("worst mean error per channel ", worst);
    CHECK(worst < 8.0);
}

TEST_CASE("off Apple, the return video of moving noise stays inside its share of the budget")
{
    const rv::Plan plan = rv::plan({1920, 1080}, 2);
    azl::H264Encoder encoder(plan.width, plan.height, plan.fps, plan.kbps);
    std::mt19937 random(7);
    constexpr double kSeconds = 10.0;
    const int frames = int(kSeconds * plan.fps);
    size_t bytes = 0;
    int pictures = 0;
    for (int i = 0; i < frames; ++i) {
        std::shared_ptr<azl::YCbCrPicture> picture = encoder.newBuffer();
        REQUIRE(picture);
        Rgb noise{plan.width, plan.height, std::vector<uint8_t>(size_t(plan.width) * plan.height * 3)};
        for (uint8_t& v : noise.rgb)
            v = uint8_t(random());
        fill(*picture, noise);
        if (const std::optional<rv::Packet> packet = encoder.encode(*picture, i / plan.fps, i == 0)) {
            bytes += packet->data.size() + packet->config.size();
            ++pictures;
        }
    }
    const double kbps = double(bytes) * 8.0 / 1000.0 / kSeconds;
    MESSAGE(kbps, " kbit/s at ", pictures / kSeconds, " pictures/s, share ", plan.kbps, " kbit/s");
    CHECK(kbps <= plan.kbps * 1.1);
    CHECK(kbps > plan.kbps * 0.25);
    CHECK(pictures >= int(frames * 0.8));
}
