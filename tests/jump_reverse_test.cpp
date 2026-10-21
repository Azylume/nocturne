#include "clips.hpp"
#include "engine/render_thread.hpp"

#include <algorithm>

namespace {

using azl::PlayMode;
using azl::SyncMode;

// count_240.mp4: 240 frames at 30 fps, a keyframe every 60. 2 MB of cache bounds the two units reverse holds to about
// 24 frames, less than one GOP, so reverse splits each GOP into units of 12 and decodes from the keyframe for each.
constexpr size_t kSmallCache = size_t(2) << 20;

azl::EngineConfig config(size_t cacheBytes = azl::EngineConfig{}.clipCacheBytes, uint32_t seed = 0)
{
    return {.width = 960, .height = 540, .clipCacheBytes = cacheBytes, .seed = seed};
}

Message send(Client& client, const Message& message)
{
    client.send(message);
    return client.next();
}

Message scrub(Client& client, double position, uint64_t layer = 1)
{
    return send(client, {{"type", "clock"}, {"op", "scrub"}, {"layer", layer}, {"position", position}});
}

Message clockState(Client& client)
{
    return send(client, {{"type", "clock"}, {"op", "state"}});
}

azl::Transport timeline(PlayMode mode)
{
    return {SyncMode::Timeline, mode, 0};
}

std::vector<int> nextFrames(azl::EngineHarness& harness, int count)
{
    std::vector<int> frames;
    for (int i = 0; i < count; ++i)
        frames.push_back(frameAfterStep(harness));
    return frames;
}

std::vector<int> countDown(int from, int count)
{
    std::vector<int> frames;
    for (int i = 0; i < count; ++i)
        frames.push_back(((from - i) % 240 + 240) % 240);
    return frames;
}

// The beat each jump of a 16-beat count_240 clip lands on, read off the screen while it plays at 120 BPM and 60 fps
// with a one-beat interval: a beat is 30 render frames and 15 clip frames.
std::vector<int> jumpBeats(azl::EngineHarness& harness, int jumps)
{
    std::vector<int> beats;
    int beat = 0;
    for (int j = 0; j <= jumps; ++j) {
        const int landed = frameAfterStep(harness);
        REQUIRE_MESSAGE(landed % 15 == 0, "jump " << j << " landed off the beat grid at frame " << landed);
        if (j > 0) {
            const int offset = ((landed / 15 - beat - 1) % 16 + 16 + 8) % 16 - 8;
            CHECK_MESSAGE(std::abs(offset) <= 4, "jump " << j << " went " << offset << " beats");
        }
        beat = landed / 15;
        beats.push_back(beat);
        for (int k = 1; k < 30; ++k)
            REQUIRE(frameAfterStep(harness) == (landed + k / 2) % 240);
    }
    return beats;
}

} // namespace

TEST_CASE("reverse plays backward through long GOPs, starting with the clip's last frame")
{
    for (const size_t cache : {azl::EngineConfig{}.clipCacheBytes, kSmallCache}) {
        CAPTURE(cache);
        TempDir media;
        azl::EngineHarness harness(config(cache), 30.0);
        Client client(harness.document());
        playMedia(client, copyMedia("count_240.mp4", media.path / "count.mp4"), timeline(PlayMode::Reverse));

        CHECK(nextFrames(harness, 245) == countDown(239, 245));
        CHECK(harness.stats().videoMisses == 0);
    }
}

TEST_CASE("with the cache holding two GOPs, reverse decodes every frame once")
{
    TempDir media;
    azl::EngineHarness harness(config(), 30.0);
    Client client(harness.document());
    playMedia(client, copyMedia("count_240.mp4", media.path / "count.mp4"), timeline(PlayMode::Reverse));

    CHECK(nextFrames(harness, 240) == countDown(239, 240));
    CHECK(harness.stats().videoMisses == 0);
    // A few more: the reader that starts a GOP may hand over the frame before it, and the next lap is under way.
    CHECK(harness.decodeStats().frames <= 240 + 4 * 2 + 60);
    CHECK(harness.decodeStats().seeks <= 4 + 2);
}

TEST_CASE("bounce turns at the clip end and reads back through every GOP from the cache")
{
    TempDir media;
    azl::EngineHarness harness(config(kSmallCache), 30.0);
    Client client(harness.document());
    playMedia(client, copyMedia("count_240.mp4", media.path / "count.mp4"), timeline(PlayMode::Bounce));

    harness.step(238);
    CHECK(nextFrames(harness, 4) == std::vector{238, 239, 239, 238});
    CHECK(nextFrames(harness, 238) == countDown(237, 238));
    CHECK(harness.stats().videoMisses == 0);
}

TEST_CASE("scrubbing moves a clip to any position, and it plays on from there")
{
    TempDir media;
    azl::EngineHarness harness(config(kSmallCache), 30.0);
    Client client(harness.document());
    playMedia(client, copyMedia("count_240.mp4", media.path / "count.mp4"), timeline(PlayMode::Loop));
    harness.step(10);

    CHECK(scrub(client, 0.5)["position"] == 0.5);
    CHECK(nextFrames(harness, 3) == std::vector{120, 121, 122});
    scrub(client, 0.9);
    CHECK(nextFrames(harness, 2) == std::vector{216, 217});
    // The clip's end is where a loop wraps to its first frame.
    scrub(client, 1.0);
    CHECK(nextFrames(harness, 2) == std::vector{0, 1});
    scrub(client, 0.3);
    CHECK(frameAfterStep(harness) == 72);

    // Backward, frame i covers (i, i + 1]: a quarter into the clip is frame 59.
    send(client, msg::command(azl::SetClipTransport{1, {}, PlayMode::Reverse, {}}));
    scrub(client, 0.25);
    CHECK(nextFrames(harness, 3) == std::vector{59, 58, 57});
    CHECK(clockState(client)["clips"][0]["frame"] == 57);

    CHECK(scrub(client, 1.5)["type"] == "error");
    CHECK(send(client, {{"type", "clock"}, {"op", "scrub"}, {"position", 0.5}})["type"] == "error");
}

TEST_CASE("Random Jump lands on the clip's beat grid, prefetched, and a seed repeats it exactly")
{
    TempDir media;
    const fs::path clip = copyMedia("count_240.mp4", media.path / "count.mp4");
    auto run = [&](uint32_t seed) {
        azl::EngineHarness harness(config(kSmallCache, seed));
        Client client(harness.document());
        playMedia(client, clip, {SyncMode::Bpm, PlayMode::RandomJump, 0, 1.0, 4.0});
        const std::vector<int> beats = jumpBeats(harness, 24);
        CHECK(harness.stats().videoMisses == 0);
        return beats;
    };

    const std::vector<int> seven = run(7);
    CHECK(seven == std::vector{0, 5, 10, 8, 6, 6, 6, 11, 8, 6, 9, 10, 15, 12, 15, 1, 6, 8, 13, 13, 14, 3, 6, 5, 8});
    CHECK(run(7) == seven);
    CHECK(run(8) != seven);
}

TEST_CASE("Random Jump every 8 beats by up to 2, and a resync lands on the prefetched downbeat")
{
    TempDir media;
    azl::EngineHarness harness(config(kSmallCache, 3));
    Client client(harness.document());
    playMedia(client, copyMedia("count_240.mp4", media.path / "count.mp4"),
              {SyncMode::Bpm, PlayMode::RandomJump, 0, 8.0, 2.0});

    int beat = 0;
    CHECK(frameAfterStep(harness) == 0);
    for (int jump = 1; jump <= 6; ++jump) {
        harness.step(8 * 30 - 1);
        const int landed = frameAfterStep(harness);
        CHECK(landed % 15 == 0);
        const int offset = ((landed / 15 - beat - 8) % 16 + 16 + 8) % 16 - 8;
        CHECK(std::abs(offset) <= 2);
        beat = landed / 15;
    }
    CHECK(harness.stats().videoMisses == 0);

    harness.step(100);
    send(client, {{"type", "clock"}, {"op", "resync"}});
    CHECK(nextFrames(harness, 3) == std::vector{0, 0, 1});
    CHECK(harness.stats().videoMisses == 0);
}

// One 8 s GOP at 9 ms a frame, BPM-synced over 16 beats, jumping every 8 beats by up to 2: targets are beats 6-10, 15
// frames a beat of decoding from the keyframe, over a second for beat 9 and up, longer than the second the playhead
// holds ahead. A seek that blocks until it decodes there starves the playhead.
//
// The clip is scrubbed to its start at these beats after a resync, each time landing on the prefetched first frame
// with both readers far ahead, as a retrigger does. A jump's draw is keyed by its slot (its start beat over 8,
// rounded), and these beats are never near a half slot, so the targets are the same in every run.
constexpr int kScrubBeats[] = {9, 19, 29, 39};
const azl::Transport kDeepJumps{SyncMode::Bpm, PlayMode::RandomJump, 0, 8.0, 2.0};

azl::EngineConfig slowDecoder(uint32_t seed)
{
    azl::EngineConfig slow = config(azl::EngineConfig{}.clipCacheBytes, seed);
    slow.decodeFrameCost = std::chrono::milliseconds(9);
    return slow;
}

// The clock op resyncs at the harness's stepped time, which a live render thread does not run on.
void resyncLive(azl::EngineHarness& harness)
{
    harness.engine().resync(azl::engineTime());
}

// Sleeps until `beat` at 120 BPM after the call that made it.
auto beatsFromNow()
{
    return [start = std::chrono::steady_clock::now()](double beat) {
        std::this_thread::sleep_until(start + std::chrono::duration<double>(beat / 2.0));
    };
}

// The target of the jump after each scrub, stepped at 60 fps (30 frames a beat).
std::vector<int> deepTargets(const fs::path& clip, uint32_t seed)
{
    azl::EngineHarness harness(config(azl::EngineConfig{}.clipCacheBytes, seed));
    Client client(harness.document());
    playMedia(client, clip, kDeepJumps);
    harness.step(10);
    send(client, {{"type", "clock"}, {"op", "resync"}});
    std::vector<int> targets;
    int frame = 0;
    for (const int beat : kScrubBeats) {
        harness.step(beat * 30 - frame);
        scrub(client, 0.0);
        harness.step(8 * 30);
        targets.push_back(frameAfterStep(harness) / 15);
        frame = beat * 30 + 8 * 30 + 1;
    }
    return targets;
}

TEST_CASE("on a slow decoder, reaching a Random Jump target deep in a long GOP never starves the playhead")
{
    constexpr uint32_t kSeed = 37;
    TempDir media;
    const fs::path clip = copyMedia("count_240_gop240.mp4", media.path / "gop.mp4");
    REQUIRE(deepTargets(clip, kSeed) == std::vector{10, 10, 9, 10});

    azl::EngineHarness harness(slowDecoder(kSeed));
    Client client(harness.document());
    playMedia(client, clip, kDeepJumps);
    azl::RenderThread render(harness.engine(), 60.0, &harness.outputs().vsync());
    auto until = beatsFromNow();
    // The cold start, with the resync that the scrub beats count from.
    resyncLive(harness);
    until(kScrubBeats[0]);
    const uint64_t coldStart = harness.stats().videoMisses;
    for (const int beat : kScrubBeats) {
        until(beat);
        scrub(client, 0.0);
    }
    until(kScrubBeats[std::size(kScrubBeats) - 1] + 10);
    CHECK(harness.stats().rendered > 1200);
    CHECK(harness.stats().videoMisses == coldStart);
}

// Each resync restarts the clip on its prefetched first frame, and the jump 8 beats on goes where slot 1 of the new
// grid draws, beat 7 for this seed, every time. The target's window and the frames after it lie in the GOP the playhead
// plays through from the keyframe, so both readers read the same GOP, and the playhead plays on from the target with
// the reader that prefetched it. Resyncs at these beats from the start come before that jump and after it.
constexpr int kResyncBeats[] = {9, 14, 25, 36};

TEST_CASE("on a slow decoder, resyncs on a Random Jump clip miss no frames")
{
    constexpr uint32_t kSeed = 18;
    TempDir media;
    const fs::path clip = copyMedia("count_240_gop240.mp4", media.path / "gop.mp4");
    {
        azl::EngineHarness stepped(config(azl::EngineConfig{}.clipCacheBytes, kSeed));
        Client client(stepped.document());
        playMedia(client, clip, kDeepJumps);
        stepped.step(10);
        send(client, {{"type", "clock"}, {"op", "resync"}});
        stepped.step(8 * 30);
        REQUIRE(frameAfterStep(stepped) == 7 * 15);
    }

    // The misses this guards against are a few frames, as many as a busy machine costs a held-up decode thread: a read
    // stalled past 50 ms (13-14 ms on a quiet M2 Pro, 100-700 under tools/load.sh) leaves a run undecided, and it runs
    // once more.
    for (int run = 1;; ++run) {
        azl::EngineHarness harness(slowDecoder(kSeed));
        Client client(harness.document());
        playMedia(client, clip, kDeepJumps);
        const auto started = std::chrono::steady_clock::now();
        azl::RenderThread render(harness.engine(), 60.0, &harness.outputs().vsync());
        auto until = beatsFromNow();
        resyncLive(harness);
        until(kResyncBeats[0]);
        const uint64_t coldStart = harness.stats().videoMisses;
        for (const int beat : kResyncBeats) {
            until(beat);
            resyncLive(harness);
        }
        until(kResyncBeats[std::size(kResyncBeats) - 1] + 11);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const double slowestRead = harness.decodeStats().maxReadMicros / 1000.0;
        MESSAGE(harness.stats().rendered.load() << " frames rendered in " << seconds << " s, slowest read "
                                                << slowestRead << " ms, "
                                                << harness.stats().videoMisses.load() - coldStart
                                                << " misses after the cold start");
        if (slowestRead < 50.0) {
            CHECK(harness.stats().rendered.load() > 0.8 * 60.0 * seconds);
            CHECK(harness.stats().videoMisses.load() == coldStart);
            break;
        }
        MESSAGE("UNDECIDED: the machine stalled the decoder, run " << run);
        if (run == 2) {
            FAIL_CHECK("undecided twice in a row");
            break;
        }
    }
}

azl::EngineConfig heldAtStart(azl::EngineConfig config)
{
    config.holdStart = true;
    return config;
}

TEST_CASE("held at start-up, a layer stays black until its clip is decoded, then plays from its first frame")
{
    TempDir media;
    azl::EngineHarness harness(heldAtStart(config()));
    Client client(harness.document());
    playMedia(client, copyMedia("count_240.mp4", media.path / "count.mp4"));

    harness.step();
    CHECK(harness.stats().starting);
    const azl::Image held = harness.readScreen();
    for (const auto& [x, y] : {std::pair{60u, kBitRow}, {240u, 405u}, {720u, 405u}})
        CHECK(pixel(held, x, y)[0] == 0);
    CHECK(nextFrames(harness, 4) == std::vector{0, 0, 1, 1});
    CHECK_FALSE(harness.stats().starting);
    CHECK(harness.stats().videoMisses == 0);
}

// Live, with one read as slow as reads get on the Mac mini while many clips open at once. Unheld, the layer shows the
// clip's first frames as soon as they are decoded, then misses the frames behind the slow read.
TEST_CASE("a show held at start-up misses no frames from its first rendered frame, though a first read is slow")
{
    TempDir media;
    azl::EngineHarness harness(heldAtStart(config()));
    Client client(harness.document());
    playMedia(client, copyMedia("count_240.mp4", media.path / "count.mp4"));
    harness.injectDecoderStall(1, 4, std::chrono::milliseconds(400));
    azl::RenderThread render(harness.engine(), 60.0, &harness.outputs().vsync());
    beatsFromNow()(6);
    CHECK_FALSE(harness.stats().starting);
    CHECK(harness.stats().rendered.load() > 150);
    CHECK(harness.stats().videoMisses.load() == 0);
}

TEST_CASE("a resync restarts a BPM-synced clip on a prefetched first frame")
{
    TempDir media;
    azl::EngineHarness harness(config(kSmallCache));
    Client client(harness.document());
    playMedia(client, copyMedia("count_240.mp4", media.path / "count.mp4"));

    CHECK(frameAfterStep(harness, 300) == 149);
    send(client, {{"type", "clock"}, {"op", "resync"}});
    CHECK(nextFrames(harness, 3) == std::vector{0, 0, 1});
    CHECK(harness.stats().videoMisses == 0);
}

TEST_CASE("a decoder error mid-stream restarts at the next keyframe while the layer holds its last frame")
{
    TempDir media;
    azl::Composition two = azl::defaultComposition();
    two.layers.push_back({.id = 2, .name = "Layer 2", .opacity = 0.0});
    azl::EngineHarness harness(config(), 30.0, {}, two);
    Client client(harness.document());
    const Message failing = addMedia(client, copyMedia("count_240.mp4", media.path / "count240.mp4"))["entry"];
    const Message other = addMedia(client, copyMedia("count_709_tv.mp4", media.path / "count.mp4"))["entry"];
    send(client, msg::command(azl::SetLayerClip{1, azl::ClipRef{failing["hash"], "failing", timeline(PlayMode::Loop)}}));
    send(client, msg::command(azl::SetLayerClip{2, azl::ClipRef{other["hash"], "other", timeline(PlayMode::Loop)}}));

    harness.injectDecoderFault(1, 70);
    std::vector<int> expected;
    for (int i = 0; i < 70; ++i)
        expected.push_back(i);
    expected.insert(expected.end(), 50, 69);
    for (int i = 120; i < 240 + 75; ++i)
        expected.push_back(i % 240);
    CHECK(nextFrames(harness, int(expected.size())) == expected);
    CHECK(harness.stats().videoMisses == 50);
    CHECK(clockState(client)["clips"][1]["frame"] == (int(expected.size()) - 1) % 60);

    client.send({{"type", "library"}, {"op", "list"}});
    const Message list = client.next();
    CHECK(std::all_of(list["entries"].begin(), list["entries"].end(), [](const Message& e) { return e["status"] == "ok"; }));
}

TEST_CASE("Random Jump and reverse are undoable transport edits; bad jump settings are refused")
{
    TempDir media;
    azl::EngineHarness harness(config());
    Client client(harness.document());
    playMedia(client, copyMedia("count_240.mp4", media.path / "count.mp4"));

    Message change = send(client, msg::command(azl::SetClipTransport{1, {}, PlayMode::RandomJump, {}, 0.25, 3.0}));
    CHECK(change["playMode"] == "randomJump");
    CHECK(change["jumpInterval"] == 0.25);
    CHECK(change["jumpDistance"] == 3.0);
    Client late(harness.document());
    const Message& transport = late.snapshot["composition"]["layers"][0]["clip"]["transport"];
    CHECK(transport["playMode"] == "randomJump");
    CHECK(transport["jumpInterval"] == 0.25);

    client.send(msg::undo());
    change = client.next();
    CHECK(change["playMode"] == "loop");
    CHECK(change["jumpInterval"] == 8.0);
    CHECK(change["jumpDistance"] == 2.0);
    CHECK(send(client, msg::command(azl::SetClipTransport{1, {}, PlayMode::Reverse, {}}))["playMode"] == "reverse");

    for (const double interval : {0.0, 0.3, 16.0, -1.0, 0.0625})
        CHECK(send(client, msg::command(azl::SetClipTransport{1, {}, {}, {}, interval, {}}))["type"] == "error");
    for (const double distance : {-1.0, 1025.0})
        CHECK(send(client, msg::command(azl::SetClipTransport{1, {}, {}, {}, {}, distance}))["type"] == "error");
}
