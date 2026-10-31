#include "gpu/vulkan_memory.hpp"

namespace azl::gpu::vk {

namespace {

constexpr VkDeviceSize kBlockBytes = 64ull * 1024 * 1024;
constexpr VkDeviceSize kOwnMemoryAbove = 16ull * 1024 * 1024;

} // namespace

void MemoryAllocator::init(VkDevice device, const VkPhysicalDeviceMemoryProperties& properties)
{
    device_ = device;
    properties_ = properties;
}

bool MemoryAllocator::allocateMemory(VkDeviceSize size, uint32_t type, VkDeviceMemory& memory, uint8_t*& mapped)
{
    VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.allocationSize = size;
    info.memoryTypeIndex = type;
    if (vkAllocateMemory(device_, &info, nullptr, &memory) != VK_SUCCESS)
        return false;
    mapped = nullptr;
    if (properties_.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        void* data = nullptr;
        vkMapMemory(device_, memory, 0, VK_WHOLE_SIZE, 0, &data);
        mapped = static_cast<uint8_t*>(data);
    }
    return true;
}

bool MemoryAllocator::fit(Block& block, const VkMemoryRequirements& requirements, Allocation& out)
{
    for (size_t i = 0; i < block.free.size(); ++i) {
        const Range r = block.free[i];
        const VkDeviceSize start = (r.offset + requirements.alignment - 1) / requirements.alignment *
                                   requirements.alignment;
        const VkDeviceSize end = start + requirements.size;
        if (end > r.offset + r.size)
            continue;
        std::vector<Range> rest;
        if (start > r.offset)
            rest.push_back({r.offset, start - r.offset});
        if (end < r.offset + r.size)
            rest.push_back({end, r.offset + r.size - end});
        block.free.erase(block.free.begin() + i);
        block.free.insert(block.free.begin() + i, rest.begin(), rest.end());
        out.memory = block.memory;
        out.offset = start;
        out.size = requirements.size;
        out.mapped = block.mapped ? block.mapped + start : nullptr;
        return true;
    }
    return false;
}

bool MemoryAllocator::allocate(const VkMemoryRequirements& requirements, uint32_t type, bool image, Allocation& out)
{
    if (type >= properties_.memoryTypeCount)
        return false;
    if (requirements.size > kOwnMemoryAbove) {
        out = {};
        out.size = requirements.size;
        out.block = Allocation::kDedicated;
        return allocateMemory(requirements.size, type, out.memory, out.mapped);
    }
    std::lock_guard lock(mutex_);
    for (uint32_t i = 0; i < blocks_.size(); ++i)
        if (blocks_[i].type == type && blocks_[i].image == image && fit(blocks_[i], requirements, out)) {
            out.block = i;
            return true;
        }
    Block block;
    block.type = type;
    block.image = image;
    if (!allocateMemory(kBlockBytes, type, block.memory, block.mapped))
        return false;
    block.free.push_back({0, kBlockBytes});
    blocks_.push_back(std::move(block));
    out.block = uint32_t(blocks_.size() - 1);
    return fit(blocks_.back(), requirements, out);
}

void MemoryAllocator::release(const Allocation& a)
{
    if (!a.memory)
        return;
    if (a.block == Allocation::kDedicated) {
        vkFreeMemory(device_, a.memory, nullptr);
        return;
    }
    std::lock_guard lock(mutex_);
    std::vector<Range>& free = blocks_[a.block].free;
    auto at = free.begin();
    while (at != free.end() && at->offset < a.offset)
        ++at;
    at = free.insert(at, {a.offset, a.size});
    if (auto next = at + 1; next != free.end() && at->offset + at->size == next->offset) {
        at->size += next->size;
        free.erase(next);
    }
    if (at != free.begin()) {
        auto previous = at - 1;
        if (previous->offset + previous->size == at->offset) {
            previous->size += at->size;
            free.erase(at);
        }
    }
}

} // namespace azl::gpu::vk
