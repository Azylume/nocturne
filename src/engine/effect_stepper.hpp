#pragma once

#include "document/composition.hpp"
#include "document/cue_edits.hpp"
#include "engine/beat_clock.hpp"
#include "engine/modulation_plan.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace azl {

// Render thread only. Which step each effect with steps is on as its Autopilot, its triggers and the Cues move it,
// outside the document, as AutopilotStepper does for layers: the steps run on the bar from the effect's last trigger,
// a trigger while its Autopilot runs holds that step for the hold, and the composition's pace scales both.
class EffectStepper {
public:
    // What an effect's Autopilot shows: the step it is on and the one it moves to next (none when off), and the bars
    // left of a trigger's hold on it. The palettes step as the steps of an effect with id kPaletteEffect, by the palette
// Autopilot and the palette selected last.
    struct Shown {
        uint64_t effect = 0;
        std::optional<size_t> step;
        std::optional<size_t> next;
        int held = 0;
    };

    EffectStepper(const BeatClock& clock, const CueState& cues, const std::atomic<uint32_t>& seed);

    void step(const Composition& composition, double beat);
    // Every effect that is on a step, by effect id, the palette first.
    const std::vector<StepPosition>& positions() const { return positions_; }
    std::vector<Shown> shown(const Composition& composition, double beat) const;
    // The palette the show is on; none without palettes.
    std::optional<Shown> shownPalette(double beat) const;

private:
    struct Pilot {
        std::optional<StepPlay> trigger;
        std::optional<size_t> step;
        // The beat the move onto `step` starts.
        double moved = 0.0;
        double stepStart = 0.0;
        size_t next = 0;
        uint64_t resyncs = 0;
        bool held = false;
        bool running = false;
        double pace = 1.0;
    };

    void stepEffect(const Effect& effect, const Composition& composition, double beat);
    void retrigger(Pilot& pilot, const Effect& effect, const StepPlay& trigger, bool running, double beat);
    void run(Pilot& pilot, const Effect& effect, const Composition& composition, double beat);
    // The effect's last trigger: the document's or a Cue's that fired after it.
    std::optional<StepPlay> triggerOf(const Effect& effect) const;
    size_t pickNext(const Effect& effect, const Pilot& pilot, AutopilotMode mode) const;
    Shown shownOf(const Effect& effect, const Pilot& pilot, double beat) const;

    const BeatClock& clock_;
    // The palettes as an effect's steps; only their number counts.
    Effect palette_{.id = kPaletteEffect};
    const CueState& cues_;
    const std::atomic<uint32_t>& seed_;
    std::map<uint64_t, Pilot> pilots_;
    std::vector<StepPosition> positions_;
};

} // namespace azl
