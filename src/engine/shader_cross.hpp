#pragma once

#include <string>

namespace azl {

struct CrossResult {
    // Empty on failure.
    std::string msl;
    std::string error;
};

// A Vulkan GLSL fragment shader to Metal Shading Language: GLSL to SPIR-V with glslang, SPIR-V to MSL with
// SPIRV-Cross. Any thread.
CrossResult fragmentGlslToMsl(const std::string& glsl);

} // namespace azl
