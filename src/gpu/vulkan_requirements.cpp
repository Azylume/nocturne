#include "gpu/vulkan_requirements.hpp"

#include <algorithm>

namespace azl::gpu {

namespace {

constexpr const char* kPushDescriptor = "VK_KHR_push_descriptor";

std::string versionText(uint32_t version)
{
    return std::to_string(version >> 22) + "." + std::to_string((version >> 12) & 0x3ff);
}

// What the device lacks, comma separated; empty when it has everything.
std::string missing(const VulkanDeviceInfo& device)
{
    if (device.apiVersion < kVulkanMinimum)
        return "Vulkan " + versionText(device.apiVersion) + " only";
    std::string text;
    const auto add = [&](bool has, const char* name) {
        if (!has)
            text += (text.empty() ? "" : ", ") + std::string(name);
    };
    const auto& e = device.extensions;
    add(std::find(e.begin(), e.end(), kPushDescriptor) != e.end(), kPushDescriptor);
    const VulkanFeatures& f = device.features;
    add(f.dynamicRendering, "dynamicRendering");
    add(f.synchronization2, "synchronization2");
    add(f.timelineSemaphore, "timelineSemaphore");
    add(f.scalarBlockLayout, "scalarBlockLayout");
    add(f.hostQueryReset, "hostQueryReset");
    add(f.textureCompressionBC, "textureCompressionBC");
    return text.empty() ? text : "no " + text;
}

std::string devicesProblem(const std::vector<VulkanDeviceInfo>& devices)
{
    if (devices.empty())
        return "no GPU offers Vulkan";
    std::string text;
    for (const VulkanDeviceInfo& device : devices)
        text += (text.empty() ? "" : "; ") + device.name + ": " + missing(device);
    return text;
}

} // namespace

std::string vulkanProblem(const std::string& what)
{
    return what + ". Update the GPU driver: Nocturne needs Vulkan " + versionText(kVulkanMinimum) + " or newer.";
}

VulkanChoice chooseVulkanDevice(const VulkanProbe& probe)
{
    if (probe.loaderVersion == 0)
        return {.problem = vulkanProblem("no Vulkan loader is installed (vulkan-1.dll is missing)")};
    if (probe.loaderVersion < kVulkanMinimum)
        return {.problem = vulkanProblem("the Vulkan loader is version " + versionText(probe.loaderVersion))};
    if (!probe.instance)
        return {.problem = vulkanProblem("the Vulkan driver did not start (vkCreateInstance failed)")};
    int best = -1;
    for (int i = 0; i < int(probe.devices.size()); ++i) {
        if (!missing(probe.devices[i]).empty())
            continue;
        if (best < 0 || probe.devices[i].discrete)
            best = i;
        if (probe.devices[i].discrete)
            break;
    }
    if (best < 0)
        return {.problem = vulkanProblem(devicesProblem(probe.devices))};
    return {.device = best};
}

} // namespace azl::gpu
