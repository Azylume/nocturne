#pragma once

// What the Vulkan backend offers beyond gpu.hpp: the handles a swapchain needs, and its validation count. Only code
// that is Vulkan-specific anyway (outputs' present threads, tests) includes this.

#include "gpu/gpu.hpp"

#include <cstdint>
#include <functional>
#include <shared_mutex>
#include <vector>

// Every queue submit of the backend, vkQueueSubmit2 itself unless a test binary links its own to make one fail.
extern "C" VkResult nocturneVulkanQueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits,
                                               VkFence fence);

namespace azl::gpu::vulkan {

VkInstance instance(const Device& device);
VkPhysicalDevice physicalDevice(const Device& device);
VkDevice logicalDevice(const Device& device);
// The family every Queue of the device belongs to.
uint32_t queueFamily(const Device& device);

// A queue family the device keeps for a video decoder; its queues are never used by the backend.
struct VideoFamily {
    uint32_t index = 0;
    uint32_t queues = 0;
    VkQueueFlags flags = 0;
};

// What a video decoder needs to build on the device's own VkDevice: the extensions and features enabled and the queues
// reserved for it. Lives as long as the device.
struct VideoDecodeDevice {
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice logicalDevice = VK_NULL_HANDLE;
    std::vector<const char*> extensions;
    std::vector<VideoFamily> families;
    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
};

// Null when the device was made without Vulkan Video decode (no decode queue, extension, or spare queue).
const VideoDecodeDevice* videoDecode(const Device& device);

// A decoded 4:2:0 two-plane image (NV12) that another component owns, with the state its timeline semaphore and layout
// are in.
struct DecodedImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkAccessFlags2 access = 0;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    uint64_t value = 0;
    // The luma rectangle to copy from the corner, even in both.
    uint32_t width = 0;
    uint32_t height = 0;
};

// Copies the image's luma plane into `luma` (R8) and its chroma plane into `chroma` (RG8 at half size) and waits for
// the copy. It waits on the image's semaphore at `value` and signals it at `value + 1`, leaves the image in
// VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL and calls `submitted(true)` once the copy is queued, or `submitted(false)` and
// returns false when it could not be. The caller keeps its lock on the image for that call only. A decode thread,
// never the render thread.
bool copyDecodedPlanes(const Device& device, const DecodedImage& image, const Texture& luma, const Texture& chroma,
                       const std::function<void(bool)>& submitted);

// Messages of severity error from the validation layer since the process started. The layer is on when
// NOCTURNE_VULKAN_VALIDATION=1 was set before the first device was made (the test mains set it).
uint64_t validationErrors();
bool validationEnabled();
// A device of its own beside the system default, kept for the life of the process, for a test that fails one. Null
// when there is no GPU.
Device makeSeparateDevice();

// A swapchain's image as a render target. The texture never destroys the image; it rests in the present layout, so
// a pass drawn into it leaves it ready to present.
Texture wrapSwapchainImage(const Device& device, VkImage image, VkFormat format, uint32_t width, uint32_t height);
// The command buffer waits for a binary semaphore (a swapchain acquire) before `stage`, or signals one when it is done.
void waitSemaphore(CommandBuffer& cmd, VkSemaphore semaphore, VkPipelineStageFlags2 stage);
void signalSemaphore(CommandBuffer& cmd, VkSemaphore semaphore);
// VK_KHR_present_id and VK_KHR_present_wait are enabled: a swapchain can tag presents and wait for them on screen.
bool presentWait(const Device& device);
// vkQueuePresentKHR on the queue, serialised with its submissions, holding presentationMutex shared. The output's
// present thread.
VkResult present(const Queue& queue, const VkPresentInfoKHR& info);
// Held shared by each acquire and present on the device, and exclusively while a surface or swapchain is made or
// destroyed: the NVIDIA driver faults in one output's present while another output's swapchain is made. Outputs still
// present side by side. Never held across a wait for the screen.
std::shared_mutex& presentationMutex(const Device& device);

#if defined(_WIN32)
// A surface for a Win32 window; VK_NULL_HANDLE when it cannot be made.
VkSurfaceKHR makeSurface(const Device& device, void* hinstance, void* hwnd);
#endif

} // namespace azl::gpu::vulkan
