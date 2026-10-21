#pragma once

#include "effects/isf.hpp"

#include <filesystem>
#include <string>

namespace azl {

// An ISF shader as Metal: `globals` holds the translated shader in namespace isf_TYPE, and `body` calls it as the body
// of the effect's stage function (chain_planner) for its kind, or for a transition of
//   float4 f(float4 from, float4 to, uint2 p, constant Header& h, float progress, constant float* tp)
// with both pictures premultiplied at pixel p and the parameter values tp.
struct IsfMetal {
    std::string globals;
    std::string body;
    // Empty when it translated.
    std::string error;
};

// The Vulkan GLSL the shader becomes: its inputs as stage inputs, ISF's built-ins and IMG_ macros over them.
std::string isfGlsl(const IsfShader& shader);

// GLSL to SPIR-V to MSL. With `cacheDir`, the MSL is kept there by a hash of the GLSL and read back instead of
// translating again.
IsfMetal translateIsf(const IsfShader& shader, const std::filesystem::path& cacheDir);

// Gives the registered effect of an ISF effect or generator its shader, or one whose compile fails with the
// translation's error, so a chain using it reports that error. A transition joins the ISF transition modes.
void applyIsf(const IsfShader& shader, const IsfMetal& metal, const std::string& type);

// The registered effect of an ISF effect or generator with its shader, or with one whose compile fails with the
// translation's error.
EffectDef isfEffectDef(const IsfShader& shader, const IsfMetal& metal);

// Once per process, before any chain compiles: translates the built-in shaders and gives the registered ISF effects
// and the ISF transition modes their Metal.
void prepareIsf(const std::filesystem::path& cacheDir);

} // namespace azl
