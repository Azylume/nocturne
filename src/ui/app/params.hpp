#pragma once

#include "document/composition.hpp"
#include "effects/effect_registry.hpp"

#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace azl::ui {

// The chain the panels edit: a layer's (0 is the composition) or, with a column, the clip's in that cell.
// No layer has this id: the Layer tab's owner with no layer selected.
constexpr uint64_t kNoLayer = std::numeric_limits<uint64_t>::max();

struct ChainOwner {
    uint64_t layer = 0;
    std::optional<uint64_t> column;

    bool operator==(const ChainOwner&) const = default;
};

// A layer's chain is the one of the deck in view: its own, or the layer's shared one.
const std::vector<Effect>* chainOf(const Composition& composition, const ChainOwner& owner);
// A layer's blend and Autopilot edited as the deck in view has them: its own value while it has one, else the shared.
Edit blendEdit(const Composition& composition, uint64_t layer, BlendMode blend);
Edit autopilotEdit(const Composition& composition, uint64_t layer, std::optional<AutopilotMode> mode,
                   std::optional<int> bars);
// Adds onto the chain chainOf shows.
AddEffect addEffectTo(const Composition& composition, const ChainOwner& owner, std::optional<size_t> index,
                      Effect effect);
// Adds them in order from `index` on (none: at the end).
std::vector<Edit> addEffectsTo(const Composition& composition, const ChainOwner& owner, std::optional<size_t> index,
                               const std::vector<Effect>& effects);
// Moves the effect onto the chain chainOf shows (to its end when `index` is past it), keeping its id; a copy is added
// there with a fresh id, so the macros and modulators on the original do not drive it. None when either is missing.
std::optional<Edit> moveEffectTo(const Composition& composition, uint64_t effect, const ChainOwner& owner,
                                 std::optional<size_t> index, bool copy);
// The chain holding the parameter's effect, wherever it sits, or for a layer's own parameter the layer's.
std::optional<ChainOwner> ownerOf(const Composition& composition, const ParamRef& ref);

// A parameter the panels show: a layer's opacity or an effect parameter, with its range and its base value.
struct ParamInfo {
    ParamRef ref;
    std::string label;
    double min = 0.0;
    double max = 1.0;
    double defaultValue = 0.0;
    double value = 0.0;
    ParamUnit unit = units::percent;
    bool wraps = false;
    // An effect parameter's definition; null for an opacity.
    const ParamDef* def = nullptr;

    double fraction(double v) const { return max > min ? (v - min) / (max - min) : 0.0; }
    double at(double fraction) const { return min + fraction * (max - min); }
    // Within the range: wrapped round when the parameter wraps, else clamped.
    double fit(double v) const;
    // The value in the parameter's unit, e.g. "-45\xc2\xb0".
    std::string format(double v) const { return formatParam(unit, fit(v)); }
    // A choice's options by name, which the row offers as a dropdown; empty for other parameters.
    const std::vector<std::string>& options() const;
    // The name a tooltip shows: the whole one when the label shortens it.
    std::string fullLabel() const { return def && !def->fullLabel.empty() ? def->fullLabel : label; }
};

std::optional<ParamInfo> paramInfo(const Composition& composition, const ParamRef& ref);
// "Layer 1 / colorize / hue": the owner, the effect and the parameter.
std::string paramLabel(const Composition& composition, const ParamRef& ref);
Edit setParam(const ParamRef& ref, double value);

// The modulator that animates the parameter: the first one driving it directly, not through a macro.
const Modulator* animationOf(const Composition& composition, const ParamRef& ref);
// "beat" or an audio feature: kick, snare, hat or bass.
std::string animationSource(const Modulator& modulator);

// A beat LFO swings an angle a quarter turn either way of its value, sweeps a share (0..100%) over its whole range and
// anything else an eighth of its range either way; an audio source rises from the value to where the LFO's upper end
// would be.
Modulator newAnimation(const ParamInfo& info, const std::string& source);
// The same with Auto range on for an audio source, sweeping the parameter's usual range (animationRange) where it has
// one: what Animate makes.
Modulator newAnimation(const Composition& composition, const ParamInfo& info, const std::string& source);
Modulator withSource(const Modulator& modulator, const ParamInfo& info, const std::string& source);

// A range of a parameter as fractions of its span, from the base value plus a source's min..max offsets.
struct Range {
    double from = 0.0;
    double to = 1.0;
};
Range rangeOf(const ParamInfo& info, double min, double max);
// The offsets that make `range` from the parameter's base value.
std::pair<double, double> offsetsOf(const ParamInfo& info, Range range);
// An animation's ends in the parameter's values: its value plus the source's offsets (from: min, to: max). A wrapping
// parameter's ends may lie past its range; they read wrapped.
struct Ends {
    double from = 0.0;
    double to = 1.0;
};
Ends endsOf(const ParamInfo& info, double min, double max);
// The offset that puts an end at `value`, at most one span either way; for a wrapping parameter the one a whole number
// of turns nearest `previous`, so an end dragged through the range's end keeps going round.
double offsetTo(const ParamInfo& info, double value, double previous);
// Where the modulator puts its target, as a fraction of the target's range, while its source is at `value` (0..1).
double modulatedAt(const Composition& composition, const Modulator& modulator, double value);

// The eight composition macros in slot order; a slot past the composition's macros is null.
std::vector<const Macro*> macroSlots(const Composition& composition);
// The edits that create every macro up to `slot` that is missing, in order, and the id the slot's macro has.
std::pair<std::vector<Edit>, uint64_t> ensureMacro(const Composition& composition, int slot);
// A new target sweeping the parameter from its value towards the end of its range that is further away.
MacroTarget newTarget(const ParamInfo& info);
const Modulator* macroModulator(const Composition& composition, uint64_t macro);

inline constexpr int kMacroSlots = 8;

// Bezier control points of the curves offered by name.
struct NamedCurve {
    const char* name;
    Curve curve;
};
const std::vector<NamedCurve>& namedCurves();
std::string curveName(const Curve& curve);

} // namespace azl::ui
