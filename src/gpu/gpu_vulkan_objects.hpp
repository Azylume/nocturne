#pragma once

// The Vulkan backend's objects behind the gpu.hpp handles. Included by gpu_vulkan_inline.hpp after gpu.hpp's
// declarations; nothing outside src/gpu/ uses these directly (gpu_vulkan.hpp is the backend's public surface).

#include <vulkan/vulkan.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace azl::gpu::vk {

// Every pipeline's resources live in one push descriptor set (set 0): shader_layout.hpp's set 0 buffers of the
// fragment or compute stage at their index, the vertex stage's from kVertexBuffers, set 1 textures from kTextures.
// Its set 2 samplers become set 1, one fixed descriptor set per device. The backend renumbers each SPIR-V module so,
// as on Metal, vertex and fragment buffer indices never clash.
inline constexpr uint32_t kBufferSlots = 16;
inline constexpr uint32_t kTextureSlots = 32;
inline constexpr uint32_t kVertexBuffers = kBufferSlots;
inline constexpr uint32_t kTextures = 2 * kBufferSlots;
inline constexpr uint32_t kBindings = kTextures + kTextureSlots;
inline constexpr uint32_t kResourceSet = 0;
inline constexpr uint32_t kSamplerSet = 1;

struct Allocation {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    uint8_t* mapped = nullptr;
    // The allocator's block, or a dedicated allocation when kDedicated.
    uint32_t block = 0;
    static constexpr uint32_t kDedicated = ~0u;
};

struct TextureObject : std::enable_shared_from_this<TextureObject> {
    ~TextureObject();
    std::shared_ptr<DeviceObject> device;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    // Level 0 alone, for drawing into and storing to: `view` itself when there is one level.
    VkImageView view0 = VK_NULL_HANDLE;
    Allocation memory;
    // False for an image the backend did not make (a swapchain's): only its view is destroyed.
    bool ownsImage = true;
    VkFormat vkFormat = VK_FORMAT_UNDEFINED;
    uint8_t format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    size_t bytes = 0;
    // The layout every operation leaves the texture in, so command buffers recorded on different threads agree on it
    // at their boundaries.
    VkImageLayout resting = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    // The layout the last recorded command left it in. Recording threads only.
    std::atomic<VkImageLayout> layout{VK_IMAGE_LAYOUT_UNDEFINED};
};

struct BufferObject : std::enable_shared_from_this<BufferObject> {
    ~BufferObject();
    std::shared_ptr<DeviceObject> device;
    VkBuffer buffer = VK_NULL_HANDLE;
    Allocation memory;
    size_t size = 0;
};

struct Binding {
    uint32_t set = 0;
    uint32_t binding = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
};

struct ShaderModuleObject : std::enable_shared_from_this<ShaderModuleObject> {
    ~ShaderModuleObject();
    struct Entry {
        std::string name;
        VkShaderModule module = VK_NULL_HANDLE;
        VkShaderStageFlagBits stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        // After renumbering: set kResourceSet or kSamplerSet.
        std::vector<Binding> bindings;
        // Compute: the group size the shader declares.
        uint32_t localX = 1;
        uint32_t localY = 1;
    };
    std::shared_ptr<DeviceObject> device;
    std::vector<Entry> entries;
};

struct PipelineCacheObject : std::enable_shared_from_this<PipelineCacheObject> {
    ~PipelineCacheObject();
    std::shared_ptr<DeviceObject> device;
    VkPipelineCache cache = VK_NULL_HANDLE;
};

struct PipelineObject : std::enable_shared_from_this<PipelineObject> {
    ~PipelineObject();
    std::shared_ptr<DeviceObject> device;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout resources = VK_NULL_HANDLE;
    VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    // Set kResourceSet's bindings, in binding order.
    std::vector<Binding> bindings;
    uint32_t localX = 1;
    uint32_t localY = 1;
};

struct FenceObject : std::enable_shared_from_this<FenceObject> {
    ~FenceObject();
    std::shared_ptr<DeviceObject> device;
    VkSemaphore semaphore = VK_NULL_HANDLE;
};

struct QueryObject : std::enable_shared_from_this<QueryObject> {
    ~QueryObject();
    std::shared_ptr<DeviceObject> device;
    VkQueryPool pool = VK_NULL_HANDLE;
    uint32_t count = 0;
};

struct QueueSlot;

struct QueueObject : std::enable_shared_from_this<QueueObject> {
    std::shared_ptr<DeviceObject> device;
    QueueSlot* slot = nullptr;
};

// Host-visible memory a command buffer's setBytes constants are copied into.
struct UniformChunk {
    VkBuffer buffer = VK_NULL_HANDLE;
    Allocation memory;
};

// One command buffer from the device's pool, reused once the GPU is done with it and its Scope has closed.
struct CommandObject {
    DeviceObject* device = nullptr;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    // Runs before `cmd` in the same submission: layout changes a render pass needed but could not record itself.
    VkCommandBuffer prologue = VK_NULL_HANDLE;
    bool prologueOpen = false;
    // Start and end of the whole command buffer.
    VkQueryPool times = VK_NULL_HANDLE;

    std::shared_ptr<QueueObject> queue;
    // The scope's and, once committed, the GPU's: back to the pool when both are gone.
    std::atomic<int> refs{0};
    bool committed = false;
    uint64_t value = 0;
    std::vector<std::shared_ptr<const void>> retained;
    std::vector<VkSemaphoreSubmitInfo> waits;
    std::vector<VkSemaphoreSubmitInfo> signals;
    std::vector<std::function<void(const Completion&)>> handlers;

    std::vector<UniformChunk> chunks;
    size_t chunk = 0;
    VkDeviceSize used = 0;

    // Encoding state.
    RenderTarget target{};
    bool rendering = false;
    PipelineObject* pipeline = nullptr;
    // The bindings changed since they were last pushed.
    bool dirty = false;
    // By binding: the fragment or compute stage's buffers, then the vertex stage's.
    std::array<VkDescriptorBufferInfo, kTextures> buffers{};
    std::array<TextureObject*, kTextureSlots> textures{};
    // Storage images a compute pass wrote, returned to their resting layout at its end.
    std::vector<TextureObject*> written;
    bool dispatched = false;
};

void retain(CommandObject* c, std::shared_ptr<const void> object);
void pushBytes(CommandObject* c, uint32_t binding, const void* data, size_t size);
void bindPipeline(CommandObject* c, PipelineObject* pipeline);
void flush(CommandObject* c);
void setScissor(CommandObject* c, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
void writeTimestamp(CommandObject* c, QueryObject* queries, uint32_t index);
void endRendering(CommandObject* c);
// Covers width x height threads in groups of the size the shader declares.
void dispatch(CommandObject* c, uint32_t width, uint32_t height);
void endCompute(CommandObject* c);
void addHandler(CommandObject* c, std::function<void(const Completion&)> handler);
Completion completion(CommandObject* c);
void waitUntilCompleted(CommandObject* c);
void signal(CommandObject* c, FenceObject* fence, uint64_t value);
void wait(CommandObject* c, FenceObject* fence, uint64_t value);
void commit(CommandObject* c);
void transition(CommandObject* c, TextureObject* texture, uint8_t layout);
uint64_t fenceValue(FenceObject* fence);
bool waitFence(FenceObject* fence, uint64_t value, uint64_t timeoutMs);
CommandObject* commandBuffer(QueueObject* queue);
size_t openScope();
void closeScope(size_t mark);
uint32_t maxThreads(PipelineObject* pipeline);
bool unifiedMemory(DeviceObject* device);

inline void bindTexture(CommandObject* c, uint32_t slot, TextureObject* texture)
{
    if (c->textures[slot] == texture)
        return;
    c->textures[slot] = texture;
    c->dirty = true;
    retain(c, texture->shared_from_this());
}

} // namespace azl::gpu::vk
