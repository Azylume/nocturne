#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace azl {

enum class ShaderStage { Vertex, Fragment, Compute };

// GLSL 4.50 for Vulkan to SPIR-V with glslang. Empty, with `error` set, when it does not compile. Any thread.
std::vector<uint32_t> glslToSpirv(ShaderStage stage, const std::string& glsl, std::string& error);

struct CrossResult {
    // Empty on failure.
    std::string text;
    std::string error;
};

// A compiled shader back to GLSL to paste into another: no #version, no entry point, no stage inputs or outputs, and
// every function, global variable, type and constant named `prefix` + its own name, so two pasted shaders never clash.
// Any thread.
CrossResult spirvToGlsl(std::vector<uint32_t> spirv, const std::string& prefix);

// One entry point of a Metal library, written as a GLSL shader with its own main.
struct ShaderEntry {
    std::string name;
    ShaderStage stage = ShaderStage::Fragment;
    std::string glsl;
};

// The entries through SPIR-V to one Metal Shading Language source, each in a namespace of its own name, so its
// function is mslFunction(name). Resources keep the indices their GLSL set and binding give them (shader_layout.hpp);
// samplers are fixed per SamplerKind. Any thread.
CrossResult glslToMsl(const std::vector<ShaderEntry>& entries);
// glslToMsl's source; throws with the compiler's error instead.
std::string mslSource(const std::vector<ShaderEntry>& entries);
std::string mslFunction(const std::string& entry);

// The fullscreen vertex shader, then the fragment entry points `names` of `glsl` (withEntry).
std::vector<ShaderEntry> fullscreenEntries(const std::string& glsl, const std::vector<std::string>& names);

} // namespace azl
