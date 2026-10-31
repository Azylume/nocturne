#if defined(_WIN32)
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#else
#include <time.h>
#endif
#include <vulkan/vulkan.h>

#include "effects/shader_layout.hpp"
#include "gpu/gpu.hpp"
#include "gpu/gpu_vulkan.hpp"
#include "gpu/vulkan_memory.hpp"
#include "gpu/vulkan_spirv.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

namespace azl::gpu {

namespace vk {

namespace {

std::atomic<uint64_t> gValidationErrors{0};
std::atomic<bool> gValidation{false};

constexpr VkDeviceSize kUniformChunk = 256 * 1024;
constexpr VkDeviceSize kStagingBytes = 64ull * 1024 * 1024;
constexpr size_t kUploadCommands = 8;
constexpr double kCalibrationSeconds = 10.0;

VKAPI_ATTR VkBool32 VKAPI_CALL onMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                         VkDebugUtilsMessageTypeFlagsEXT,
                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        ++gValidationErrors;
    std::fprintf(stderr, "vulkan: %s\n", data->pMessage);
    return VK_FALSE;
}

bool envOn(const char* name)
{
    const char* value = std::getenv(name);
    return value && std::string(value) == "1";
}

double hostSeconds(uint64_t ticks)
{
#if defined(_WIN32)
    static const double frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart);
    }();
    return double(ticks) / frequency;
#else
    return double(ticks) * 1e-9;
#endif
}

uint64_t hostTicks()
{
#if defined(_WIN32)
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return uint64_t(t.QuadPart);
#else
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec);
#endif
}

constexpr VkTimeDomainKHR kHostDomain =
#if defined(_WIN32)
    VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;
#else
    VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
#endif

VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

bool hasExtension(const std::vector<VkExtensionProperties>& list, const char* name)
{
    return std::any_of(list.begin(), list.end(),
                       [name](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

} // namespace

struct QueueSlot {
    VkQueue queue = VK_NULL_HANDLE;
    // Serialises submissions and presents to `queue`, and orders `submitted`.
    std::mutex mutex;
    VkSemaphore timeline = VK_NULL_HANDLE;
    // Under mutex.
    uint64_t submitted = 0;
    // Under DeviceObject::doneMutex, in timeline order.
    std::deque<CommandObject*> inFlight;
};

// Copies CPU data into textures through a staging ring. Decode and text threads; each upload waits for its copy.
struct Uploader {
    struct Span {
        VkDeviceSize start = 0;
        VkDeviceSize end = 0;
        uint64_t value = 0;
    };
    struct Recording {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        uint64_t value = 0;
    };
    QueueSlot* slot = nullptr;
    VkBuffer staging = VK_NULL_HANDLE;
    Allocation memory;
    std::mutex mutex;
    // Under mutex: the ring's next free byte, the copies still reading it and the recordings.
    VkDeviceSize head = 0;
    std::deque<Span> spans;
    std::array<Recording, kUploadCommands> recordings{};
    size_t next = 0;
};

struct DeviceObject : std::enable_shared_from_this<DeviceObject> {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    uint32_t family = 0;
    bool timestamps = false;
    VkDeviceSize uniformAlign = 256;
    PFN_vkCmdPushDescriptorSetKHR pushDescriptorSet = nullptr;
    PFN_vkGetCalibratedTimestampsKHR calibratedTimestamps = nullptr;

    std::vector<std::unique_ptr<QueueSlot>> slots;
    std::atomic<uint32_t> nextSlot{0};

    std::array<VkSampler, kSamplerKinds> samplers{};
    VkDescriptorSetLayout samplerLayout = VK_NULL_HANDLE;
    VkDescriptorPool samplerPool = VK_NULL_HANDLE;
    VkDescriptorSet samplerSet = VK_NULL_HANDLE;

    MemoryAllocator allocator;
    Uploader uploader;

    std::mutex poolMutex;
    // Under poolMutex.
    std::vector<CommandObject*> freeCommands;

    // Wakes the completion thread on every submission.
    VkSemaphore wake = VK_NULL_HANDLE;
    std::mutex doneMutex;
    // Under doneMutex.
    uint64_t wakeValue = 0;
    std::atomic<bool> lost{false};

    std::mutex calibrationMutex;
    // Under calibrationMutex: a device tick and the host seconds it was taken at.
    uint64_t calibratedTicks = 0;
    double calibratedSeconds = 0.0;
    bool calibrated = false;
    // Host seconds of the last calibration. The completion thread's once it runs.
    double lastCalibration = 0.0;

    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags need, VkMemoryPropertyFlags prefer) const
    {
        uint32_t fallback = ~0u;
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            const VkMemoryPropertyFlags flags = memory.memoryTypes[i].propertyFlags;
            if (!(bits & (1u << i)) || (flags & need) != need)
                continue;
            if ((flags & prefer) == prefer)
                return i;
            if (fallback == ~0u)
                fallback = i;
        }
        return fallback;
    }
};

namespace {

thread_local std::vector<CommandObject*> tScope;
thread_local int tScopeDepth = 0;

// Where a layout's accesses happen: what a barrier out of it waits for and what a barrier into it holds back.
struct Use {
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
};

constexpr VkPipelineStageFlags2 kShaders = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

Use useOf(VkImageLayout layout)
{
    switch (layout) {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return {kShaders, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    case VK_IMAGE_LAYOUT_GENERAL:
        return {kShaders, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
    default: break;
    }
    // Undefined and present: whatever came before, including a swapchain acquire's semaphore wait.
    return {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE};
}

constexpr VkAccessFlags2 kWrites = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                                   VK_ACCESS_2_TRANSFER_WRITE_BIT;

bool readOnly(VkImageLayout layout)
{
    return layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
           layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
}

// Moves the texture to `to` after everything recorded before, its contents kept unless `discard`.
void barrier(VkCommandBuffer cmd, TextureObject* t, VkImageLayout to, bool discard = false)
{
    const VkImageLayout from = t->layout.load(std::memory_order_relaxed);
    if (from == to && readOnly(from))
        return;
    const Use src = useOf(from), dst = useOf(to);
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src.stage;
    b.srcAccessMask = src.access & kWrites;
    b.dstStageMask = to == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ? VK_PIPELINE_STAGE_2_NONE : dst.stage;
    b.dstAccessMask = dst.access;
    b.oldLayout = discard ? VK_IMAGE_LAYOUT_UNDEFINED : from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t->image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
    VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    info.imageMemoryBarrierCount = 1;
    info.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &info);
    t->layout.store(to, std::memory_order_relaxed);
}

void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    info.memoryBarrierCount = 1;
    info.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &info);
}

VkImageLayout vulkanLayout(Layout layout)
{
    switch (layout) {
    case Layout::RenderTarget: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case Layout::Sampled: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case Layout::Storage: return VK_IMAGE_LAYOUT_GENERAL;
    case Layout::CopySource: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case Layout::CopyTarget: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    case Layout::Present: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    case Layout::Undefined: break;
    }
    return VK_IMAGE_LAYOUT_UNDEFINED;
}

uint64_t counter(DeviceObject& d, VkSemaphore semaphore)
{
    uint64_t value = 0;
    vkGetSemaphoreCounterValue(d.device, semaphore, &value);
    return value;
}

bool waitValue(DeviceObject& d, VkSemaphore semaphore, uint64_t value, uint64_t timeoutNs)
{
    VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    info.semaphoreCount = 1;
    info.pSemaphores = &semaphore;
    info.pValues = &value;
    return vkWaitSemaphores(d.device, &info, timeoutNs) == VK_SUCCESS;
}

VkSemaphore makeTimeline(VkDevice device)
{
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
    VkSemaphore semaphore = VK_NULL_HANDLE;
    vkCreateSemaphore(device, &info, nullptr, &semaphore);
    return semaphore;
}

// Submits to the slot and returns the timeline value its completion signals. `tracked` goes to the completion thread.
uint64_t submit(DeviceObject& d, QueueSlot& slot, const VkCommandBufferSubmitInfo* cmds, uint32_t cmdCount,
                std::vector<VkSemaphoreSubmitInfo>& waits, std::vector<VkSemaphoreSubmitInfo>& signals,
                CommandObject* tracked)
{
    std::lock_guard lock(slot.mutex);
    const uint64_t value = ++slot.submitted;
    signals.push_back({VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, slot.timeline, value,
                       VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0});
    VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    info.waitSemaphoreInfoCount = uint32_t(waits.size());
    info.pWaitSemaphoreInfos = waits.data();
    info.commandBufferInfoCount = cmdCount;
    info.pCommandBufferInfos = cmds;
    info.signalSemaphoreInfoCount = uint32_t(signals.size());
    info.pSignalSemaphoreInfos = signals.data();
    const VkResult result = vkQueueSubmit2(slot.queue, 1, &info, VK_NULL_HANDLE);
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "vulkan: queue submit failed (%d)\n", int(result));
        if (result == VK_ERROR_DEVICE_LOST)
            d.lost = true;
    }
    if (tracked) {
        tracked->value = value;
        std::lock_guard done(d.doneMutex);
        slot.inFlight.push_back(tracked);
        VkSemaphoreSignalInfo wake{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        wake.semaphore = d.wake;
        wake.value = ++d.wakeValue;
        vkSignalSemaphore(d.device, &wake);
    }
    return value;
}

void beginRecording(VkCommandBuffer cmd)
{
    VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &info);
}

VkCommandBuffer prologue(CommandObject* c)
{
    if (!c->prologueOpen) {
        beginRecording(c->prologue);
        c->prologueOpen = true;
    }
    return c->prologue;
}

void recycle(CommandObject* c)
{
    vkResetCommandPool(c->device->device, c->pool, 0);
    c->retained.clear();
    c->waits.clear();
    c->signals.clear();
    c->handlers.clear();
    c->written.clear();
    c->queue.reset();
    c->chunk = 0;
    c->used = 0;
    c->prologueOpen = false;
    c->committed = false;
    c->pipeline = nullptr;
    c->textures = {};
    c->buffers = {};
    DeviceObject* d = c->device;
    std::lock_guard lock(d->poolMutex);
    d->freeCommands.push_back(c);
}

void release(CommandObject* c)
{
    if (c->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
        recycle(c);
}

CommandObject* newCommand(DeviceObject& d)
{
    auto* c = new CommandObject;
    c->device = &d;
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool.queueFamilyIndex = d.family;
    vkCreateCommandPool(d.device, &pool, nullptr, &c->pool);
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = c->pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 2;
    VkCommandBuffer buffers[2];
    vkAllocateCommandBuffers(d.device, &alloc, buffers);
    c->cmd = buffers[0];
    c->prologue = buffers[1];
    if (d.timestamps) {
        VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        query.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query.queryCount = 2;
        vkCreateQueryPool(d.device, &query, nullptr, &c->times);
    }
    return c;
}

UniformChunk makeChunk(DeviceObject& d)
{
    UniformChunk chunk;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = kUniformChunk;
    info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    vkCreateBuffer(d.device, &info, nullptr, &chunk.buffer);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d.device, chunk.buffer, &req);
    const uint32_t type = d.memoryType(req.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    d.allocator.allocate(req, type, false, chunk.memory);
    vkBindBufferMemory(d.device, chunk.buffer, chunk.memory.memory, chunk.memory.offset);
    return chunk;
}

void calibrate(DeviceObject& d)
{
    if (!d.calibratedTimestamps)
        return;
    VkCalibratedTimestampInfoKHR infos[2] = {{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR},
                                             {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR}};
    infos[0].timeDomain = VK_TIME_DOMAIN_DEVICE_KHR;
    infos[1].timeDomain = kHostDomain;
    uint64_t stamps[2] = {};
    uint64_t deviation = 0;
    if (d.calibratedTimestamps(d.device, 2, infos, stamps, &deviation) != VK_SUCCESS)
        return;
    std::lock_guard lock(d.calibrationMutex);
    d.calibratedTicks = stamps[0];
    d.calibratedSeconds = hostSeconds(stamps[1]);
    d.calibrated = true;
    d.lastCalibration = hostSeconds(hostTicks());
}

double secondsOf(DeviceObject& d, uint64_t ticks)
{
    std::lock_guard lock(d.calibrationMutex);
    if (!d.calibrated)
        return 0.0;
    const double delta = (double(ticks) - double(d.calibratedTicks)) * d.properties.limits.timestampPeriod * 1e-9;
    return d.calibratedSeconds + delta;
}

void completeAll(DeviceObject& d, std::vector<CommandObject*>& done)
{
    for (CommandObject* c : done) {
        const Completion result = completion(c);
        for (auto& handler : c->handlers)
            handler(result);
        release(c);
    }
    done.clear();
}

// Runs every command buffer's completion handlers, in submission order per queue, and returns it to the pool.
void completionLoop(DeviceObject* d)
{
    std::vector<VkSemaphore> semaphores;
    std::vector<uint64_t> values;
    std::vector<CommandObject*> done;
    for (;;) {
        semaphores.assign(1, d->wake);
        {
            std::lock_guard lock(d->doneMutex);
            values.assign(1, d->wakeValue + 1);
            for (auto& slot : d->slots)
                if (!slot->inFlight.empty()) {
                    semaphores.push_back(slot->timeline);
                    values.push_back(slot->inFlight.front()->value);
                }
        }
        VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        info.flags = VK_SEMAPHORE_WAIT_ANY_BIT;
        info.semaphoreCount = uint32_t(semaphores.size());
        info.pSemaphores = semaphores.data();
        info.pValues = values.data();
        if (vkWaitSemaphores(d->device, &info, 1000000000ull) == VK_ERROR_DEVICE_LOST) {
            d->lost = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        {
            std::lock_guard lock(d->doneMutex);
            for (auto& slot : d->slots) {
                const uint64_t reached = counter(*d, slot->timeline);
                while (!slot->inFlight.empty() && (slot->inFlight.front()->value <= reached || d->lost)) {
                    done.push_back(slot->inFlight.front());
                    slot->inFlight.pop_front();
                }
            }
        }
        completeAll(*d, done);
        if (hostSeconds(hostTicks()) - d->lastCalibration > kCalibrationSeconds)
            calibrate(*d);
    }
}

VkFormat vulkanFormat(Format format)
{
    switch (format) {
    case Format::BGRA8: return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::RGBA16: return VK_FORMAT_R16G16B16A16_UNORM;
    case Format::R8: return VK_FORMAT_R8_UNORM;
    case Format::RG8: return VK_FORMAT_R8G8_UNORM;
    case Format::R16: return VK_FORMAT_R16_UNORM;
    case Format::RG16: return VK_FORMAT_R16G16_UNORM;
    case Format::BC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case Format::BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
    case Format::BC4: return VK_FORMAT_BC4_UNORM_BLOCK;
    case Format::Invalid: break;
    }
    return VK_FORMAT_UNDEFINED;
}

Format fromVulkan(VkFormat format)
{
    for (Format f : {Format::BGRA8, Format::RGBA8, Format::RGBA16F, Format::RGBA16, Format::R8, Format::RG8,
                     Format::R16, Format::RG16, Format::BC1, Format::BC3, Format::BC4})
        if (vulkanFormat(f) == format)
            return f;
    return Format::Invalid;
}

VkImageView makeView(VkDevice device, VkImage image, VkFormat format, uint32_t levels)
{
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    vkCreateImageView(device, &info, nullptr, &view);
    return view;
}

// The level's copy region: tightly packed rows (BC: of blocks).
VkBufferImageCopy region(const TextureObject& t, uint32_t level, VkDeviceSize offset)
{
    VkBufferImageCopy r{};
    r.bufferOffset = offset;
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
    r.imageExtent = {std::max(1u, t.width >> level), std::max(1u, t.height >> level), 1};
    return r;
}

VkSampler makeSampler(VkDevice device, SamplerKind kind)
{
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxLod = 0.0f;
    if (kind == SamplerKind::Pixel)
        info.unnormalizedCoordinates = VK_TRUE;
    if (kind == SamplerKind::Text) {
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        info.maxLod = VK_LOD_CLAMP_NONE;
        info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
    VkSampler sampler = VK_NULL_HANDLE;
    vkCreateSampler(device, &info, nullptr, &sampler);
    return sampler;
}

void makeSamplerSet(DeviceObject& d)
{
    std::array<VkDescriptorSetLayoutBinding, kSamplerKinds> bindings{};
    for (uint32_t i = 0; i < kSamplerKinds; ++i) {
        d.samplers[i] = makeSampler(d.device, SamplerKind(i));
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_ALL, &d.samplers[i]};
    }
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = kSamplerKinds;
    layout.pBindings = bindings.data();
    vkCreateDescriptorSetLayout(d.device, &layout, nullptr, &d.samplerLayout);
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_SAMPLER, kSamplerKinds};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    vkCreateDescriptorPool(d.device, &pool, nullptr, &d.samplerPool);
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = d.samplerPool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &d.samplerLayout;
    vkAllocateDescriptorSets(d.device, &alloc, &d.samplerSet);
}

VkInstance makeInstance(bool validation, VkDebugUtilsMessengerEXT& messenger)
{
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
    std::vector<const char*> extensions;
    for (const char* name : {VK_KHR_SURFACE_EXTENSION_NAME,
#if defined(_WIN32)
                             VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#endif
                             VK_EXT_DEBUG_UTILS_EXTENSION_NAME})
        if (hasExtension(available, name) && (validation || std::strcmp(name, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) != 0))
            extensions.push_back(name);
    const bool debugUtils = std::find_if(extensions.begin(), extensions.end(), [](const char* e) {
                                return std::strcmp(e, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
                            }) != extensions.end();

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Nocturne";
    app.pEngineName = "Nocturne";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = uint32_t(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    const char* layer = "VK_LAYER_KHRONOS_validation";
    if (validation) {
        info.enabledLayerCount = 1;
        info.ppEnabledLayerNames = &layer;
    }
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS && validation) {
        std::fprintf(stderr, "vulkan: the validation layer is missing; running without it\n");
        gValidation = false;
        info.enabledLayerCount = 0;
        if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        return instance;
    }
    if (instance && validation && debugUtils) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        VkDebugUtilsMessengerCreateInfoEXT m{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        m.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        m.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        m.pfnUserCallback = onMessage;
        if (create)
            create(instance, &m, nullptr, &messenger);
    }
    return instance;
}

VkPhysicalDevice pickPhysical(VkInstance instance)
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    VkPhysicalDevice best = VK_NULL_HANDLE;
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(device, &p);
        if (p.apiVersion < VK_API_VERSION_1_3)
            continue;
        if (!best || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            best = device;
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            break;
    }
    return best;
}

bool makeLogical(DeviceObject& d, uint32_t queues)
{
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(d.physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(d.physical, nullptr, &count, available.data());
    if (!hasExtension(available, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME))
        return false;
    std::vector<const char*> extensions{VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    if (hasExtension(available, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    const char* calibration = hasExtension(available, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)
                                  ? VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME
                              : hasExtension(available, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)
                                  ? VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME
                                  : nullptr;
    if (calibration)
        extensions.push_back(calibration);

    VkPhysicalDeviceVulkan13Features has13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features has12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &has13};
    VkPhysicalDeviceFeatures2 has{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &has12};
    vkGetPhysicalDeviceFeatures2(d.physical, &has);
    if (!has13.dynamicRendering || !has13.synchronization2 || !has12.timelineSemaphore || !has12.scalarBlockLayout ||
        !has.features.textureCompressionBC)
        return false;

    VkPhysicalDeviceVulkan13Features use13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    use13.dynamicRendering = VK_TRUE;
    use13.synchronization2 = VK_TRUE;
    use13.pipelineCreationCacheControl = has13.pipelineCreationCacheControl;
    use13.robustImageAccess = has13.robustImageAccess;
    VkPhysicalDeviceVulkan12Features use12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &use13};
    use12.timelineSemaphore = VK_TRUE;
    use12.scalarBlockLayout = VK_TRUE;
    use12.hostQueryReset = has12.hostQueryReset;
    use12.uniformBufferStandardLayout = has12.uniformBufferStandardLayout;
    VkPhysicalDeviceFeatures2 use{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &use12};
    use.features.textureCompressionBC = VK_TRUE;
    use.features.shaderStorageImageExtendedFormats = has.features.shaderStorageImageExtendedFormats;
    use.features.shaderStorageImageWriteWithoutFormat = has.features.shaderStorageImageWriteWithoutFormat;
    use.features.shaderStorageImageReadWithoutFormat = has.features.shaderStorageImageReadWithoutFormat;

    const std::vector<float> priorities(queues, 1.0f);
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = d.family;
    queue.queueCount = queues;
    queue.pQueuePriorities = priorities.data();
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &use};
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = uint32_t(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    if (vkCreateDevice(d.physical, &info, nullptr, &d.device) != VK_SUCCESS)
        return false;
    d.pushDescriptorSet = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
        vkGetDeviceProcAddr(d.device, "vkCmdPushDescriptorSetKHR"));
    if (calibration)
        d.calibratedTimestamps = reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(vkGetDeviceProcAddr(
            d.device, std::strcmp(calibration, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) == 0
                          ? "vkGetCalibratedTimestampsKHR"
                          : "vkGetCalibratedTimestampsEXT"));
    return d.pushDescriptorSet != nullptr;
}

bool pickFamily(DeviceObject& d, uint32_t& queues)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(d.physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(d.physical, &count, families.data());
    constexpr VkQueueFlags kNeed = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    for (uint32_t i = 0; i < count; ++i)
        if ((families[i].queueFlags & kNeed) == kNeed) {
            d.family = i;
            queues = std::min(families[i].queueCount, 4u);
            d.timestamps = families[i].timestampValidBits > 0 && d.properties.limits.timestampComputeAndGraphics;
            return true;
        }
    return false;
}

void makeUploader(DeviceObject& d)
{
    Uploader& u = d.uploader;
    u.slot = d.slots.back().get();
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = kStagingBytes;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vkCreateBuffer(d.device, &info, nullptr, &u.staging);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d.device, u.staging, &req);
    const uint32_t type = d.memoryType(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    d.allocator.allocate(req, type, false, u.memory);
    vkBindBufferMemory(d.device, u.staging, u.memory.memory, u.memory.offset);
    for (auto& r : u.recordings) {
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool.queueFamilyIndex = d.family;
        vkCreateCommandPool(d.device, &pool, nullptr, &r.pool);
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = r.pool;
        alloc.commandBufferCount = 1;
        vkAllocateCommandBuffers(d.device, &alloc, &r.cmd);
    }
}

std::shared_ptr<DeviceObject> makeDevice()
{
    auto d = std::make_shared<DeviceObject>();
    const bool validation = envOn("NOCTURNE_VULKAN_VALIDATION");
    gValidation = validation;
    d->instance = makeInstance(validation, d->messenger);
    if (!d->instance)
        return nullptr;
    d->physical = pickPhysical(d->instance);
    if (!d->physical)
        return nullptr;
    vkGetPhysicalDeviceProperties(d->physical, &d->properties);
    vkGetPhysicalDeviceMemoryProperties(d->physical, &d->memory);
    d->uniformAlign = std::max(d->properties.limits.minUniformBufferOffsetAlignment,
                               d->properties.limits.minStorageBufferOffsetAlignment);
    uint32_t queues = 0;
    if (!pickFamily(*d, queues) || !makeLogical(*d, queues))
        return nullptr;
    d->allocator.init(d->device, d->memory);
    for (uint32_t i = 0; i < queues; ++i) {
        auto slot = std::make_unique<QueueSlot>();
        vkGetDeviceQueue(d->device, d->family, i, &slot->queue);
        slot->timeline = makeTimeline(d->device);
        d->slots.push_back(std::move(slot));
    }
    d->wake = makeTimeline(d->device);
    makeSamplerSet(*d);
    makeUploader(*d);
    calibrate(*d);
    std::thread(completionLoop, d.get()).detach();
    return d;
}

uint32_t texelsPerRow(Format format, size_t rowBytes)
{
    const size_t units = rowBytes / unitBytes(format);
    return uint32_t(isCompressed(format) ? units * 4 : units);
}

} // namespace

TextureObject::~TextureObject()
{
    VkDevice dev = device->device;
    if (view0 != view)
        vkDestroyImageView(dev, view0, nullptr);
    vkDestroyImageView(dev, view, nullptr);
    if (ownsImage) {
        vkDestroyImage(dev, image, nullptr);
        device->allocator.release(memory);
    }
}

BufferObject::~BufferObject()
{
    vkDestroyBuffer(device->device, buffer, nullptr);
    device->allocator.release(memory);
}

ShaderModuleObject::~ShaderModuleObject()
{
    for (Entry& e : entries)
        vkDestroyShaderModule(device->device, e.module, nullptr);
}

PipelineCacheObject::~PipelineCacheObject()
{
    vkDestroyPipelineCache(device->device, cache, nullptr);
}

PipelineObject::~PipelineObject()
{
    vkDestroyPipeline(device->device, pipeline, nullptr);
    vkDestroyPipelineLayout(device->device, layout, nullptr);
    vkDestroyDescriptorSetLayout(device->device, resources, nullptr);
}

FenceObject::~FenceObject()
{
    vkDestroySemaphore(device->device, semaphore, nullptr);
}

QueryObject::~QueryObject()
{
    vkDestroyQueryPool(device->device, pool, nullptr);
}

void retain(CommandObject* c, std::shared_ptr<const void> object)
{
    c->retained.push_back(std::move(object));
}

void pushBytes(CommandObject* c, uint32_t binding, const void* data, size_t size)
{
    if (binding >= kTextures || size == 0 || size > kUniformChunk)
        return;
    VkDeviceSize offset = alignUp(c->used, c->device->uniformAlign);
    if (c->chunk >= c->chunks.size() || offset + size > kUniformChunk) {
        if (c->chunk < c->chunks.size())
            ++c->chunk;
        if (c->chunk >= c->chunks.size())
            c->chunks.push_back(makeChunk(*c->device));
        offset = 0;
    }
    const UniformChunk& chunk = c->chunks[c->chunk];
    std::memcpy(chunk.memory.mapped + offset, data, size);
    c->used = offset + size;
    c->buffers[binding] = {chunk.buffer, offset, size};
    c->dirty = true;
}

void bindPipeline(CommandObject* c, PipelineObject* p)
{
    if (c->pipeline == p)
        return;
    c->pipeline = p;
    retain(c, p->shared_from_this());
    vkCmdBindPipeline(c->cmd, p->bindPoint, p->pipeline);
    vkCmdBindDescriptorSets(c->cmd, p->bindPoint, p->layout, kSamplerSet, 1, &c->device->samplerSet, 0, nullptr);
    c->dirty = true;
}

void flush(CommandObject* c)
{
    PipelineObject* p = c->pipeline;
    if (!p)
        return;
    std::array<VkWriteDescriptorSet, kBindings> writes;
    std::array<VkDescriptorImageInfo, kSlots> images;
    uint32_t n = 0;
    for (const Binding& b : p->bindings) {
        VkWriteDescriptorSet& w = writes[n];
        w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstBinding = b.binding;
        w.descriptorCount = 1;
        w.descriptorType = b.type;
        if (b.binding < kTextures) {
            if (!c->buffers[b.binding].buffer)
                continue;
            w.pBufferInfo = &c->buffers[b.binding];
        } else {
            const uint32_t slot = b.binding - kTextures;
            TextureObject* t = c->textures[slot];
            if (!t)
                continue;
            if (c->rendering && t->layout.load(std::memory_order_relaxed) == VK_IMAGE_LAYOUT_UNDEFINED)
                barrier(prologue(c), t, t->resting);
            const bool storage = b.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            const bool general = storage || t->layout.load(std::memory_order_relaxed) == VK_IMAGE_LAYOUT_GENERAL;
            images[slot] = {VK_NULL_HANDLE, storage ? t->view0 : t->view,
                            general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            w.pImageInfo = &images[slot];
        }
        ++n;
    }
    if (n)
        c->device->pushDescriptorSet(c->cmd, p->bindPoint, p->layout, kResourceSet, n, writes.data());
    c->dirty = false;
}

void setScissor(CommandObject* c, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    const VkRect2D rect{{int32_t(x), int32_t(y)}, {width, height}};
    vkCmdSetScissor(c->cmd, 0, 1, &rect);
}

void writeTimestamp(CommandObject* c, QueryObject* queries, uint32_t index)
{
    vkCmdResetQueryPool(prologue(c), queries->pool, index, 1);
    vkCmdWriteTimestamp2(c->cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries->pool, index);
    retain(c, queries->shared_from_this());
}

void endRendering(CommandObject* c)
{
    vkCmdEndRendering(c->cmd);
    for (const TimestampWrite& write : c->target.timestamps)
        if (write.queries)
            vkCmdWriteTimestamp2(c->cmd, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, write.queries->native()->pool,
                                 write.end);
    TextureObject* t = c->target.texture->native();
    if (t->resting != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
        barrier(c->cmd, t, t->resting);
    c->rendering = false;
}

void dispatch(CommandObject* c, uint32_t width, uint32_t height)
{
    PipelineObject* p = c->pipeline;
    if (!p)
        return;
    if (c->dispatched)
        memoryBarrier(c->cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    for (const Binding& b : p->bindings) {
        if (b.binding < kTextures)
            continue;
        TextureObject* t = c->textures[b.binding - kTextures];
        if (!t)
            continue;
        const VkImageLayout now = t->layout.load(std::memory_order_relaxed);
        if (b.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            if (now != VK_IMAGE_LAYOUT_GENERAL) {
                barrier(c->cmd, t, VK_IMAGE_LAYOUT_GENERAL);
                c->dirty = true;
            }
            if (std::find(c->written.begin(), c->written.end(), t) == c->written.end())
                c->written.push_back(t);
        } else if (now != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && now != VK_IMAGE_LAYOUT_GENERAL) {
            barrier(c->cmd, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            c->dirty = true;
        }
    }
    if (c->dirty)
        flush(c);
    vkCmdDispatch(c->cmd, (width + p->localX - 1) / p->localX, (height + p->localY - 1) / p->localY, 1);
    c->dispatched = true;
}

void endCompute(CommandObject* c)
{
    for (TextureObject* t : c->written)
        if (t->resting != VK_IMAGE_LAYOUT_GENERAL)
            barrier(c->cmd, t, t->resting);
    c->written.clear();
}

void addHandler(CommandObject* c, std::function<void(const Completion&)> handler)
{
    c->handlers.push_back(std::move(handler));
}

Completion completion(CommandObject* c)
{
    if (!c->committed)
        return {};
    DeviceObject& d = *c->device;
    if (d.lost || counter(d, c->queue->slot->timeline) < c->value)
        return {};
    Completion done{true, 0.0, 0.0};
    uint64_t ticks[2] = {};
    if (c->times && vkGetQueryPoolResults(d.device, c->times, 0, 2, sizeof(ticks), ticks, sizeof(uint64_t),
                                          VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        done.gpuStart = secondsOf(d, ticks[0]);
        done.gpuEnd = secondsOf(d, ticks[1]);
    }
    return done;
}

void waitUntilCompleted(CommandObject* c)
{
    if (c->committed)
        waitValue(*c->device, c->queue->slot->timeline, c->value, UINT64_MAX);
}

void signal(CommandObject* c, FenceObject* fence, uint64_t value)
{
    c->signals.push_back({VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, fence->semaphore, value,
                          VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0});
    retain(c, fence->shared_from_this());
}

void wait(CommandObject* c, FenceObject* fence, uint64_t value)
{
    c->waits.push_back({VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, fence->semaphore, value,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0});
    retain(c, fence->shared_from_this());
}

void commit(CommandObject* c)
{
    if (c->committed)
        return;
    if (c->times)
        vkCmdWriteTimestamp2(c->cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, c->times, 1);
    vkEndCommandBuffer(c->cmd);
    VkCommandBufferSubmitInfo cmds[2] = {{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO},
                                         {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}};
    uint32_t count = 0;
    if (c->prologueOpen) {
        vkEndCommandBuffer(c->prologue);
        cmds[count++].commandBuffer = c->prologue;
    }
    cmds[count++].commandBuffer = c->cmd;
    c->committed = true;
    c->refs.fetch_add(1, std::memory_order_acq_rel);
    submit(*c->device, *c->queue->slot, cmds, count, c->waits, c->signals, c);
}

void transition(CommandObject* c, TextureObject* texture, uint8_t layout)
{
    const VkImageLayout to = vulkanLayout(Layout(layout));
    if (c->rendering || to == VK_IMAGE_LAYOUT_UNDEFINED)
        return;
    barrier(c->cmd, texture, to);
    retain(c, texture->shared_from_this());
}

uint64_t fenceValue(FenceObject* fence)
{
    return counter(*fence->device, fence->semaphore);
}

bool waitFence(FenceObject* fence, uint64_t value, uint64_t timeoutMs)
{
    return waitValue(*fence->device, fence->semaphore, value, timeoutMs * 1000000ull);
}

CommandObject* commandBuffer(QueueObject* queue)
{
    DeviceObject& d = *queue->device;
    CommandObject* c = nullptr;
    {
        std::lock_guard lock(d.poolMutex);
        if (!d.freeCommands.empty()) {
            c = d.freeCommands.back();
            d.freeCommands.pop_back();
        }
    }
    if (!c)
        c = newCommand(d);
    c->queue = queue->shared_from_this();
    c->refs.store(0, std::memory_order_relaxed);
    if (tScopeDepth > 0) {
        c->refs.store(1, std::memory_order_relaxed);
        tScope.push_back(c);
    }
    beginRecording(c->cmd);
    if (c->times) {
        vkResetQueryPool(d.device, c->times, 0, 2);
        vkCmdWriteTimestamp2(c->cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, c->times, 0);
    }
    return c;
}

size_t openScope()
{
    ++tScopeDepth;
    return tScope.size();
}

void closeScope(size_t mark)
{
    for (size_t i = mark; i < tScope.size(); ++i)
        release(tScope[i]);
    tScope.resize(mark);
    --tScopeDepth;
}

uint32_t maxThreads(PipelineObject* pipeline)
{
    return pipeline->device->properties.limits.maxComputeWorkGroupInvocations;
}

bool unifiedMemory(DeviceObject* device)
{
    return device->properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
}

namespace {

VkDescriptorSetLayout makeResourceLayout(VkDevice device, const std::vector<Binding>& bindings,
                                         const std::vector<VkShaderStageFlags>& stages)
{
    std::vector<VkDescriptorSetLayoutBinding> list;
    for (size_t i = 0; i < bindings.size(); ++i)
        list.push_back({bindings[i].binding, bindings[i].type, 1, stages[i], nullptr});
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    info.bindingCount = uint32_t(list.size());
    info.pBindings = list.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    vkCreateDescriptorSetLayout(device, &info, nullptr, &layout);
    return layout;
}

// The pipeline's set kResourceSet from its stages' bindings, and its layout with the device's sampler set.
bool makeLayout(DeviceObject& d, PipelineObject& p, std::initializer_list<const ShaderModuleObject::Entry*> entries,
                std::string* error)
{
    std::map<uint32_t, std::pair<VkDescriptorType, VkShaderStageFlags>> merged;
    for (const auto* entry : entries)
        for (const Binding& b : entry->bindings) {
            if (b.set != kResourceSet)
                continue;
            auto [it, added] = merged.try_emplace(b.binding, b.type, 0);
            if (!added && it->second.first != b.type) {
                if (error)
                    *error = "binding " + std::to_string(b.binding) + " has two descriptor types";
                return false;
            }
            it->second.second |= entry->stage;
        }
    std::vector<VkShaderStageFlags> stages;
    for (const auto& [binding, use] : merged) {
        p.bindings.push_back({kResourceSet, binding, use.first});
        stages.push_back(use.second);
    }
    p.resources = makeResourceLayout(d.device, p.bindings, stages);
    const VkDescriptorSetLayout sets[2] = {p.resources, d.samplerLayout};
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = 2;
    info.pSetLayouts = sets;
    return vkCreatePipelineLayout(d.device, &info, nullptr, &p.layout) == VK_SUCCESS;
}

const ShaderModuleObject::Entry* findEntry(const ShaderModuleObject* module, const std::string& name)
{
    if (!module)
        return nullptr;
    for (const auto& e : module->entries)
        if (e.name == name)
            return &e;
    return nullptr;
}

void setError(std::string* error, const std::string& what)
{
    if (error)
        *error = what;
}

VkPipelineColorBlendAttachmentState blendOf(Blending blending)
{
    VkPipelineColorBlendAttachmentState s{};
    s.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                       VK_COLOR_COMPONENT_A_BIT;
    auto factors = [&s](VkBlendFactor source, VkBlendFactor destination) {
        s.blendEnable = VK_TRUE;
        s.srcColorBlendFactor = source;
        s.dstColorBlendFactor = destination;
        s.colorBlendOp = VK_BLEND_OP_ADD;
        s.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        s.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        s.alphaBlendOp = VK_BLEND_OP_ADD;
    };
    // With a premultiplied source s of alpha a over the target d:
    //   normal s + d(1-a), add s + d, screen s(1-d) + d, multiply s·d + d(1-a), i.e. mix(d, d·s/a, a).
    switch (blending) {
    case Blending::None: break;
    case Blending::Normal: factors(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA); break;
    case Blending::Add: factors(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE); break;
    case Blending::Screen: factors(VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_ONE); break;
    case Blending::Multiply: factors(VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA); break;
    }
    return s;
}

size_t cacheBytes(VkDevice device, VkPipelineCache cache)
{
    size_t size = 0;
    vkGetPipelineCacheData(device, cache, &size, nullptr);
    return size;
}

// Creates through the cache; `cached` when the cache already held the pipeline, which the cache not growing shows.
// (Asking the driver to fail on a miss does not: its own shader cache answers for pipelines this cache never held.)
VkResult createCached(DeviceObject& d, const PipelineCache& cache, const VkGraphicsPipelineCreateInfo& info,
                      bool* cached, VkPipeline& pipeline)
{
    const VkPipelineCache vkCache = cache ? cache.native()->cache : VK_NULL_HANDLE;
    const size_t before = vkCache ? cacheBytes(d.device, vkCache) : 0;
    const VkResult result = vkCreateGraphicsPipelines(d.device, vkCache, 1, &info, nullptr, &pipeline);
    if (cached)
        *cached = vkCache && result == VK_SUCCESS && cacheBytes(d.device, vkCache) == before;
    return result;
}

std::filesystem::path pathOf(const std::string& utf8)
{
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

bool cacheHeaderMatches(const DeviceObject& d, const std::vector<char>& data)
{
    VkPipelineCacheHeaderVersionOne header{};
    if (data.size() < sizeof(header))
        return false;
    std::memcpy(&header, data.data(), sizeof(header));
    return header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
           header.vendorID == d.properties.vendorID && header.deviceID == d.properties.deviceID &&
           std::memcmp(header.pipelineCacheUUID, d.properties.pipelineCacheUUID, VK_UUID_SIZE) == 0;
}

} // namespace

} // namespace vk

Device Device::systemDefault()
{
    // Made once and kept for the life of the process, like Metal's system default device.
    static auto* device = new std::shared_ptr<vk::DeviceObject>(vk::makeDevice());
    Device d;
    d.handle_ = *device;
    return d;
}

Device Device::retain(NativeDevice device)
{
    Device d;
    if (device)
        d.handle_ = device->shared_from_this();
    return d;
}

std::string Device::name() const
{
    return handle_->properties.deviceName;
}

Texture Device::makeTexture(const TextureDesc& desc) const
{
    vk::DeviceObject& d = *handle_;
    const VkFormat format = vk::vulkanFormat(desc.format);
    if (format == VK_FORMAT_UNDEFINED || desc.width == 0 || desc.height == 0)
        return {};
    uint32_t full = 1;
    while ((std::max(desc.width, desc.height) >> full) > 0)
        ++full;
    const uint32_t levels = desc.mipLevels == 0 ? full : std::min(desc.mipLevels, full);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (has(desc.usage, Usage::Sampled))
        usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (has(desc.usage, Usage::RenderTarget))
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (has(desc.usage, Usage::Storage))
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    VkImageFormatProperties limits;
    if (vkGetPhysicalDeviceImageFormatProperties(d.physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage,
                                                 0, &limits) != VK_SUCCESS ||
        desc.width > limits.maxExtent.width || desc.height > limits.maxExtent.height)
        return {};

    auto* t = new vk::TextureObject;
    Texture texture = Texture::adopt(t);
    t->device = handle_;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {desc.width, desc.height, 1};
    info.mipLevels = levels;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(d.device, &info, nullptr, &t->image) != VK_SUCCESS)
        return {};
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(d.device, t->image, &req);
    const uint32_t type = d.memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (type == ~0u || !d.allocator.allocate(req, type, true, t->memory))
        return {};
    vkBindImageMemory(d.device, t->image, t->memory.memory, t->memory.offset);
    t->vkFormat = format;
    t->format = uint8_t(desc.format);
    t->width = desc.width;
    t->height = desc.height;
    t->mipLevels = levels;
    t->bytes = size_t(req.size);
    t->view = vk::makeView(d.device, t->image, format, levels);
    t->view0 = levels == 1 ? t->view : vk::makeView(d.device, t->image, format, 1);
    t->resting = has(desc.usage, Usage::Sampled)        ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                 : has(desc.usage, Usage::RenderTarget) ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                 : has(desc.usage, Usage::Storage)      ? VK_IMAGE_LAYOUT_GENERAL
                                                        : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    return texture;
}

Buffer Device::makeBuffer(size_t bytes, Memory memory) const
{
    vk::DeviceObject& d = *handle_;
    auto* b = new vk::BufferObject;
    Buffer buffer = Buffer::adopt(b);
    b->device = handle_;
    b->size = bytes;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = std::max<size_t>(bytes, 4);
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (vkCreateBuffer(d.device, &info, nullptr, &b->buffer) != VK_SUCCESS)
        return {};
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d.device, b->buffer, &req);
    constexpr VkMemoryPropertyFlags kHost = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const uint32_t type =
        memory == Memory::Device   ? d.memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)
        : memory == Memory::Shared ? d.memoryType(req.memoryTypeBits, kHost, VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
                                   : d.memoryType(req.memoryTypeBits, kHost, 0);
    if (type == ~0u || !d.allocator.allocate(req, type, false, b->memory))
        return {};
    vkBindBufferMemory(d.device, b->buffer, b->memory.memory, b->memory.offset);
    return buffer;
}

Queue Device::makeQueue() const
{
    vk::DeviceObject& d = *handle_;
    auto q = std::make_shared<vk::QueueObject>();
    q->device = handle_;
    // The last slot is the uploader's when there are several.
    const uint32_t shared = d.slots.size() > 1 ? uint32_t(d.slots.size()) - 1 : 1;
    q->slot = d.slots[d.nextSlot++ % shared].get();
    Queue queue;
    queue.handle_ = std::move(q);
    return queue;
}

Fence Device::makeFence() const
{
    auto f = std::make_shared<vk::FenceObject>();
    f->device = handle_;
    f->semaphore = vk::makeTimeline(handle_->device);
    Fence fence;
    fence.handle_ = std::move(f);
    return fence;
}

TimestampMode Device::timestampMode() const
{
    return handle_->timestamps ? TimestampMode::StageBoundary : TimestampMode::None;
}

TimestampQueries Device::makeTimestampQueries(uint32_t count) const
{
    TimestampQueries queries;
    if (!handle_->timestamps || count == 0)
        return queries;
    auto q = std::make_shared<vk::QueryObject>();
    q->device = handle_;
    q->count = count;
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = count;
    if (vkCreateQueryPool(handle_->device, &info, nullptr, &q->pool) != VK_SUCCESS)
        return queries;
    vkResetQueryPool(handle_->device, q->pool, 0, count);
    queries.handle_ = std::move(q);
    return queries;
}

bool Device::upload(const Texture& texture, uint32_t level, const void* data, size_t rowBytes) const
{
    vk::TextureObject* t = texture.native();
    if (!t || level >= t->mipLevels)
        return false;
    vk::DeviceObject& d = *handle_;
    vk::Uploader& u = d.uploader;
    const Format format = Format(t->format);
    const uint32_t width = std::max(1u, t->width >> level), height = std::max(1u, t->height >> level);
    const size_t tight = gpu::rowBytes(format, width);
    const uint32_t rows = rowCount(format, height);
    const VkDeviceSize bytes = VkDeviceSize(tight) * rows;
    if (bytes > vk::kStagingBytes)
        return false;

    std::unique_lock lock(u.mutex);
    // Copies start at a multiple of every format's texel or block size.
    VkDeviceSize start = vk::alignUp(u.head, 16);
    if (start + bytes > vk::kStagingBytes)
        start = 0;
    const VkDeviceSize end = start + bytes;
    while (!u.spans.empty()) {
        const vk::Uploader::Span& span = u.spans.front();
        const bool overlaps = span.start < end && start < span.end;
        if (!overlaps && vk::counter(d, u.slot->timeline) < span.value)
            break;
        vk::waitValue(d, u.slot->timeline, span.value, UINT64_MAX);
        u.spans.pop_front();
    }
    u.head = end;
    for (uint32_t r = 0; r < rows; ++r)
        std::memcpy(u.memory.mapped + start + VkDeviceSize(r) * tight, static_cast<const uint8_t*>(data) + r * rowBytes,
                    tight);

    vk::Uploader::Recording& rec = u.recordings[u.next++ % vk::kUploadCommands];
    if (rec.value)
        vk::waitValue(d, u.slot->timeline, rec.value, UINT64_MAX);
    vkResetCommandPool(d.device, rec.pool, 0);
    vk::beginRecording(rec.cmd);
    const bool whole = t->mipLevels == 1;
    vk::barrier(rec.cmd, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, whole);
    const VkBufferImageCopy copy = vk::region(*t, level, start);
    vkCmdCopyBufferToImage(rec.cmd, u.staging, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    vk::barrier(rec.cmd, t, t->resting);
    vkEndCommandBuffer(rec.cmd);
    VkCommandBufferSubmitInfo cmd{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmd.commandBuffer = rec.cmd;
    std::vector<VkSemaphoreSubmitInfo> none, signals;
    rec.value = vk::submit(d, *u.slot, &cmd, 1, none, signals, nullptr);
    u.spans.push_back({start, end, rec.value});
    const uint64_t value = rec.value;
    lock.unlock();
    return vk::waitValue(d, u.slot->timeline, value, UINT64_MAX);
}

ShaderModule Device::makeShaderModule(const ShaderSource& source, std::string* error) const
{
    ShaderModule m;
    if (source.spirv.empty()) {
        vk::setError(error, "shader compile failed: no SPIR-V");
        return m;
    }
    auto module = std::make_shared<vk::ShaderModuleObject>();
    module->device = handle_;
    for (const ShaderSource::Spirv& spirv : source.spirv) {
        vk::ShaderModuleObject::Entry entry;
        entry.name = spirv.name;
        std::vector<uint32_t> code = spirv.code;
        vk::SpirvInterface face;
        std::string message;
        if (!vk::renumber(code, face, message)) {
            vk::setError(error, spirv.name + ": " + message);
            return m;
        }
        entry.stage = face.stage;
        entry.bindings = std::move(face.bindings);
        entry.localX = face.localX;
        entry.localY = face.localY;
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = code.size() * sizeof(uint32_t);
        info.pCode = code.data();
        if (vkCreateShaderModule(handle_->device, &info, nullptr, &entry.module) != VK_SUCCESS) {
            vk::setError(error, spirv.name + ": shader module creation failed");
            return m;
        }
        module->entries.push_back(std::move(entry));
    }
    m.handle_ = std::move(module);
    return m;
}

RenderPipeline Device::makeRenderPipeline(const RenderPipelineDesc& desc, const PipelineCache& cache, bool* cached,
                                          std::string* error) const
{
    vk::DeviceObject& d = *handle_;
    RenderPipeline result;
    const auto* vertex = vk::findEntry(desc.module.native(), desc.vertex);
    const auto* fragment = vk::findEntry(desc.module.native(), desc.fragment);
    if (!vertex || !fragment) {
        vk::setError(error, "pipeline creation failed: no entry " + (vertex ? desc.fragment : desc.vertex));
        return result;
    }
    auto p = std::make_shared<vk::PipelineObject>();
    p->device = handle_;
    if (!vk::makeLayout(d, *p, {vertex, fragment}, error))
        return result;

    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex->module;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment->module;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    const VkPipelineColorBlendAttachmentState attachment = vk::blendOf(desc.blending);
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    const VkDynamicState dynamic[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamics{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamics.dynamicStateCount = 2;
    dynamics.pDynamicStates = dynamic;
    const VkFormat format = vk::vulkanFormat(desc.format);
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamics;
    info.layout = p->layout;
    const VkResult made = vk::createCached(d, cache, info, cached, p->pipeline);
    if (made != VK_SUCCESS || !p->pipeline) {
        vk::setError(error, "pipeline creation failed (" + std::to_string(int(made)) + ")");
        return result;
    }
    p->bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    result.handle_ = std::move(p);
    return result;
}

ComputePipeline Device::makeComputePipeline(const ShaderModule& module, std::string_view function,
                                            std::string* error) const
{
    vk::DeviceObject& d = *handle_;
    ComputePipeline result;
    const auto* entry = vk::findEntry(module.native(), std::string(function));
    if (!entry || entry->stage != VK_SHADER_STAGE_COMPUTE_BIT) {
        vk::setError(error, "compute pipeline creation failed: no compute entry " + std::string(function));
        return result;
    }
    auto p = std::make_shared<vk::PipelineObject>();
    p->device = handle_;
    if (!vk::makeLayout(d, *p, {entry}, error))
        return result;
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = entry->module;
    info.stage.pName = "main";
    info.layout = p->layout;
    if (vkCreateComputePipelines(d.device, VK_NULL_HANDLE, 1, &info, nullptr, &p->pipeline) != VK_SUCCESS) {
        vk::setError(error, "compute pipeline creation failed");
        return result;
    }
    p->bindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
    p->localX = entry->localX;
    p->localY = entry->localY;
    result.handle_ = std::move(p);
    return result;
}

PipelineCache Device::openPipelineCache(const std::string& path) const
{
    vk::DeviceObject& d = *handle_;
    std::vector<char> data;
    if (!path.empty()) {
        std::ifstream file(vk::pathOf(path), std::ios::binary);
        data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    if (!vk::cacheHeaderMatches(d, data))
        data.clear();
    auto c = std::make_shared<vk::PipelineCacheObject>();
    c->device = handle_;
    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info.initialDataSize = data.size();
    info.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(d.device, &info, nullptr, &c->cache) != VK_SUCCESS) {
        info.initialDataSize = 0;
        info.pInitialData = nullptr;
        if (vkCreatePipelineCache(d.device, &info, nullptr, &c->cache) != VK_SUCCESS)
            return {};
    }
    PipelineCache cache;
    cache.handle_ = std::move(c);
    return cache;
}

bool Device::savePipelineCache(const PipelineCache& cache, const std::string& path, std::string* error) const
{
    VkDevice dev = handle_->device;
    size_t size = 0;
    std::vector<char> data;
    if (vkGetPipelineCacheData(dev, cache.native()->cache, &size, nullptr) == VK_SUCCESS) {
        data.resize(size);
        if (vkGetPipelineCacheData(dev, cache.native()->cache, &size, data.data()) != VK_SUCCESS)
            data.clear();
    }
    const std::filesystem::path target = vk::pathOf(path);
    const std::filesystem::path temporary = vk::pathOf(path + ".tmp");
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(data.data(), std::streamsize(data.size()));
        if (data.empty() || !file) {
            vk::setError(error, "pipeline cache not written: " + path);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, target, ec);
    if (ec)
        vk::setError(error, "pipeline cache not written: " + ec.message());
    return !ec;
}

bool TimestampQueries::resolve(uint32_t first, std::span<uint64_t> ticks) const
{
    if (ticks.empty() || first + ticks.size() > handle_->count)
        return false;
    std::vector<uint64_t> results(ticks.size() * 2);
    const VkResult r = vkGetQueryPoolResults(handle_->device->device, handle_->pool, first, uint32_t(ticks.size()),
                                             results.size() * sizeof(uint64_t), results.data(), 2 * sizeof(uint64_t),
                                             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (r != VK_SUCCESS && r != VK_NOT_READY)
        return false;
    for (size_t i = 0; i < ticks.size(); ++i)
        ticks[i] = results[2 * i + 1] ? results[2 * i] : 0;
    return true;
}

RenderPassSetup CommandBuffer::setup(const RenderTarget& target)
{
    cmd_->target = target;
    return RenderPassSetup(cmd_);
}

RenderPass CommandBuffer::begin(const RenderPassSetup& setup)
{
    vk::CommandObject* c = setup.native();
    vk::TextureObject* t = c->target.texture->native();
    for (const TimestampWrite& write : c->target.timestamps) {
        if (!write.queries)
            continue;
        VkQueryPool pool = write.queries->native()->pool;
        vkCmdResetQueryPool(c->cmd, pool, write.start, 1);
        vkCmdResetQueryPool(c->cmd, pool, write.end, 1);
        vkCmdWriteTimestamp2(c->cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, pool, write.start);
        vk::retain(c, write.queries->native()->shared_from_this());
    }
    vk::retain(c, t->shared_from_this());
    vk::barrier(c->cmd, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, c->target.load != Load::Keep);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = t->view0;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = c->target.load == Load::Clear  ? VK_ATTACHMENT_LOAD_OP_CLEAR
                   : c->target.load == Load::Keep ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                  : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    const ClearColor& k = c->target.clear;
    color.clearValue.color = {{float(k.r), float(k.g), float(k.b), float(k.a)}};
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea = {{0, 0}, {t->width, t->height}};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &color;
    vkCmdBeginRendering(c->cmd, &info);
    // Flipped, so clip space points up as on Metal and the same vertex shaders draw the same picture.
    const VkViewport viewport{0.0f, float(t->height), float(t->width), -float(t->height), 0.0f, 1.0f};
    vkCmdSetViewport(c->cmd, 0, 1, &viewport);
    vk::setScissor(c, 0, 0, t->width, t->height);
    c->rendering = true;
    c->pipeline = nullptr;
    c->textures = {};
    c->buffers = {};
    c->dirty = true;
    return RenderPass(c);
}

void CommandBuffer::copy(const Texture& from, const Texture& to)
{
    vk::TextureObject* a = from.native();
    vk::TextureObject* b = to.native();
    vk::barrier(cmd_->cmd, a, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vk::barrier(cmd_->cmd, b, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true);
    // A 2D texture has at most 32 levels.
    std::array<VkImageCopy, 32> regions{};
    const uint32_t levels = std::min(a->mipLevels, b->mipLevels);
    for (uint32_t level = 0; level < levels; ++level) {
        VkImageCopy& r = regions[level];
        r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        r.extent = {std::max(1u, a->width >> level), std::max(1u, a->height >> level), 1};
    }
    vkCmdCopyImage(cmd_->cmd, a->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b->image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels, regions.data());
    vk::barrier(cmd_->cmd, a, a->resting);
    vk::barrier(cmd_->cmd, b, b->resting);
    vk::retain(cmd_, a->shared_from_this());
    vk::retain(cmd_, b->shared_from_this());
}

void CommandBuffer::copy(const Texture& from, const Buffer& to, size_t rowBytes, size_t offset, uint32_t level)
{
    vk::TextureObject* t = from.native();
    vk::barrier(cmd_->cmd, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy r = vk::region(*t, level, offset);
    r.bufferRowLength = vk::texelsPerRow(Format(t->format), rowBytes);
    vkCmdCopyImageToBuffer(cmd_->cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to.native()->buffer, 1, &r);
    vk::barrier(cmd_->cmd, t, t->resting);
    vk::memoryBarrier(cmd_->cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    vk::retain(cmd_, t->shared_from_this());
    vk::retain(cmd_, to.native()->shared_from_this());
}

void CommandBuffer::synchronize(const Texture&)
{
}

namespace vulkan {

VkInstance instance(const Device& device)
{
    return device.native()->instance;
}

VkPhysicalDevice physicalDevice(const Device& device)
{
    return device.native()->physical;
}

VkDevice logicalDevice(const Device& device)
{
    return device.native()->device;
}

uint32_t queueFamily(const Device& device)
{
    return device.native()->family;
}

uint64_t validationErrors()
{
    return vk::gValidationErrors.load();
}

bool validationEnabled()
{
    return vk::gValidation.load();
}

Texture wrapSwapchainImage(const Device& device, VkImage image, VkFormat format, uint32_t width, uint32_t height)
{
    auto* t = new vk::TextureObject;
    Texture texture = Texture::adopt(t);
    t->device = device.native()->shared_from_this();
    t->image = image;
    t->ownsImage = false;
    t->vkFormat = format;
    t->format = uint8_t(vk::fromVulkan(format));
    t->width = width;
    t->height = height;
    t->view = t->view0 = vk::makeView(t->device->device, image, format, 1);
    t->resting = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    return texture;
}

void waitSemaphore(CommandBuffer& cmd, VkSemaphore semaphore, VkPipelineStageFlags2 stage)
{
    cmd.native()->waits.push_back({VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, semaphore, 0, stage, 0});
}

void signalSemaphore(CommandBuffer& cmd, VkSemaphore semaphore)
{
    cmd.native()->signals.push_back(
        {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, semaphore, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0});
}

VkResult present(const Queue& queue, const VkPresentInfoKHR& info)
{
    vk::QueueSlot& slot = *queue.native()->slot;
    std::lock_guard lock(slot.mutex);
    return vkQueuePresentKHR(slot.queue, &info);
}

#if defined(_WIN32)
VkSurfaceKHR makeSurface(const Device& device, void* hinstance, void* hwnd)
{
    VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    info.hinstance = static_cast<HINSTANCE>(hinstance);
    info.hwnd = static_cast<HWND>(hwnd);
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (vkCreateWin32SurfaceKHR(device.native()->instance, &info, nullptr, &surface) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return surface;
}
#endif

} // namespace vulkan

} // namespace azl::gpu
