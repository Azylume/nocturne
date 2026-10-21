#include "djlink/clock_follower.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace azl::djlink {

ClockFollower::ClockFollower(BeatSource& source, Engine& engine, DocumentServer& document, Now now, Options options)
    : source_(source), engine_(engine), document_(document), now_(std::move(now)), allowed_(std::move(options.allowed))
{
    if (!options.thread)
        return;
    thread_ = std::thread([this] {
        while (running_) {
            step(now_());
            std::this_thread::sleep_for(kPeriod);
        }
    });
}

ClockFollower::~ClockFollower()
{
    running_ = false;
    if (thread_.joinable())
        thread_.join();
}

void ClockFollower::step(double now)
{
    const std::optional<OnAirBeat> beat = !allowed_ || allowed_() ? source_.onAirBeat(now) : std::nullopt;
    if (!beat) {
        if (last_)
            letGo();
        return;
    }
    bool fresh = !last_ || last_->player != beat->player || last_->time != beat->time;
    last_ = beat;
    const double clockBeat = engine_.transport().timeline.beatAt(now);
    const BeatTimeline target = followTimeline(*beat, clockBeat, now);
    const double offset = clockBeat - target.beatAt(now);
    if (std::abs(offset) > kAlignAbove)
        aligning_ = true;
    if (aligning_ && std::abs(offset) <= kAligned) {
        aligning_ = false;
        fresh = true;
    }
    if (aligning_)
        return engine_.follow(pulledIn(target, clockBeat, offset, now), true);
    if (fresh)
        engine_.follow(target, true);
}

// The clock's own beat now, advancing at the target's tempo bent towards it: by kAlignBend while far off, then in
// proportion to the offset over a beat, so it settles without overshooting.
BeatTimeline ClockFollower::pulledIn(const BeatTimeline& target, double clockBeat, double offset, double now)
{
    const double bend = std::clamp(offset, -kAlignBend, kAlignBend);
    return {now, clockBeat, target.bpm * (1.0 - bend), 0.0, target.resyncs};
}

void ClockFollower::letGo()
{
    const double bpm = std::round(last_->bpm * 100.0) / 100.0;
    last_.reset();
    aligning_ = false;
    try {
        if (allowed_ && !allowed_())
            return;
        document_.perform(SetTempo{bpm}, "djlink");
        engine_.unfollow();
    } catch (const std::exception& e) {
        std::printf("djlink: keeping the tempo failed: %s\n", e.what());
        engine_.unfollow();
    }
}

} // namespace azl::djlink
