#include "hap_clips.hpp"

#include "media/media_library.hpp"
#include "media/stills.hpp"

namespace {

const azl::Transport kTimeline{azl::SyncMode::Timeline, azl::PlayMode::Loop, 0};

Message scrubTo(Client& client, double position)
{
    client.send({{"type", "clock"}, {"op", "scrub"}, {"layer", uint64_t{1}}, {"position", position}});
    return client.next();
}

std::vector<int> nextShown(azl::EngineHarness& harness, int count)
{
    std::vector<int> frames;
    for (int i = 0; i < count; ++i)
        frames.push_back(frameAfterStep(harness));
    return frames;
}

} // namespace

TEST_CASE("HAP, HAP Alpha, HAP Q and HAP Q Alpha clips play at the frame for their time, loop, and composite alpha")
{
    for (const azl::HapCodec codec : kHapCodecs) {
        CAPTURE(int(codec));
        TempDir media;
        const fs::path file = writeHapCount(media.path / "count.mov", codec);
        CHECK(azl::probeMedia(file).status == azl::MediaStatus::Ok);
        azl::EngineHarness harness({.width = 960, .height = 540}, 30.0);
        Client client(harness.document());
        playMedia(client, file, kTimeline);

        CHECK(frameAfterStep(harness) == 0);
        checkHapPatches(harness.readScreen(), codec);
        CHECK(frameAfterStep(harness) == 1);
        CHECK(frameAfterStep(harness, 57) == 58);
        CHECK(frameAfterStep(harness) == 59);
        CHECK(frameAfterStep(harness) == 0);
        checkHapPatches(harness.readScreen(), codec);
        CHECK(harness.stats().videoMisses == 0);
        CHECK(harness.decodeStats().errors == 0);
    }
}

TEST_CASE("a HAP clip plays backward, and scrubs to any frame, each frame decoded on its own")
{
    TempDir media;
    const fs::path file = writeHapCount(media.path / "count.mov", azl::HapCodec::HapQ, 120, 4);
    {
        azl::EngineHarness harness({.width = 960, .height = 540}, 30.0);
        Client client(harness.document());
        playMedia(client, file, {azl::SyncMode::Timeline, azl::PlayMode::Reverse, 0});
        CHECK(nextShown(harness, 4) == std::vector{119, 118, 117, 116});
        harness.step(110);
        CHECK(nextShown(harness, 8) == std::vector{5, 4, 3, 2, 1, 0, 119, 118});
        CHECK(harness.stats().videoMisses == 0);
    }
    azl::EngineHarness harness({.width = 960, .height = 540}, 30.0);
    Client client(harness.document());
    playMedia(client, file, kTimeline);
    harness.step(10);
    CHECK(scrubTo(client, 0.5)["position"] == 0.5);
    CHECK(nextShown(harness, 3) == std::vector{60, 61, 62});
    scrubTo(client, 0.9);
    CHECK(nextShown(harness, 2) == std::vector{108, 109});
    scrubTo(client, 0.25);
    CHECK(frameAfterStep(harness) == 30);
    checkHapPatches(harness.readScreen(), azl::HapCodec::HapQ);
}

TEST_CASE("a HAP clip's still and sampled pictures come from its own frames")
{
    TempDir media;
    const fs::path file = writeHapCount(media.path / "count.mov", azl::HapCodec::HapAlpha);
    CHECK_FALSE(azl::videoStill(file, 160).empty());

    azl::VideoSampler sampler(file, 64, 36, 3);
    for (int i = 0; i < 3; ++i) {
        const std::vector<uint8_t> bgra = sampler.picture(i);
        REQUIRE(bgra.size() == size_t(64) * 36 * 4);
        // Bottom left: RGB (200, 40, 40), opaque, as BGRA.
        const uint8_t* p = &bgra[(size_t(30) * 64 + 10) * 4];
        CHECK(std::abs(int(p[2]) - 200) <= 6);
        CHECK(std::abs(int(p[1]) - 40) <= 6);
        CHECK(p[3] == 255);
    }
}
