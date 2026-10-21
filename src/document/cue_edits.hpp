#pragma once

#include "document/composition.hpp"

#include <nlohmann/json.hpp>

#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace azl {

Edit apply(Composition& composition, AddCue& edit);
Edit apply(Composition& composition, RemoveCue& edit);
Edit apply(Composition& composition, SetCue& edit);
Edit apply(Composition& composition, SetCueEnabled& edit);
Edit apply(Composition& composition, SetCueRepeat& edit);
Edit apply(Composition& composition, SetGigWindow& edit);
Edit apply(Composition& composition, SetTimeline& edit);
Edit apply(Composition& composition, CancelTrigger& edit);

nlohmann::json toJson(const AddCue& e);
nlohmann::json toJson(const RemoveCue& e);
nlohmann::json toJson(const SetCue& e);
nlohmann::json toJson(const SetCueEnabled& e);
nlohmann::json toJson(const SetCueRepeat& e);
nlohmann::json toJson(const SetGigWindow& e);
nlohmann::json toJson(const SetTimeline& e);
nlohmann::json toJson(const CancelTrigger& e);

std::optional<Edit> cueEditFromJson(const std::string& op, const nlohmann::json& j);

void to_json(nlohmann::json& j, const Cue& cue);
void from_json(const nlohmann::json& j, Cue& cue);
void to_json(nlohmann::json& j, const GigWindow& window);
void from_json(const nlohmann::json& j, GigWindow& window);
void to_json(nlohmann::json& j, const Timeline& timeline);
void from_json(const nlohmann::json& j, Timeline& timeline);

inline constexpr double kNever = std::numeric_limits<double>::infinity();

// A Cue firing on `beat`, holding until `until` (never while no later Section is marked).
struct CueFiring {
    uint64_t cue = 0;
    double beat = 0.0;
    double until = kNever;

    bool operator==(const CueFiring&) const = default;
};

// The timeline at a beat: the Section playing since `start` (none before the first mark), the Cues that have fired in
// it in firing order (a repeating Cue by its last firing), and the values they hold, merged with the later Cue winning (a ramping macro at its value on the
// beat). Everything follows from the document
// and the beat alone, so every engine on the same beat clock holds the same values on the same beat.
struct CueState {
    std::optional<Section> section;
    double start = -kNever;
    double end = kNever;
    std::vector<CueFiring> active;
    std::vector<CueAutopilot> autopilot;
    std::vector<CueMacro> macros;
    // The columns the Cues fired, in firing order.
    struct ColumnFiring {
        CueColumn column;
        double beat = 0.0;
    };
    std::vector<ColumnFiring> columns;
    // The effects' Autopilot settings the Cues hold, and the steps they moved effects to, in firing order.
    std::vector<CueEffect> effects;
    struct StepFiring {
        uint64_t effect = 0;
        size_t step = 0;
        double beat = 0.0;
    };
    std::vector<StepFiring> steps;
    // Each cued macro's last Cue: from the value it held then to the Cue's value over `bars` bars.
    struct Ramp {
        uint64_t macro = 0;
        double from = 0.0;
        double to = 0.0;
        double fired = 0.0;
        int bars = 0;
    };
    std::vector<Ramp> ramps;
};

// Fills `state`, reusing its storage, so the render thread does not allocate once it has grown.
void cueStateAt(const Composition& composition, double beat, CueState& state);
// The firings after `beat`, in beat order: those of the Section playing and of the scheduled ones, a repeating Cue's
// next one in each. Skipped firings are left out.
std::vector<CueFiring> upcomingCues(const Composition& composition, double beat);
// The layer's Autopilot in `deck` with the Cues' values over it.
Autopilot cuedAutopilot(const Layer& layer, uint64_t deck, const std::vector<CueAutopilot>& cued);

// The effect's Autopilot with the Cues' values over it.
Autopilot cuedAutopilot(const Effect& effect, const std::vector<CueEffect>& cued);

// Marks `section` from `at` on: a mark already on that beat is replaced, and what is over by `now` (marks before the
// Section playing, skips before it started) is dropped.
Timeline markSection(const Timeline& timeline, Section section, double at, double now);
// After a resync at `beat`: the Section playing then starts on beat 0 and keeps its skips; scheduled marks are dropped.
Timeline rebasedTimeline(const Timeline& timeline, double beat);

} // namespace azl
