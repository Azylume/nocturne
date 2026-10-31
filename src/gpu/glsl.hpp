#pragma once

#include "engine/shader_cross.hpp"
#include "gpu/gpu.hpp"

#include <string>
#include <vector>

namespace azl::gpu {

// GLSL entry points compiled for the backend built in: one MSL source on Metal, SPIR-V per entry on Vulkan. Any thread
// but the render thread. Throws with the compiler's error when one does not compile.
ShaderSource compileGlsl(const std::vector<ShaderEntry>& entries);

// The function name a pipeline gives for entry `entry` of a module made from compileGlsl.
std::string entryPoint(const std::string& entry);

} // namespace azl::gpu
