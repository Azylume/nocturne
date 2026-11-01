#pragma once

#include <cstdint>
#include <string>
#include <vector>

// What the Vulkan backend needs from the machine, decided from plain values so it is tested without a GPU.
namespace azl::gpu {

// VK_MAKE_API_VERSION(0, major, minor, 0).
constexpr uint32_t vulkanVersion(uint32_t major, uint32_t minor)
{
    return (major << 22) | (minor << 12);
}
constexpr uint32_t kVulkanMinimum = vulkanVersion(1, 3);

struct VulkanFeatures {
    bool dynamicRendering = false;
    bool synchronization2 = false;
    bool timelineSemaphore = false;
    bool scalarBlockLayout = false;
    bool hostQueryReset = false;
    bool textureCompressionBC = false;
};

struct VulkanDeviceInfo {
    std::string name;
    uint32_t apiVersion = 0;
    bool discrete = false;
    std::vector<std::string> extensions;
    VulkanFeatures features;
};

// What the machine offers, read in order: the loader's version (0 when vulkan-1.dll is missing), whether an instance
// was made, then its devices.
struct VulkanProbe {
    uint32_t loaderVersion = 0;
    bool instance = false;
    std::vector<VulkanDeviceInfo> devices;
};

struct VulkanChoice {
    // Index into the probe's devices, a discrete GPU first; -1 when none will do.
    int device = -1;
    // Empty when a device was chosen; otherwise one line for people: what is missing and what to do.
    std::string problem;
};

VulkanChoice chooseVulkanDevice(const VulkanProbe& probe);

// The line chooseVulkanDevice gives for `what`: it adds what to do about it.
std::string vulkanProblem(const std::string& what);

} // namespace azl::gpu
