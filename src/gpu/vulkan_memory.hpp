#pragma once

#include "gpu/gpu.hpp"

#include <mutex>
#include <vector>

namespace azl::gpu::vk {

// Device memory for the backend's few kinds of resources (render targets and decoded pictures, staging and readback
// buffers, constants): 64 MB blocks per memory type, images and buffers in separate blocks so their granularity never
// matters, first fit with neighbours merged on release. Large resources get memory of their own. Host-visible blocks
// stay mapped. Any thread.
class MemoryAllocator {
public:
    void init(VkDevice device, const VkPhysicalDeviceMemoryProperties& properties);
    bool allocate(const VkMemoryRequirements& requirements, uint32_t type, bool image, Allocation& out);
    // A default Allocation is ignored.
    void release(const Allocation& allocation);

private:
    struct Range {
        VkDeviceSize offset;
        VkDeviceSize size;
    };
    struct Block {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint8_t* mapped = nullptr;
        uint32_t type = 0;
        bool image = false;
        // Sorted by offset, never touching each other.
        std::vector<Range> free;
    };

    bool allocateMemory(VkDeviceSize size, uint32_t type, VkDeviceMemory& memory, uint8_t*& mapped);
    static bool fit(Block& block, const VkMemoryRequirements& requirements, Allocation& out);

    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties properties_{};
    std::mutex mutex_;
    // Under mutex_.
    std::vector<Block> blocks_;
};

} // namespace azl::gpu::vk
