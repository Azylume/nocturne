#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace azl {

// Tells the chain planner how a stage may fuse with its neighbours (ADR 0009).
enum class StageKind { Pointwise, Remap, Neighbourhood, MultiPass };

// What the shader receives for a parameter: its value, or something the CPU derives from it each frame.
enum class ParamDrive {
    Value,
    // The value integrated over seconds, times the parameter `scaledBy` names when it names one, so a rate moves its
    // motion smoothly when it changes.
    Phase,
    // How many periods of this many beats (at least a quarter beat) the beat clock has counted; 0 when it is 0.
    Beats,
    // The level (0..1) of the audio feature with this index, in AudioFeature order.
    Audio,
};

// How a value reads: times `scale`, with `decimals` decimals, then `suffix`.
struct ParamUnit {
    std::string suffix;
    double scale = 1.0;
    int decimals = 2;
};

namespace units {
inline const ParamUnit plain{};
inline const ParamUnit whole{"", 1.0, 0};
inline const ParamUnit percent{"%", 100.0, 0};
inline const ParamUnit degrees{"\xc2\xb0", 1.0, 0};
// A share of a turn shown in degrees.
inline const ParamUnit turns{"\xc2\xb0", 360.0, 0};
inline const ParamUnit times{"x", 1.0, 2};
inline const ParamUnit strips{" strips", 1.0, 1};
inline const ParamUnit stripsPerSecond{" strips/s", 1.0, 2};
inline const ParamUnit beats{" beats", 1.0, 2};
inline const ParamUnit hertz{" Hz", 1.0, 2};
inline const ParamUnit lines{" lines", 1.0, 1};
inline const ParamUnit linesPerSecond{" lines/s", 1.0, 2};
inline const ParamUnit cells{" cells", 1.0, 1};
inline const ParamUnit cellsPerSecond{" cells/s", 1.0, 2};
inline const ParamUnit perSecond{"/s", 1.0, 2};
} // namespace units

struct ParamDef {
    std::string name;
    double min = 0.0;
    double max = 1.0;
    double defaultValue = 0.0;
    ParamDrive drive = ParamDrive::Value;
    std::string scaledBy;
    // Shown instead of the name when set.
    std::string label;
    // Whole values pick between alternatives, so a new value takes effect at once rather than gliding there.
    bool choice = false;
    ParamUnit unit{};
    // The value is periodic over its range (an angle or a hue): it wraps around instead of stopping at an end.
    bool wraps = false;
    // The names of the alternatives a choice picks between, in order, spread evenly over the range.
    std::vector<std::string> options{};
    // Where each option after the first starts, as a share of the range, when not halfway between two option values.
    std::vector<double> optionStarts{};
    // The whole name when `label` shortens it.
    std::string fullLabel{};
    // Parameters of the same group fold under one header in an effect's card.
    std::string group{};
};

// "-45\xc2\xb0", "2.30 strips/s".
std::string formatParam(const ParamUnit& unit, double value);
// The value brought into the range: wrapped around when the parameter wraps, else clamped.
double fitParam(const ParamDef& param, double value);
double wrapInto(double value, double min, double max);
// The option a value picks, as the shaders read it: the nearest of the options spread evenly over the range, or the last
// whose start it reaches.
int optionIndex(const ParamDef& param, double value);
// The value that picks the option: min + index x (max - min) / (options - 1).
double optionValue(const ParamDef& param, int index);

// A pass a multi-pass effect renders before the layer's pass combines its result. `metal` is the body of
//   float4 fxs_TYPE_i(float2 px, texture2d<float> prev, texture2d<float> side0, ..., ARGS, constant float* p)
// where px is this target's pixel, side0.. are the targets of the effect's earlier side passes this frame, and
// INPUT(q) is the effect's input at composition pixel q, with the stages before it fused in.
struct SidePass {
    // The target is the composition size divided by this.
    uint32_t divisor = 1;
    // The target keeps its content across frames: two textures swap roles each frame and `prev` is last frame's.
    bool persistent = false;
    std::string metal;
};

// A shader that replaces the effect's own while any of `params` is on (off its default, or passing `when` if set, or
// driven by a macro or a modulator), so a chain pays for a feature only while it uses it. It compiles as its own
// effect type, TYPE_NAME, with the same parameters and `kind` if set; switching variants compiles the chain anew.
struct EffectVariant {
    std::string name;
    std::vector<std::string> params;
    std::string metal;
    bool (*when)(double value) = nullptr;
    std::optional<StageKind> kind{};
};

// `metal` is the body of one Metal function whose signature the planner writes from the kind:
//   Pointwise:     float4 fx_TYPE(float4 c, float2 px, constant Header& h, constant float* p)
//   Remap:         float2 fx_TYPE(float2 px, constant Header& h, constant float* p[, constant float* q]), returning the
//                  input pixel to read; q when it derives values
//   Neighbourhood: float4 fx_TYPE(texture2d<float> t, float2 px, constant Header& h, constant float* p), sampling t
//                  with `pix`; with derived values, a last argument `constant float* q` holds them
//   MultiPass:     float4 fx_TYPE(float4 c, float2 px, constant Header& h, texture2d<float> a, constant float* p), where
//                  a is the target of its last side pass
// px are composition pixel coordinates with centres at .5; p holds the parameters in declaration order, each as its
// drive derives it; h.size is the composition size, h.time its clock in seconds and h.beat the beat clock's beats.
struct EffectDef {
    std::string type;
    StageKind kind = StageKind::Pointwise;
    // The registry appends "mix" (dry/wet) to every effect as its last parameter.
    std::vector<ParamDef> params;
    // Intermediates are 8-bit unless a stage needs headroom. For a multi-pass effect it applies to its side targets.
    bool needsFloat16 = false;
    std::string metal;
    std::vector<SidePass> sidePasses;
    // Checked in order; the first with a parameter on wins.
    std::vector<EffectVariant> variants{};
    // Empty for our own effects; "Classic" for the pack that mirrors a common VJ app's controls, "ISF" for imported
    // shaders.
    std::string category{};
    // A source instead of an effect: it ignores its input and draws a picture of its own. It sits first in a
    // generator clip's chain, a clip with no media and no text.
    bool generator = false;
    // A variant of another effect, which the planner picks; never offered on its own.
    bool variant = false;
    // For a pointwise effect: whether, with these parameter values as the shader reads them, an input with no red, green
    // or blue (whatever its alpha) comes out with none, so that where its input is black it adds nothing under Add or
    // Screen. Null when not known.
    bool (*keepsBlack)(const float* p) = nullptr;
    // For a pointwise effect: the body of
    //   float mask_TYPE(float2 px, constant Header& h, constant float* p, constant float* q)
    // which the stage evaluates first and hands to `metal` as a last argument `float m`. q holds the derived values.
    std::string mask{};
    // Values the CPU works out once a frame from the parameters (p, as the shader reads them; `rates` has a driven
    // parameter's own value instead, a phase's rate), the composition time in seconds and the beat clock's beats,
    // `derived` of them, which follow every stage's parameters in the pass's buffer.
    void (*derive)(const float* p, const float* rates, float time, float beat, float* out) = nullptr;
    size_t derived = 0;
    // Whether, with these parameter values, the result has no red, green or blue wherever the mask is 0, whatever the
    // input, at full dry/wet.
    bool (*blackOutsideMask)(const float* p) = nullptr;
    // Shown instead of the label the type spells when set.
    std::string label{};
    // Metal at file scope that the effect's functions use, written once before them.
    std::string globals{};
    // The type is no longer registered.
    bool retired = false;
};

// Null for an unknown type. The pointer stays valid for the life of the process.
const EffectDef* findEffect(const std::string& type);

// Every registered effect type, our own before the categories, otherwise in registration order; no generators.
std::vector<std::string> effectTypes();
// Every generator type in registration order.
std::vector<std::string> generatorTypes();
// The type as people read it: "classicShiftRgb" is "Classic Shift RGB", or the effect's own label.
std::string effectLabel(const std::string& type);
bool isGenerator(const std::string& type);

// Any thread: registers the effect, replacing one of the same type for later lookups. Pointers to the one replaced stay
// valid.
void registerEffect(EffectDef effect);
// Any thread: later lookups of the type find nothing.
void retireEffect(const std::string& type);
// Start-up and tests only: gives a registered effect its shader.
void setEffectShader(const std::string& type, std::string globals, std::string metal);

// A hash of every effect type with its parameters' names and ranges, apart from the user's own shaders (which the
// engine sends its clients): a UI and an engine that agree on it understand each other's effect edits.
std::string effectRegistryVersion();

} // namespace azl
