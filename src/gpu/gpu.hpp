#pragma once

// The GPU interface the engine, media, outputs and the harness draw through (ADR 0016). One backend is compiled in:
// Metal on Apple, Vulkan elsewhere. Every class here is concrete; its members and its per-draw methods come from the
// backend's header, inline, so a draw costs what the backend's own call costs and no virtual call is made.
//
// Lifetimes: Device, Queue, Texture, Buffer, pipelines, Fence and TimestampQueries are reference-counted handles, cheap
// to copy and safe to release on any thread. CommandBuffer, RenderPass and ComputePass are views that live inside one
// Scope on the thread that encodes them; a committed command buffer keeps everything it uses alive until it completes.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if defined(__APPLE__)
#include "gpu/gpu_metal_types.hpp"
#else
#include "gpu/gpu_vulkan_types.hpp"
#endif

namespace azl::gpu {

enum class Format : uint8_t {
    Invalid,
    BGRA8,
    RGBA8,
    RGBA16F,
    RGBA16,
    R8,
    RG8,
    // 16-bit unsigned normalised (RGBA16 too): P010 planes keep 10 bits in the high bits.
    R16,
    RG16,
    // Rows of 4x4 blocks: HAP frames.
    BC1,
    BC3,
    BC4,
};

bool isCompressed(Format format);
// Bytes of one texel, or of one 4x4 block for the BC formats.
uint32_t unitBytes(Format format);
// Bytes of one row of texels (BC: of blocks) of a level `width` pixels wide, tightly packed.
size_t rowBytes(Format format, uint32_t width);
// Rows of texels (BC: of blocks) of a level `height` pixels high.
uint32_t rowCount(Format format, uint32_t height);

enum class Usage : uint32_t {
    Sampled = 1,
    RenderTarget = 2,
    Storage = 4,
};
constexpr Usage operator|(Usage a, Usage b)
{
    return Usage(uint32_t(a) | uint32_t(b));
}
constexpr bool has(Usage set, Usage bit)
{
    return (uint32_t(set) & uint32_t(bit)) != 0;
}

enum class Memory : uint8_t {
    // Only the GPU reads and writes it.
    Device,
    // The CPU writes it with Device::upload, the GPU reads it.
    Upload,
    // The CPU reads what the GPU wrote, after CommandBuffer::synchronize and completion. Buffers: mapped, coherent.
    Shared,
};

struct TextureDesc {
    Format format = Format::BGRA8;
    uint32_t width = 1;
    uint32_t height = 1;
    // 0: a full chain down to 1x1.
    uint32_t mipLevels = 1;
    Usage usage = Usage::Sampled;
    Memory memory = Memory::Device;
};

// What a texture is used for next. Metal tracks this itself and ignores it; Vulkan turns it into a layout transition
// and barrier.
enum class Layout : uint8_t { Undefined, RenderTarget, Sampled, Storage, CopySource, CopyTarget, Present };

// Fixed-function blending of a premultiplied colour into the target.
enum class Blending { None, Normal, Add, Screen, Multiply };

class Texture {
public:
    Texture() = default;
    explicit operator bool() const;
    bool operator==(const Texture& other) const { return native() == other.native(); }

    uint32_t width() const;
    uint32_t height() const;
    Format format() const;
    uint32_t mipLevels() const;
    size_t allocatedBytes() const;

    // Backend code and the call sites not yet on this interface.
    NativeTexture native() const;
    // Shares a texture the backend made elsewhere (a drawable's, a decoder's).
    static Texture retain(NativeTexture texture);
    static Texture adopt(NativeTexture texture);

private:
    TextureHandle handle_;
};

// The native handle of a texture that may be absent, for the call sites not yet on this interface.
inline NativeTexture native(const Texture* texture);

class Buffer {
public:
    Buffer() = default;
    explicit operator bool() const;
    size_t size() const;
    // Mapped bytes of a Shared or Upload buffer.
    void* contents() const;
    NativeBuffer native() const;
    static Buffer adopt(NativeBuffer buffer);

private:
    BufferHandle handle_;
};

// Compiled shader code: MSL on Metal, SPIR-V on Vulkan.
struct ShaderSource {
    std::string msl;
    std::vector<uint32_t> spirv;
};

class ShaderModule {
public:
    ShaderModule() = default;
    explicit operator bool() const;
    NativeShaderModule native() const;
    static ShaderModule retain(NativeShaderModule module);

private:
    friend class Device;
    ShaderModuleHandle handle_;
};

// A file of compiled pipelines, so a pipeline built once builds without compiling the next time (Metal binary
// archive, VkPipelineCache). Compile thread only.
class PipelineCache {
public:
    PipelineCache() = default;
    explicit operator bool() const;
    NativePipelineCache native() const;
    static PipelineCache retain(NativePipelineCache cache);

private:
    friend class Device;
    PipelineCacheHandle handle_;
};

struct RenderPipelineDesc {
    ShaderModule module;
    std::string vertex;
    std::string fragment;
    Format format = Format::BGRA8;
    Blending blending = Blending::None;
};

class RenderPipeline {
public:
    RenderPipeline() = default;
    explicit operator bool() const;
    NativeRenderPipeline native() const;
    static RenderPipeline retain(NativeRenderPipeline pipeline);

private:
    friend class Device;
    RenderPipelineHandle handle_;
};

class ComputePipeline {
public:
    ComputePipeline() = default;
    explicit operator bool() const;
    // Threads one group may hold.
    uint32_t maxThreads() const;
    NativeComputePipeline native() const;
    static ComputePipeline retain(NativeComputePipeline pipeline);

private:
    friend class Device;
    ComputePipelineHandle handle_;
};

// A GPU timeline value, like a Vulkan timeline semaphore: command buffers signal and wait for values, the CPU reads
// or waits for them.
class Fence {
public:
    Fence() = default;
    explicit operator bool() const;
    uint64_t value() const;
    // Blocks the calling thread (never the render thread) until the value is reached; false after `timeoutMs`.
    bool wait(uint64_t value, uint64_t timeoutMs) const;
    NativeFence native() const;

private:
    friend class Device;
    FenceHandle handle_;
};

// Where the device can take GPU timestamps: around a pass's stages (Apple GPUs, Vulkan), or only as commands between
// draws (Intel Macs), or nowhere.
enum class TimestampMode : uint8_t { None, StageBoundary, DrawBoundary };

class TimestampQueries {
public:
    TimestampQueries() = default;
    explicit operator bool() const;
    uint32_t count() const;
    // Once the command buffer that wrote them has completed: `ticks.size()` timestamps from `first`, 0 where none was
    // written. Ticks are in the backend's unit; callers calibrate against Completion times. False when unreadable.
    bool resolve(uint32_t first, std::span<uint64_t> ticks) const;
    NativeTimestampQueries native() const;

private:
    friend class Device;
    TimestampQueriesHandle handle_;
};

// A render pass's start and end timestamps, written at its stage boundaries. Each profiler of a pass takes one of its
// kTimestampSlots, so several can time the same pass.
inline constexpr size_t kTimestampSlots = 4;
struct TimestampWrite {
    const TimestampQueries* queries = nullptr;
    uint32_t start = 0;
    uint32_t end = 0;
};

enum class Load : uint8_t { Clear, Keep, DontCare };

struct ClearColor {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double a = 0.0;
};

// What a render pass draws into. Holds no references: the texture and queries outlive the setup.
struct RenderTarget {
    const Texture* texture = nullptr;
    Load load = Load::DontCare;
    ClearColor clear{};
    std::array<TimestampWrite, kTimestampSlots> timestamps{};
};

struct Scissor {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// One render pass being encoded. Per-draw calls are inline backend calls.
class RenderPass {
public:
    explicit operator bool() const;
    void setPipeline(const RenderPipeline& pipeline);
    void setTexture(uint32_t slot, const Texture& texture);
    // Fragment-stage constants at `index`, copied at the call.
    void setBytes(uint32_t index, const void* data, size_t size);
    void setVertexBytes(uint32_t index, const void* data, size_t size);
    void setScissor(const Scissor& scissor);
    void draw(uint32_t vertices, uint32_t instances = 1);
    // A timestamp between draws: for TimestampMode::DrawBoundary.
    void writeTimestamp(const TimestampQueries& queries, uint32_t index);
    void end();

    NativeRenderPass native() const { return pass_; }
    // Wraps a pass the backend began elsewhere; never owns it.
    static RenderPass wrap(NativeRenderPass pass) { return RenderPass(pass); }

private:
    explicit RenderPass(NativeRenderPass pass) : pass_(pass) {}
    friend class CommandBuffer;
    NativeRenderPass pass_{};
};

class ComputePass {
public:
    explicit operator bool() const;
    void setPipeline(const ComputePipeline& pipeline);
    void setTexture(uint32_t slot, const Texture& texture);
    void setBytes(uint32_t index, const void* data, size_t size);
    // `width` x `height` threads in groups of `groupWidth` x `groupHeight`; partial groups at the edges.
    void dispatch(uint32_t width, uint32_t height, uint32_t groupWidth, uint32_t groupHeight);
    void end();
    NativeComputePass native() const { return pass_; }

private:
    explicit ComputePass(NativeComputePass pass) : pass_(pass) {}
    friend class CommandBuffer;
    NativeComputePass pass_{};
};

// When a command buffer ran: host-clock seconds (the steady clock's source: mach_absolute_time, QPC), 0 when unknown.
struct Completion {
    bool ok = false;
    double gpuStart = 0.0;
    double gpuEnd = 0.0;
};

// A render pass whose target and timestamps are set but which has not begun: the native descriptor is open for
// backend-specific instruments (Tracy) until `CommandBuffer::begin`.
class RenderPassSetup {
public:
    NativeRenderPassSetup native() const { return setup_; }

private:
    explicit RenderPassSetup(NativeRenderPassSetup setup) : setup_(setup) {}
    friend class CommandBuffer;
    NativeRenderPassSetup setup_{};
};

class CommandBuffer {
public:
    CommandBuffer() = default;
    explicit operator bool() const;

    RenderPassSetup setup(const RenderTarget& target);
    RenderPass begin(const RenderPassSetup& setup);
    RenderPass begin(const RenderTarget& target) { return begin(setup(target)); }
    ComputePass beginCompute();

    // Same size and format.
    void copy(const Texture& from, const Texture& to);
    // Level `level` of `from` into `to` at `offset`, rows `rowBytes` apart: a readback.
    void copy(const Texture& from, const Buffer& to, size_t rowBytes, size_t offset = 0, uint32_t level = 0);
    // Makes the GPU's writes to a Memory::Shared texture visible to the CPU once this command buffer completes.
    void synchronize(const Texture& texture);
    void transition(const Texture& texture, Layout layout);

    void signal(const Fence& fence, uint64_t value);
    void wait(const Fence& fence, uint64_t value);

    // Called once on a backend thread when the GPU is done with this command buffer, whether it ran or failed. Add
    // before commit.
    template <class F>
    void onCompleted(F&& handler);
    void commit();
    // Blocks the calling thread until the GPU is done; never on the render thread.
    void waitUntilCompleted();
    Completion completion() const;

    NativeCommandBuffer native() const { return cmd_; }
    static CommandBuffer wrap(NativeCommandBuffer cmd) { return CommandBuffer(cmd); }

private:
    explicit CommandBuffer(NativeCommandBuffer cmd) : cmd_(cmd) {}
    friend class Queue;
    NativeCommandBuffer cmd_{};
};

// One submission queue. Command buffers of one queue run in commit order.
class Queue {
public:
    Queue() = default;
    explicit operator bool() const;
    // Inside a Scope.
    CommandBuffer commandBuffer() const;
    NativeQueue native() const;

private:
    friend class Device;
    QueueHandle handle_;
};

// The autorelease scope Metal objects made on a thread need (Apple); nothing elsewhere. One around each frame's or
// present's encoding, and around each loop iteration of a thread that encodes.
class Scope {
public:
    Scope();
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    ScopeHandle handle_{};
};

class Device {
public:
    Device() = default;
    // The system's default GPU; null when there is none.
    static Device systemDefault();
    static Device retain(NativeDevice device);
    explicit operator bool() const;

    std::string name() const;
    bool unifiedMemory() const;
    // Null when the device cannot make it.
    Texture makeTexture(const TextureDesc& desc) const;
    Buffer makeBuffer(size_t bytes, Memory memory = Memory::Shared) const;
    Queue makeQueue() const;
    Fence makeFence() const;
    TimestampMode timestampMode() const;
    // Null when the device takes no timestamps.
    TimestampQueries makeTimestampQueries(uint32_t count) const;

    // Writes level `level` of a Memory::Upload texture from CPU memory, rows `rowBytes` apart (BC: rows of blocks).
    // Any thread but the render thread: a backend without CPU-visible textures copies through a staging buffer and
    // waits for that copy.
    bool upload(const Texture& texture, uint32_t level, const void* data, size_t rowBytes) const;

    // Compile thread: these compile. Null with `error` set on failure.
    ShaderModule makeShaderModule(const ShaderSource& source, std::string* error = nullptr) const;
    // With a cache, a pipeline it holds builds without compiling and sets `cached`; one it lacks is compiled and added.
    RenderPipeline makeRenderPipeline(const RenderPipelineDesc& desc, const PipelineCache& cache = {},
                                      bool* cached = nullptr, std::string* error = nullptr) const;
    ComputePipeline makeComputePipeline(const ShaderModule& module, std::string_view function,
                                        std::string* error = nullptr) const;
    // Opens the cache file at `path`, or an empty cache when it is missing or unreadable (a kill mid-write, an OS
    // update). Whether a device may keep one at all is the caller's call (archivesWork in engine/metal_util.hpp).
    PipelineCache openPipelineCache(const std::string& path) const;
    // Writes the whole cache to `path`; false with `error` set when it could not.
    bool savePipelineCache(const PipelineCache& cache, const std::string& path, std::string* error = nullptr) const;

    NativeDevice native() const;

private:
    DeviceHandle handle_;
};

// Reads one texture back into CPU memory: `encode` copies it into a buffer on the GPU, `data` is valid once that
// command buffer has completed. Rows are tightly packed.
class Readback {
public:
    Readback() = default;
    Readback(const Device& device, const Texture& texture)
        : rowBytes_(gpu::rowBytes(texture.format(), texture.width())), rows_(rowCount(texture.format(), texture.height())),
          buffer_(device.makeBuffer(rowBytes_ * rows_, Memory::Shared)), texture_(texture)
    {
    }
    explicit operator bool() const { return bool(buffer_); }
    void encode(CommandBuffer& cmd) const { cmd.copy(texture_, buffer_, rowBytes_); }
    const uint8_t* data() const { return static_cast<const uint8_t*>(buffer_.contents()); }
    size_t rowBytes() const { return rowBytes_; }
    size_t size() const { return rowBytes_ * rows_; }

private:
    size_t rowBytes_ = 0;
    size_t rows_ = 0;
    Buffer buffer_;
    Texture texture_;
};

} // namespace azl::gpu

#if defined(__APPLE__)
#include "gpu/gpu_metal_inline.hpp"
#else
#include "gpu/gpu_vulkan_inline.hpp"
#endif
