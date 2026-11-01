#include "gpu/vulkan_requirements.hpp"

#include <doctest.h>

#include <string>

using azl::gpu::chooseVulkanDevice;
using azl::gpu::VulkanDeviceInfo;
using azl::gpu::VulkanProbe;
using azl::gpu::vulkanVersion;

namespace {

VulkanDeviceInfo capable(const std::string& name, bool discrete)
{
    VulkanDeviceInfo d{.name = name, .apiVersion = vulkanVersion(1, 3), .discrete = discrete};
    d.extensions = {"VK_KHR_swapchain", "VK_KHR_push_descriptor"};
    d.features = {true, true, true, true, true, true};
    return d;
}

VulkanProbe machine(std::vector<VulkanDeviceInfo> devices)
{
    return {.loaderVersion = vulkanVersion(1, 4), .instance = true, .devices = std::move(devices)};
}

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

void checkAdvice(const std::string& problem)
{
    CHECK(contains(problem, "Update the GPU driver"));
    CHECK(contains(problem, "Vulkan 1.3 or newer"));
}

} // namespace

TEST_CASE("the Vulkan check chooses a capable discrete GPU over an integrated one listed first")
{
    const auto choice = chooseVulkanDevice(machine({capable("Intel UHD", false), capable("RTX 3050", true)}));
    CHECK(choice.device == 1);
    CHECK(choice.problem.empty());
    CHECK(chooseVulkanDevice(machine({capable("Intel UHD", false)})).device == 0);
}

TEST_CASE("the Vulkan check names a missing vulkan-1.dll, an old loader or a failed instance")
{
    VulkanProbe probe = machine({capable("RTX 3050", true)});
    probe.loaderVersion = 0;
    auto choice = chooseVulkanDevice(probe);
    CHECK(choice.device == -1);
    CHECK(contains(choice.problem, "vulkan-1.dll is missing"));
    checkAdvice(choice.problem);

    probe.loaderVersion = vulkanVersion(1, 2);
    choice = chooseVulkanDevice(probe);
    CHECK(choice.device == -1);
    CHECK(contains(choice.problem, "the Vulkan loader is version 1.2"));

    probe.loaderVersion = vulkanVersion(1, 3);
    probe.instance = false;
    choice = chooseVulkanDevice(probe);
    CHECK(choice.device == -1);
    CHECK(contains(choice.problem, "vkCreateInstance failed"));
    checkAdvice(choice.problem);
}

TEST_CASE("the Vulkan check names no device, or a device below Vulkan 1.3")
{
    auto choice = chooseVulkanDevice(machine({}));
    CHECK(choice.device == -1);
    CHECK(contains(choice.problem, "no GPU offers Vulkan"));
    checkAdvice(choice.problem);

    VulkanDeviceInfo old = capable("GTX 650", true);
    old.apiVersion = vulkanVersion(1, 2) | 199;
    choice = chooseVulkanDevice(machine({old}));
    CHECK(choice.device == -1);
    CHECK(contains(choice.problem, "GTX 650: Vulkan 1.2 only"));
    checkAdvice(choice.problem);
}

TEST_CASE("the Vulkan check skips and names a device without push descriptors or a required feature")
{
    VulkanDeviceInfo noPush = capable("RTX 3050", true);
    noPush.extensions = {"VK_KHR_swapchain"};
    VulkanDeviceInfo noBC = capable("Intel UHD", false);
    noBC.features.textureCompressionBC = false;
    noBC.features.dynamicRendering = false;

    auto choice = chooseVulkanDevice(machine({noPush, noBC}));
    CHECK(choice.device == -1);
    CHECK(contains(choice.problem, "RTX 3050: no VK_KHR_push_descriptor"));
    CHECK(contains(choice.problem, "Intel UHD: no dynamicRendering, textureCompressionBC"));
    checkAdvice(choice.problem);

    choice = chooseVulkanDevice(machine({noPush, capable("Intel UHD", false)}));
    CHECK(choice.device == 1);
}
