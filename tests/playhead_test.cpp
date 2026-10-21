#include "engine/playhead.hpp"

#include <doctest.h>

#include <vector>

using azl::FramePick;
using azl::FrameTarget;
using azl::Playhead;
using azl::PlayMode;
using azl::SyncMode;
using azl::Transport;

namespace {

constexpr azl::ClipLength kClip{100, 25.0};

Playhead started(const Transport& transport, azl::ClipLength length = kClip)
{
    Playhead playhead;
    playhead.retime(transport, length, 0.0, 0.0, 0);
    return playhead;
}

FramePick pickAt(Playhead& playhead, double seconds, double beat, std::vector<FrameTarget>* targets = nullptr)
{
    std::vector<FrameTarget> scratch;
    return playhead.pick(seconds, beat, targets ? *targets : scratch);
}

} // namespace

TEST_CASE("playhead: a BPM-synced clip runs on the beats and always prefetches its first frame")
{
    Playhead playhead = started({.sync = SyncMode::Bpm, .beats = 4});
    std::vector<FrameTarget> targets;
    CHECK(pickAt(playhead, 0.0, 2.0, &targets).index == 50);
    CHECK(targets == std::vector<FrameTarget>{{0, true}});
    CHECK(pickAt(playhead, 0.0, 5.0).index == 25);
}

TEST_CASE("playhead: a timeline clip loops between its in and out points and prefetches where it wraps")
{
    Playhead playhead = started({.sync = SyncMode::Timeline, .in = 0.5, .out = 1.0});
    std::vector<FrameTarget> targets;
    CHECK(pickAt(playhead, 1.0, 0.0, &targets).index == 75);
    CHECK(targets == std::vector<FrameTarget>{{50, true}});
    CHECK(pickAt(playhead, 2.0, 0.0).index == 50);
}

TEST_CASE("playhead: a speed change keeps the picture where it is and plays on at the new speed")
{
    Transport transport{.sync = SyncMode::Timeline};
    Playhead playhead = started(transport);
    CHECK(pickAt(playhead, 1.0, 0.0).index == 25);
    transport.speed = 2.0;
    playhead.retime(transport, kClip, 1.0, 0.0, 0);
    CHECK(pickAt(playhead, 1.0, 0.0).index == 25);
    CHECK(pickAt(playhead, 1.5, 0.0).index == 50);
}

TEST_CASE("playhead: a resync restarts a BPM-synced clip on the new downbeat, a timeline clip plays on")
{
    Playhead bpm = started({.sync = SyncMode::Bpm, .beats = 4});
    bpm.retime(bpm.transport, kClip, 3.0, 0.0, 1);
    CHECK(pickAt(bpm, 3.0, 0.0).index == 0);
    CHECK(pickAt(bpm, 3.0, 1.0).index == 25);

    Playhead timeline = started({.sync = SyncMode::Timeline});
    timeline.retime(timeline.transport, kClip, 1.0, 0.0, 1);
    CHECK(pickAt(timeline, 1.0, 0.0).index == 25);
}

TEST_CASE("playhead: a bounce prefetches where it turns")
{
    Playhead playhead = started({.sync = SyncMode::Timeline, .playMode = PlayMode::Bounce});
    std::vector<FrameTarget> targets;
    const FramePick forward = pickAt(playhead, 1.0, 0.0, &targets);
    CHECK(forward.forward);
    CHECK(targets == std::vector<FrameTarget>{{99, false}});
    targets.clear();
    const FramePick backward = pickAt(playhead, 5.0, 0.0, &targets);
    CHECK_FALSE(backward.forward);
    CHECK(targets == std::vector<FrameTarget>{{0, true}});
}

TEST_CASE("playhead: a scrub moves the clip and it plays on from there")
{
    Playhead playhead = started({.sync = SyncMode::Timeline});
    playhead.scrub(0.6, 2.0, 0.0);
    CHECK(pickAt(playhead, 2.0, 0.0).index == 60);
    CHECK(pickAt(playhead, 2.4, 0.0).index == 70);
}

TEST_CASE("playhead: Random Jump lands on the target it prefetched, and its key repeats it")
{
    const Transport transport{.sync = SyncMode::Bpm,
                              .playMode = PlayMode::RandomJump,
                              .beats = 8,
                              .jumpInterval = 2.0,
                              .jumpDistance = 3.0};
    auto run = [&](uint64_t key) {
        Playhead playhead;
        playhead.key = key;
        playhead.retime(transport, {96, 24.0}, 0.0, 0.0, 0);
        std::vector<int> shown;
        for (int jump = 1; jump <= 12; ++jump) {
            std::vector<FrameTarget> targets;
            pickAt(playhead, 0.0, jump * 2.0 - 0.25, &targets);
            REQUIRE(!targets.empty());
            const int landing = pickAt(playhead, 0.0, jump * 2.0).index;
            CHECK(landing == targets.front().index);
            CHECK(landing % 12 == 0);
            shown.push_back(landing);
        }
        return shown;
    };
    const std::vector<int> first = run(42);
    CHECK(run(42) == first);
}
