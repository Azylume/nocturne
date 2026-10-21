#pragma once

#include "document/composition.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace azl {

// One engine's beat timeline: `beat` at engine time `time`, advancing at the BPM bent by the nudge. `resyncs` counts
// that engine's resyncs, so a follower can tell a jump from a drift.
struct BeatTimeline {
    double time = 0.0;
    double beat = 0.0;
    double bpm = 120.0;
    double nudge = 0.0;
    uint64_t resyncs = 0;

    double tempo() const { return std::clamp(bpm * (1.0 + nudge / 100.0), kMinBpm, kMaxBpm); }
    double beatAt(double seconds) const { return beat + (seconds - time) * tempo() / 60.0; }

    bool operator==(const BeatTimeline&) const = default;
};

// The composition's beat timeline: beats as a function of engine time, beat(t) = anchor.beat + (t - anchor.time) *
// tempo / 60. Every change re-anchors at its own time, so positions are computed, never accumulated, and do not drift.
class BeatClock {
public:
    double beatAt(double seconds) const { return anchor_.beat + (seconds - anchor_.time) * tempo() / 60.0; }
    double bpm() const { return bpm_; }
    double nudge() const { return nudge_; }
    // The BPM the beats advance at: the BPM bent by the nudge.
    double tempo() const { return std::clamp(bpm_ * (1.0 + nudge_ / 100.0), kMinBpm, kMaxBpm); }
    uint64_t resyncs() const { return resyncs_; }
    BeatTimeline timeline() const { return {anchor_.time, anchor_.beat, bpm_, nudge_, resyncs_}; }

    // Out-of-range values are clamped and non-finite ones ignored, so a bad value never corrupts the timeline.
    void setBpm(double bpm, double seconds)
    {
        if (!std::isfinite(bpm))
            return;
        rebase(seconds);
        bpm_ = std::clamp(bpm, kMinBpm, kMaxBpm);
    }

    // Bends the tempo by `percent` until the nudge is set back to 0. The beat phase keeps what it gained or lost.
    void setNudge(double percent, double seconds)
    {
        if (!std::isfinite(percent))
            return;
        rebase(seconds);
        nudge_ = percent;
    }

    // Continues a saved beat grid: `beat` at `seconds`.
    void restore(double beat, double seconds)
    {
        if (std::isfinite(beat))
            anchor_ = {seconds, beat};
    }

    // The downbeat is now: beat 0 at `seconds`.
    void resync(double seconds)
    {
        anchor_ = {seconds, 0.0};
        ++resyncs_;
    }

    // Takes over a followed timeline: `beat` at `seconds`; a resync of the followed clock counts as one here too.
    void follow(double beat, double seconds, double bpm, double nudge, bool resynced)
    {
        if (!std::isfinite(beat) || !std::isfinite(bpm) || !std::isfinite(nudge))
            return;
        anchor_ = {seconds, beat};
        bpm_ = std::clamp(bpm, kMinBpm, kMaxBpm);
        nudge_ = nudge;
        resyncs_ += resynced;
    }

private:
    void rebase(double seconds) { anchor_ = {seconds, beatAt(seconds)}; }

    struct Anchor {
        double time = 0.0;
        double beat = 0.0;
    };
    Anchor anchor_;
    double bpm_ = 120.0;
    double nudge_ = 0.0;
    uint64_t resyncs_ = 0;
};

// Follows another engine's beat timeline, already mapped onto this engine's clock, without a visible jump. A target
// that disagrees with the beat shown is approached by bending the tempo by at most kMaxBend; only a resync of the
// followed clock, or an error too large to bend away within kMaxSlew seconds, jumps.
class BeatFollower {
public:
    static constexpr double kMaxBend = 0.02;
    static constexpr double kMaxSlew = 30.0;

    // Returns true when the followed clock resynced since the last target.
    bool target(const BeatTimeline& timeline, double seconds)
    {
        if (!std::isfinite(timeline.time) || !std::isfinite(timeline.beat) || !std::isfinite(timeline.bpm))
            return false;
        const bool first = !target_;
        const bool resynced = !first && timeline.resyncs != target_->resyncs;
        double error = first || resynced ? 0.0 : beatAt(seconds) - timeline.beatAt(seconds);
        if (std::abs(error) > kMaxSlew * rate(timeline))
            error = 0.0;
        target_ = timeline;
        error_ = error;
        errorTime_ = seconds;
        return resynced;
    }

    // Only after a target.
    const BeatTimeline& timeline() const { return *target_; }
    // The beat shown at `seconds`: the target's plus what is left of the error being bent away.
    double beatAt(double seconds) const
    {
        const double left = std::max(0.0, std::abs(error_) - rate(*target_) * std::max(0.0, seconds - errorTime_));
        return target_->beatAt(seconds) + std::copysign(left, error_);
    }

private:
    static double rate(const BeatTimeline& timeline) { return kMaxBend * timeline.tempo() / 60.0; }

    std::optional<BeatTimeline> target_;
    double error_ = 0.0;
    double errorTime_ = 0.0;
};

// Tap tempo: the mean interval over a run of taps, so jitter on the taps in between cancels out. A pause of two
// seconds starts a new run.
class TapTempo {
public:
    static constexpr double kReset = 2.0;

    // The tempo of the run so far, or nothing when this tap starts a run.
    std::optional<double> tap(double seconds)
    {
        if (seconds - last_ >= kReset) {
            first_ = last_ = seconds;
            intervals_ = 0;
            return std::nullopt;
        }
        if (seconds <= last_)
            return std::nullopt;
        last_ = seconds;
        ++intervals_;
        return std::clamp(60.0 * intervals_ / (last_ - first_), kMinBpm, kMaxBpm);
    }

private:
    double first_ = 0.0;
    double last_ = -std::numeric_limits<double>::infinity();
    int intervals_ = 0;
};

} // namespace azl
