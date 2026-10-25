#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azl {

// A cubic Bezier from (0, 0) to (1, 1) through two control points, as CSS cubic-bezier(x1, y1, x2, y2). The x of both
// control points is within 0..1, so the curve is a function of x; y may overshoot within -1..2.
struct Curve {
    std::array<double, 4> points{0.0, 0.0, 1.0, 1.0};

    bool operator==(const Curve&) const = default;
};

// The curve's y at x (clamped to 0..1).
double curveAt(const Curve& curve, double x);

// A parameter that macros and modulators drive: one of a layer's own (effect 0, e.g. param "opacity"; layer 0 is the
// composition) or an effect's, by the effect's id alone (layer 0), wherever the effect sits.
struct ParamRef {
    uint64_t layer = 0;
    uint64_t effect = 0;
    std::string param;

    bool operator==(const ParamRef&) const = default;
};

// Palette targets, of layer 0 with no effect: the palette position (0..1 across the palettes, blending neighbours) and
// a palette slot's hue rotation in turns ("paletteHue1" turns the first slot's colour).
inline constexpr std::string_view kPalettePosition = "palettePosition";
inline constexpr std::string_view kPaletteHue = "paletteHue";
// The 0-based slot a "paletteHueN" target turns.
std::optional<size_t> paletteHueSlot(std::string_view param);
std::string paletteHueParam(size_t slot);

// A driven value is its base plus every offset driving it, times every factor, clamped to its range. A source maps its
// 0..1 value through its curve onto min..max (in the target's units) to make that offset or factor. The document keeps
// only the base; the engine computes the rest every frame.
struct MacroTarget {
    ParamRef param;
    Curve curve;
    double min = 0.0;
    double max = 1.0;

    bool operator==(const MacroTarget&) const = default;
};

// A named knob (0..1) on the composition (group 0) or on a group, driving its targets.
struct Macro {
    uint64_t id = 0;
    std::string name;
    uint64_t group = 0;
    double value = 0.0;
    std::vector<MacroTarget> targets;

    bool operator==(const Macro&) const = default;
};

inline constexpr size_t kMaxMacros = 8;
inline constexpr size_t kMaxMacroTargets = 32;
inline constexpr size_t kMaxModulators = 64;

// Beat LFOs run on the beat clock, so they follow tempo, nudge and resync. Each peaks on the beat at phase 0: sine
// eases 1 -> 0 -> 1, saw falls 1 -> 0, ramp rises 0 -> 1, square is 1 for the first half of its period. Random holds
// a value for each period, the same on every engine for the same beat. Custom follows the modulator's points.
enum class ModulatorSource { Lfo, Audio };
enum class LfoShape { Sine, Saw, Square, Ramp, Random, Custom };
enum class Combine { Add, Multiply };

inline constexpr double kMinLfoBeats = 1.0 / 16.0;
inline constexpr double kMaxEnvelopeMs = 10000.0;
inline constexpr double kMaxLfoBeats = 64.0;

// A point of a custom LFO curve: where it sits in the period (0..1, below 1) and its value (0..1). The curve runs from
// each point to the next, the last one wrapping round to the first; straight, or eased when the point is smooth.
struct LfoPoint {
    double phase = 0.0;
    double value = 0.0;
    bool smooth = false;

    bool operator==(const LfoPoint&) const = default;
};

inline constexpr size_t kMaxLfoPoints = 16;

// The Low/Mid and Mid/High edges in Hz of the adjustable audio bands (features lowBand, midBand and highBand). The
// defaults are edges of the analyser's fixed bands: Low is sub and bass, Mid lowMid and mid, High highMid and high.
inline constexpr std::array<double, 2> kDefaultBandEdges{150.0, 2000.0};
inline constexpr double kMinBandEdge = 40.0;
inline constexpr double kMaxBandEdge = 12000.0;
// Mid spans at least a third of an octave.
inline constexpr double kMinMidBandRatio = 1.25;
bool validBandEdges(const std::array<double, 2>& edges);

// An audio modulator's envelope. None is the law of shows from before presets: attack and release are the time
// constants of a one-pole smoother, both 0 follows the feature raw. The presets and Custom follow like a level meter
// (engine/envelope.hpp): attack is the time to 90 % of a rise, hold how long a peak stays, release the time to fall by
// half (6 dB); Gate jumps to 1 on each hit, holds and decays to 0 within its release.
enum class EnvelopePreset { None, Meter, Punchy, Smooth, Breathe, Gate, Custom };

// A moving source driving a macro (macro != 0) or a parameter.
struct ModTarget {
    uint64_t macro = 0;
    ParamRef param;

    bool operator==(const ModTarget&) const = default;
};

struct Modulator {
    uint64_t id = 0;
    std::string name;
    ModulatorSource source = ModulatorSource::Lfo;
    LfoShape shape = LfoShape::Sine;
    // The custom curve, in phase order; kept when another shape is chosen.
    std::vector<LfoPoint> points;
    // The LFO's period in beats and its offset in periods.
    double beats = 1.0;
    double phase = 0.0;
    // The audio feature by name: level, sub, bass, lowMid, mid, highMid, high, kick, snare, hat, or the adjustable
    // lowBand, midBand or highBand.
    std::string feature = "kick";
    // The audio envelope's times in milliseconds, as its preset reads them.
    double attack = 0.0;
    double release = 0.0;
    double hold = 0.0;
    EnvelopePreset envelope = EnvelopePreset::None;
    // An audio modulator maps its feature's running range (engine/auto_range.hpp) onto min..max instead of raw 0..1,
    // so it swings the same at any input level. Off unless set: a show saved without it keeps its raw mapping; the UI
    // and nocturne-ctl turn it on for the audio modulators they create.
    bool autoRange = false;
    ModTarget target;
    Combine combine = Combine::Add;
    Curve curve;
    double min = 0.0;
    double max = 1.0;

    bool operator==(const Modulator&) const = default;
};

// The LFO's value (0..1) at `phase` periods; Custom reads `points` (0 without any).
double lfoAt(LfoShape shape, double phase, const std::vector<LfoPoint>& points = {});

// "meter", "punchy", "smooth", "breathe", "gate" or "custom"; None is "none".
const char* envelopePresetName(EnvelopePreset preset);
std::optional<EnvelopePreset> envelopePresetFromName(std::string_view name);
// Puts the modulator on the preset with the preset's attack, hold and release; Custom and None keep its times.
void setEnvelope(Modulator& modulator, EnvelopePreset preset);

// Modulation edits. Applying them completes them: zero ids get fresh ones, a missing index means the end.
struct AddMacro {
    std::optional<size_t> index;
    Macro macro;
};

struct RemoveMacro {
    uint64_t macro = 0;
};

struct SetMacroValue {
    uint64_t macro = 0;
    double value = 0.0;
};

struct AddMacroTarget {
    uint64_t macro = 0;
    std::optional<size_t> index;
    MacroTarget target;
};

struct RemoveMacroTarget {
    uint64_t macro = 0;
    size_t index = 0;
};

// Replaces the target at `index`: its parameter, curve and range.
struct SetMacroTarget {
    uint64_t macro = 0;
    size_t index = 0;
    MacroTarget target;
};

struct AddModulator {
    std::optional<size_t> index;
    Modulator modulator;
};

struct RemoveModulator {
    uint64_t modulator = 0;
};

// Replaces the modulator with the same id.
struct SetModulator {
    Modulator modulator;
};

// Replaces every macro and modulator at once, as loading a Macro Set and undoing it do. Targets on a parameter their
// effect does not have are kept, so a show's stale ones never block either.
struct SetMacros {
    std::vector<Macro> macros;
    std::vector<Modulator> modulators;
};

} // namespace azl
