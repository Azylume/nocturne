#pragma once

#include "djlink/djlink_state.hpp"
#include "engine/engine.hpp"
#include "protocol/document_server.hpp"

#include <atomic>
#include <functional>
#include <optional>
#include <thread>

namespace azl::djlink {

// Drives the engine's beat clock from a BeatSource. While the source has an on-air beat, the clock follows it,
// re-targeted on every new beat and every change of player, and never jumps: a clock more than kAlignAbove beats off
// the player's bar (a handover to a player half a bar out, or the follow starting) is pulled in by bending its tempo by
// up to kAlignBend until it is within kAligned, so half a bar takes about two bars; smaller offsets are bent away by the
// engine's own follower. When the beat is lost, the document's tempo takes the last followed BPM (a performed edit, cause "djlink", no undo step) and
// the clock runs on by itself from where it is.
class ClockFollower {
public:
    using Now = std::function<double()>;
    // False while something else owns the clock (a node following its main engine): nothing is followed then.
    using Allowed = std::function<bool()>;

    static constexpr auto kPeriod = std::chrono::milliseconds(20);
    static constexpr double kAlignAbove = 0.25;
    static constexpr double kAligned = 0.02;
    static constexpr double kAlignBend = 0.25;

    struct Options {
        // Steps every kPeriod on its own thread; without, the caller steps (tests).
        bool thread = true;
        Allowed allowed;
    };

    ClockFollower(BeatSource& source, Engine& engine, DocumentServer& document, Now now, Options options);
    ~ClockFollower();
    ClockFollower(const ClockFollower&) = delete;
    ClockFollower& operator=(const ClockFollower&) = delete;

    void step(double now);
    bool following() const { return last_.has_value(); }
    bool aligning() const { return aligning_; }

private:
    void letGo();
    static BeatTimeline pulledIn(const BeatTimeline& target, double clockBeat, double offset, double now);

    BeatSource& source_;
    Engine& engine_;
    DocumentServer& document_;
    const Now now_;
    const Allowed allowed_;
    std::optional<OnAirBeat> last_;
    bool aligning_ = false;
    std::atomic<bool> running_{true};
    std::thread thread_;
};

} // namespace azl::djlink
