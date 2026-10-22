#pragma once

#include "document/composition.hpp"
#include "effects/effect_registry.hpp"

#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace azl {

// An Interactive Shader Format fragment shader: a JSON header in the first comment, then GLSL.
enum class IsfKind { Effect, Generator, Transition };

// The GLSL value an ISF input becomes, made from consecutive parameters.
enum class IsfInputType { Float, Bool, Long, Colour, Point };

struct IsfInput {
    std::string name;
    IsfInputType type = IsfInputType::Float;
    // Long: the values its options stand for.
    std::vector<int> values{};
};

struct IsfShader {
    std::string name;
    // The effect type, or for a transition the document's name of its mode.
    std::string type;
    bool user = false;
    IsfKind kind = IsfKind::Effect;
    // An effect that reads its input only at its own pixel fuses as a pointwise stage; a transition must, since it mixes
    // the two pictures pixel by pixel.
    bool pointwise = false;
    std::vector<IsfInput> inputs;
    // The inputs as parameters, in order: a colour as NAMERed, NAMEGreen, NAMEBlue, NAMEAlpha, a point as NAMEX, NAMEY.
    std::vector<ParamDef> params;
    // The GLSL after the header.
    std::string glsl;
};

struct IsfSource {
    std::string name;
    std::string text;
    // From the engine machine's user folder rather than built into the program.
    bool user = false;
};

// The most parameter values a transition takes: they travel in its mix uniforms.
inline constexpr size_t kMaxTransitionParams = 16;
// The most values a long input offers.
inline constexpr size_t kMaxLongValues = 256;

// The shader or what is wrong with it.
std::variant<IsfShader, std::string> parseIsf(const IsfSource& source);

// "Chromatic Aberration" is "isfChromaticAberration".
std::string isfType(const std::string& name);
// A user shader's type: "My Wipe" is "userMyWipe"; empty when the name has no letters or digits.
std::string userIsfType(const std::string& name);

// The effect a parsed effect or generator registers as, with no shader yet: the engine translates it (isf_compiler).
EffectDef isfEffect(const IsfShader& shader);

// The shaders built into the program (third_party/isf), by file name.
const std::vector<IsfSource>& isfSources();
// isfSources() parsed, once, in the same order.
const std::vector<std::variant<IsfShader, std::string>>& parsedIsfSources();
// The effects and generators among them, as isfEffect makes them; a source that does not parse is left out.
std::vector<EffectDef> isfEffects();

// An ISF transition as a transition mode: its inputs as parameters and, once the engine has translated it, its Metal:
// `globals` at file scope and `function` the body of
//   float4 isfTransitionN(float4 from, float4 to, uint2 p, constant Header& h, float progress, constant float* tp)
// for the mode's number N, mixing the two premultiplied pictures at pixel p with the parameter values tp.
struct IsfTransition {
    TransitionMode mode;
    std::string name;
    std::vector<ParamDef> params;
    bool user = false;
    std::string globals{};
    std::string function{};
};
using IsfTransitions = std::vector<IsfTransition>;

// The built-in transitions, then the user folder's. Any thread; a chain keeps the set it was planned with.
std::shared_ptr<const IsfTransitions> isfTransitions();
// Replaces the set. Any thread.
void setIsfTransitions(IsfTransitions transitions);
// The built-in transitions as parsed, with no Metal.
IsfTransitions builtinIsfTransitions();
// The transition mode a parsed transition stands for: a built-in one's, or a user one's (userTransitionMode).
IsfTransition isfTransition(const IsfShader& shader);
// Null when the mode is not an ISF transition of the set.
const IsfTransition* findIsfTransition(const IsfTransitions& transitions, TransitionMode mode);
// A client's copy of the engine's user shaders: registers the effects, generators and transitions of `sources` (user
// sources, with no Metal) in place of those of the previous call; one the process already has with its Metal, as an
// engine in the same process registered it, is left alone. Any thread.
void mirrorUserIsf(const std::vector<IsfSource>& sources);

// The values the shader reads for the layer's values of the transition, each a default where unset, clamped to its
// input's range.
std::vector<float> transitionParamValues(const IsfTransition& transition, const std::map<std::string, double>& values);

} // namespace azl
