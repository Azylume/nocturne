#pragma once

// What the Vulkan backend offers beyond gpu.hpp: the handles a swapchain needs, and its validation count. Only code
// that is Vulkan-specific anyway (outputs' present threads, tests) includes this.

#include "gpu/gpu.hpp"

#include <cstdint>

namespace azl::gpu::vulkan {

VkInstance instance(const Device& device);
VkPhysicalDevice physicalDevice(const Device& device);
VkDevice logicalDevice(const Device& device);
// The family every Queue of the device belongs to.
uint32_t queueFamily(const Device& device);

// Messages of severity error from the validation layer since the process started. The layer is on when
// NOCTURNE_VULKAN_VALIDATION=1 was set before the first device was made (the test mains set it).
uint64_t validationErrors();
bool validationEnabled();

// A swapchain's image as a render target. The texture never destroys the image; it rests in the present layout, so
// a pass drawn into it leaves it ready to present.
Texture wrapSwapchainImage(const Device& device, VkImage image, VkFormat format, uint32_t width, uint32_t height);
// The command buffer waits for a binary semaphore (a swapchain acquire) before `stage`, or signals one when it is done.
void waitSemaphore(CommandBuffer& cmd, VkSemaphore semaphore, VkPipelineStageFlags2 stage);
void signalSemaphore(CommandBuffer& cmd, VkSemaphore semaphore);
// vkQueuePresentKHR on the queue, serialised with its submissions. The output's present thread.
VkResult present(const Queue& queue, const VkPresentInfoKHR& info);

#if defined(_WIN32)
// A surface for a Win32 window; VK_NULL_HANDLE when it cannot be made.
VkSurfaceKHR makeSurface(const Device& device, void* hinstance, void* hwnd);
#endif

} // namespace azl::gpu::vulkan
