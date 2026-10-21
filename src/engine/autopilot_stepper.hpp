#pragma once

#include "document/composition.hpp"
#include "document/composition_file.hpp"
#include "document/cue_edits.hpp"
#include "engine/beat_clock.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace azl {

// The column a column Cue fires when it fires now: its own, or the index-th of the deck shown; 0 when there is none.
uint64_t cueColumn(const Composition& composition, const CueColumn& target);

// Render thread only. Which cell each layer plays as the layers' Autopilots, the column Autopilot and column Cues step
// it, outside the document: they read the beat clock's resyncs, the Cues holding at the frame and the seed, and never
// change the document.
class AutopilotStepper {
public:
    // Whether the layer's cell in `column` holds a clip that can play.
    using Playable = std::function<bool(const Layer& layer, uint64_t column)>;

    // What a layer's Autopilot shows: the column it plays next, and the bars left of a trigger's hold on it.
    struct Shown {
        uint64_t next = 0;
        int held = 0;
    };

    // The column Autopilot: the column it triggered last, the one it plays next and the bars left of a trigger's hold.
    struct Columns {
        uint64_t column = 0;
        uint64_t next = 0;
        int held = 0;
    };

    AutopilotStepper(const BeatClock& clock, const CueState& cues, const std::atomic<uint32_t>& seed,
                     Playable playable);

    // Every layer that still plays the trigger a pilot was saved with continues it, and the column Autopilot its step.
    void restore(const Composition& composition, const std::vector<ResumePilot>& pilots);
    void fireColumnCues(const Composition& composition, double beat);
    void stepColumns(const Composition& composition, double beat);
    // The cell the layer plays at `beat`.
    std::optional<Play> steer(const Layer& layer, const Composition& composition, double beat);
    // Forgets the layers not in `ids`.
    void keepLayers(const std::set<uint64_t>& ids);

    // The cell the layer plays as of the last step.
    std::optional<Play> playOf(const Layer& layer) const;
    // The layer's Autopilot in the deck it plays, as the Cues holding at this frame set it.
    Autopilot autopilotOf(const Layer& layer, const Composition& composition) const;
    // The column the layer's Autopilot picked for its next step, whether or not it is on; nullopt without a pilot.
    std::optional<uint64_t> pickedNext(uint64_t layer) const;
    // The column to preload for the layer's next step: 0 while the column Autopilot runs or the layer's is off.
    uint64_t layerNext(const Layer& layer, const Composition& composition) const;
    // The column the column Autopilot plays next, 0 when it is off.
    uint64_t columnNext() const;
    bool columnsRunning() const { return columnPilot_.has_value(); }

    // Every layer's pilot while its Autopilot is on, then the column Autopilot's as layer 0.
    std::vector<ResumePilot> pilots(const Composition& composition) const;
    Shown shown(const Layer& layer, const Composition& composition, double beat) const;
    Columns columns(const Composition& composition, double beat) const;

private:
    // Autopilot on a layer: the cell it plays, stepped on from the document's last trigger, and the column it picked
    // for the next step a whole step ahead, so that clip is preloaded by the time it starts.
    struct Pilot {
        std::optional<Play> trigger;
        Play play;
        double stepStart = 0.0;
        uint64_t next = 0;
        uint64_t resyncs = 0;
        bool held = false;
        double pace = 1.0;
    };

    // The column Autopilot: the column it triggered last, what that step played, the one it picked for the next step a
    // whole step ahead, so its clips are preloaded by the time it starts, and each layer's document trigger as last
    // seen, so a performer's trigger shows as a change.
    struct ColumnPilot {
        uint64_t column = 0;
        std::optional<Play> fired;
        uint64_t next = 0;
        double stepStart = 0.0;
        uint64_t resyncs = 0;
        bool held = false;
        double pace = 1.0;
        std::map<uint64_t, std::optional<Play>> triggers;
    };

    // The cell a column Cue started on a layer, kept while the layer's trigger stays the same.
    struct ColumnCue {
        std::optional<Play> trigger;
        Play play;
    };

    void restoreColumns(const Composition& composition, const ResumePilot& saved);
    double stepLength(const Layer& layer, const Composition& composition, const Pilot& pilot, double pace) const;
    uint64_t pickNext(const Layer& layer, const Composition& composition, Pilot& pilot);
    uint64_t pickColumn(const Composition& composition, const ColumnPilot& pilot);
    bool columnPlayable(const Composition& composition, uint64_t column);
    void keepBaseFilled(const Composition& composition, std::vector<uint64_t>& columns);
    // The cell the layer's last trigger, or a column Cue that fired after it, plays.
    std::optional<Play> triggered(const Layer& layer);

    const BeatClock& clock_;
    const CueState& cues_;
    const std::atomic<uint32_t>& seed_;
    Playable playable_;
    std::map<uint64_t, Pilot> pilots_;
    std::map<uint64_t, ColumnCue> columnCues_;
    // Set while the column Autopilot runs.
    std::optional<ColumnPilot> columnPilot_;
    double lastColumnCue_ = -kNever;
    uint64_t columnCueResyncs_ = 0;
};

} // namespace azl
