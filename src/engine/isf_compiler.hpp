#pragma once

#include "effects/isf.hpp"

#include <filesystem>
#include <string>

namespace azl {

// An ISF shader as GLSL to paste into a chain's pass: `globals` holds the compiled shader with every name prefixed
// isf_TYPE_, its entry point the function isf_TYPE_isf_run, and `body` calls that as the body of the effect's function
// (chain_planner) for its kind, or for a transition of
//   vec4 f(vec4 from, vec4 to, uvec2 p, float progress)
// with both pictures premultiplied at pixel p and the parameter values in the pass's FadeMix `m`.
struct IsfGlsl {
    std::string globals;
    std::string body;
    // Empty when it compiled.
    std::string error;
};

// The Vulkan GLSL the shader becomes: ISF's built-ins and IMG_ macros over globals that isf_run sets from its
// arguments before it calls the shader's main.
std::string isfGlsl(const IsfShader& shader);

// isfGlsl compiled to SPIR-V and back to prefixed GLSL. With `cacheDir`, the result is kept there by a hash of the GLSL
// and read back instead of compiling again.
IsfGlsl translateIsf(const IsfShader& shader, const std::filesystem::path& cacheDir);

// Gives the registered effect of an ISF effect or generator its shader, or one whose compile fails with the
// translation's error, so a chain using it reports that error. A transition joins the ISF transition modes.
void applyIsf(const IsfShader& shader, const IsfGlsl& glsl, const std::string& type);

// The registered effect of an ISF effect or generator with its shader, or with one whose compile fails with the
// translation's error.
EffectDef isfEffectDef(const IsfShader& shader, const IsfGlsl& glsl);

// Once per process, before any chain compiles: translates the built-in shaders and gives the registered ISF effects
// and the ISF transition modes their GLSL.
void prepareIsf(const std::filesystem::path& cacheDir);

} // namespace azl
