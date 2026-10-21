#pragma once

#include "audio/audio_analyzer.hpp"
#include "document/composition.hpp"
#include "effects/chain_planner.hpp"
#include "effects/colour.hpp"
#include "engine/envelope.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace azl {

// Where an effect with steps is: the step it moves to, from `beat` on the beat clock.
struct StepPosition {
    uint64_t effect = 0;
    size_t step = 0;
    double beat = 0.0;
};

// The document's macros and modulators resolved for the render thread. It is built off the render thread whenever the
// document changes; evaluating a frame then needs no allocation, no lock and no lookup by name, and modulated values
// reach the shaders as parameter uniforms, so nothing ever recompiles. Targets whose layer or effect is gone are
// ignored.
class ModulationPlan {
public:
    explicit ModulationPlan(const Composition& composition);

    bool usesAudio() const { return usesAudio_; }

    // Continues the audio sources' attack and release, the beat LFOs' cycles and the effects' phases from where the
    // plan this one replaces left them, by modulator and effect id, so an effect moved to another chain keeps its
    // phases and an LFO whose period changes keeps its place in the cycle. Render thread only,
    // before this plan's first evaluate.
    void carry(const ModulationPlan& previous);

    // Render thread only: the macros these Cues set take their values in place of the document's until the next call.
    void cue(const std::vector<CueMacro>& macros);
    // Render thread only: the effects move to these steps (by effect id); an effect not listed keeps the document's
    // values. A move glides over the effect's transition from wherever its parameters are: numbers straight (the short
    // way round when they wrap), colours in OKLab, choices switching halfway. The stepped value is the base that
    // macros and modulators drive. A live edit of a parameter takes it over until the next move. The palettes move as
    // the effect kPaletteEffect: every colour linked to a palette slot crossfades in OKLab to that slot's colour in
    // the palette moved to, over the palette transition; the palette position and the slots' hue targets move the
    // colours the crossfade heads for.
    void step(const std::vector<StepPosition>& positions);

    // Render thread only: computes every driven value at engine time `seconds` and `beat` on the beat clock from the
    // audio analysis.
    void evaluate(double seconds, double beat, const AudioFrame& audio);
    float opacity(uint64_t layer, float base) const;
    // A text parameter of the clip the layer plays, driven from that clip's own value.
    float text(uint64_t layer, TextParam param, float base) const;
    // Overwrites the modulated and derived parameters in `values`, which holds the pass's stage parameters in the order the shader
    // reads them.
    void apply(const PlannedPass& pass, const std::vector<const Effect*>& effects, std::vector<float>& values) const;
    // Whether a macro, a modulator, a drive other than a plain value, steps or a palette move any of the effect's
    // parameters.
    bool drives(uint64_t effect) const;
    // Then puts back, in place of each derived parameter, the value its drive derives from (a phase's rate).
    void rates(const PlannedPass& pass, const std::vector<const Effect*>& effects, std::vector<float>& values) const;

    // Any thread: the values as of the last evaluated frame.
    struct MacroValue {
        uint64_t id;
        double value;
    };
    struct ParamValue {
        ParamRef param;
        double value;
    };
    std::vector<MacroValue> macros() const;
    std::vector<ParamValue> params() const;
    // Each modulator's source (0..1) before its curve.
    std::vector<MacroValue> modulators() const;
    // Each palette slot's colour as the last frame drew it.
    std::vector<Rgba> paletteColours() const;
    // Render thread only: the colour a colour linked to this palette colour shows; none when no palette holds it.
    std::optional<Rgba> linkedColour(uint64_t id) const;
    // The modulators whose target parameter does not exist, in document order.
    const std::vector<uint64_t>& dangling() const { return dangling_; }

    static constexpr size_t kCurvePoints = 65;

private:
    using Lut = std::array<float, kCurvePoints>;

    struct Slot {
        double base = 0.0;
        double min = 0.0;
        double max = 1.0;
        double add = 0.0;
        double mul = 1.0;
        double value = 0.0;
        bool wraps = false;
        // A choice's parameter: the value steps between its options.
        const ParamDef* options = nullptr;
    };
    struct Source {
        uint64_t id = 0;
        ModulatorSource kind = ModulatorSource::Lfo;
        LfoShape shape = LfoShape::Sine;
        double beats = 1.0;
        double phase = 0.0;
        size_t feature = 0;
        double sensitivity = 1.0;
        EnvelopePreset envelope = EnvelopePreset::None;
        // Seconds.
        EnvelopeTimes times;
        Combine combine = Combine::Add;
        Lut curve{};
        double min = 0.0;
        double max = 1.0;
        size_t slot = 0;
        std::vector<LfoPoint> points;
    };
    struct Link {
        size_t macro = 0;
        size_t slot = 0;
        Lut curve{};
        double min = 0.0;
        double max = 1.0;
    };
    struct EffectParam {
        uint64_t effect = 0;
        size_t index = 0;
        size_t slot = 0;
    };
    // A parameter the shader receives as its drive derives it.
    struct Derived {
        uint64_t effect = 0;
        size_t index = 0;
        ParamDrive drive = ParamDrive::Value;
        // The parameter's value and its slot when it is modulated; then the same for the parameter that scales it.
        double base = 0.0;
        size_t slot = SIZE_MAX;
        double scaleBase = 1.0;
        size_t scaleSlot = SIZE_MAX;
        double value = 0.0;
        double rate = 0.0;
        bool started = false;
    };
    // A continuous parameter set by hand within two frames of its previous change moves to the new value over that
    // time, so a drag the UI sends at 30 Hz moves the picture every frame; any other change lands at once.
    struct Glide {
        uint64_t effect = 0;
        size_t index = 0;
        double base = 0.0;
        double from = 0.0;
        double value = 0.0;
        // When the base last changed; negative before.
        double changed = -1.0;
        double start = 0.0;
        double duration = 0.0;
        bool pending = false;
        // Glides the short way round between `min` and `max`.
        bool wraps = false;
        double min = 0.0;
        double max = 1.0;
    };
    struct Opacity {
        uint64_t layer = 0;
        size_t slot = 0;
    };
    // A parameter an effect's steps set: each step's value (NaN where a step leaves it), the document's, where the
    // move onto the current step started from, the stepped base and the base the frame drew.
    struct Stepped {
        uint64_t effect = 0;
        size_t index = 0;
        ParamRef param;
        std::vector<double> values;
        double doc = 0.0;
        double from = 0.0;
        double base = 0.0;
        double drawn = 0.0;
        bool manual = false;
        bool choice = false;
        bool wraps = false;
        // A channel of a colour linked to a palette slot.
        bool palette = false;
        double min = 0.0;
        double max = 1.0;
        size_t slot = SIZE_MAX;
        size_t glide = SIZE_MAX;
        size_t derived = SIZE_MAX;
    };
    // A colour linked to a palette slot: its channels in `stepped_` (alpha none when it has none), and where the
    // crossfade onto the palette it is on started.
    struct PaletteLink {
        std::array<size_t, 3> channels{};
        size_t alpha = SIZE_MAX;
        ColourModel model = ColourModel::Rgb;
        size_t slot = 0;
        Rgba from;
    };
    // The palettes' colours by palette and slot (a palette shorter than the longest takes the active one's), the
    // palette the colours are on and where the crossfade onto it started, each slot's colour then and as drawn, and
    // the palette targets' slots.
    struct Palettes {
        std::vector<std::vector<Rgba>> colours;
        size_t active = 0;
        double beats = 0.0;
        size_t step = 0;
        double moved = 0.0;
        std::optional<StepPosition> target;
        std::vector<Rgba> from;
        std::vector<Rgba> drawn;
        std::vector<PaletteLink> links;
        // Palette colour id to slot.
        std::vector<std::pair<uint64_t, size_t>> ids;
        size_t position = SIZE_MAX;
        // Palette slot to the slot of its hue target.
        std::vector<std::pair<size_t, size_t>> hues;
    };
    struct StepColour {
        std::array<size_t, 3> channels{};
        ColourModel model = ColourModel::Rgb;
    };
    // An effect with steps: its parameters in `stepped_` from `first`, the step it is on and where the move onto it
    // started, and the position the stepper gave last.
    struct StepGroup {
        uint64_t effect = 0;
        double beats = 0.0;
        size_t first = 0;
        size_t count = 0;
        std::vector<StepColour> colours;
        std::optional<size_t> step;
        double moved = 0.0;
        std::optional<StepPosition> target;
    };
    struct Text {
        uint64_t layer = 0;
        TextParam param = TextParam::Size;
        size_t slot = 0;
    };

    double follow(size_t source, double input, double dt, const AudioFrame& audio);
    void buildSteps(const Composition& composition);
    // Finds the parameter's macro slot, hand glide and derived value.
    void attach(Stepped& p) const;
    void buildPalettes(const Composition& composition);
    void glideSteps(double beat);
    void glidePalettes(double beat);
    // Where the slot's colour heads on the palette `step`: that palette's, or the palette position's blend, turned by
    // the slot's hue target.
    Rgba paletteTarget(size_t slot, size_t step) const;
    Rgba drawnColour(const PaletteLink& link) const;
    void publishPalettes();
    void mixColours(const StepGroup& group, double t);
    // The value the step gives the parameter: its own, or the document's where the step leaves it.
    static double target(const Stepped& p, std::optional<size_t> step);
    // `t` of the way from where the move started to `to`: straight, the short way round, or a switch halfway.
    static double glided(const Stepped& p, double to, double t);
    double stepBase(const Stepped& p) const { return p.manual ? p.doc : p.base; }
    double drawnBase(const Stepped& p) const;
    // What the shader receives for a stepped parameter.
    double outputOf(const Stepped& p) const;
    static double fit(const Slot& slot, double value);
    static Lut lut(const Curve& curve);
    static double at(const Lut& lut, double x);

    std::vector<Slot> slots_;
    std::vector<uint64_t> macroIds_;
    std::vector<uint64_t> dangling_;
    // The document's value of each macro.
    std::vector<double> macroBases_;
    std::vector<ParamRef> paramRefs_;
    std::vector<Source> sources_;
    std::vector<Link> links_;
    std::vector<EffectParam> effectParams_;
    std::vector<Derived> derived_;
    double derivedTime_ = 0.0;
    std::vector<Glide> glides_;
    std::vector<Opacity> opacities_;
    std::vector<Text> texts_;
    std::vector<Stepped> stepped_;
    // Indices into `stepped_` by effect id.
    std::vector<size_t> steppedByEffect_;
    std::vector<StepGroup> stepGroups_;
    Palettes palettes_;
    bool usesAudio_ = false;
    // Slots, then sources, then stepped parameters, then each palette slot's red, green, blue and alpha.
    std::unique_ptr<std::atomic<float>[]> published_;
    std::vector<double> sourceValues_;
    std::vector<EnvelopeState> envelopes_;
    // Each beat LFO's periods so far, advanced by the beats between frames; NaN until it starts.
    std::vector<double> cycles_;
    double beat_ = 0.0;
    double audioTime_ = -1.0;
};

} // namespace azl
