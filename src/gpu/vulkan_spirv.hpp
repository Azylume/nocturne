#pragma once

#include "gpu/gpu.hpp"

#include <string>
#include <vector>

namespace azl::gpu::vk {

// What one SPIR-V entry point uses, after renumbering.
struct SpirvInterface {
    VkShaderStageFlagBits stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    std::vector<Binding> bindings;
    uint32_t localX = 1;
    uint32_t localY = 1;
};

// Moves the module's resources from shader_layout.hpp's sets to the backend's (gpu_vulkan_objects.hpp: buffers and
// textures into the push set, vertex buffers after the fragment stage's, samplers into set kSamplerSet) by rewriting
// its decorations, and lists them. False with `error` set for a resource the layout has no place for.
bool renumber(std::vector<uint32_t>& code, SpirvInterface& out, std::string& error);

} // namespace azl::gpu::vk
