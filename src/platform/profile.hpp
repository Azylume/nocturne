#pragma once

// Tracy instrumentation, built with -DNOCTURNE_TRACY=ON. In the default build every macro is empty and nothing
// below links Tracy: no profiler thread, no socket.

#include <string_view>

namespace MTL {
class BlitPassDescriptor;
class ComputePassDescriptor;
class Device;
class RenderPassDescriptor;
} // namespace MTL

#ifdef NOCTURNE_TRACY

#include <tracy/Tracy.hpp>

#include <functional>
#include <mutex>
#include <set>
#include <string>

namespace azl::profile {

inline constexpr bool kEnabled = true;

// Tracy tells frame sets apart by the address of their name, so each name lives as long as the process.
inline const char* persistentName(std::string_view name)
{
    static std::mutex mutex;
    static std::set<std::string, std::less<>> names;
    std::lock_guard lock(mutex);
    return names.emplace(name).first->c_str();
}

// One Tracy GPU timeline, for one command queue. Its Metal timestamp buffers are made once a profiler connects. Without
// timestamps at stage boundaries (older Intel Macs) it records nothing.
class GpuTimeline {
public:
    GpuTimeline(MTL::Device* device, std::string_view prefix, std::string_view name);
    // Collects finished encoders' timestamps: from the queue's command buffer completion handlers.
    void collect() const;

    struct Context;
    Context* context() const { return context_; }

private:
    Context* context_ = nullptr;
};

// Times one encoder: made before the encoder from `desc`, alive at least until the encoder is made.
class GpuZone {
public:
    GpuZone(const GpuTimeline* timeline, MTL::RenderPassDescriptor* desc, const tracy::SourceLocationData* where);
    GpuZone(const GpuTimeline* timeline, MTL::ComputePassDescriptor* desc, const tracy::SourceLocationData* where);
    GpuZone(const GpuTimeline* timeline, MTL::BlitPassDescriptor* desc, const tracy::SourceLocationData* where);
    ~GpuZone();
    GpuZone(const GpuZone&) = delete;
    GpuZone& operator=(const GpuZone&) = delete;

private:
    void* scope_ = nullptr;
};

} // namespace azl::profile

#define AZL_ZONE(name) ZoneScopedN(name)
#define AZL_FRAME_MARK FrameMark
#define AZL_FRAME_MARK_NAMED(name) FrameMarkNamed(::azl::profile::persistentName(name))
#define AZL_THREAD_NAME(name) ::tracy::SetThreadName(std::string(name).c_str())
#define AZL_GPU_ZONE(timeline, desc, name)                                                                            \
    static constexpr ::tracy::SourceLocationData TracyConcat(azl_gpu_where_, TracyLine){                              \
        name, TracyFunction, TracyFile, uint32_t(TracyLine), 0};                                                      \
    ::azl::profile::GpuZone TracyConcat(azl_gpu_zone_, TracyLine)(timeline, desc,                                     \
                                                                  &TracyConcat(azl_gpu_where_, TracyLine))
#define AZL_GPU_COLLECT(timeline) (timeline).collect()

#else

namespace azl::profile {

inline constexpr bool kEnabled = false;

// Takes any backend's device: the Tracy build, which has GPU timelines, is Apple's.
struct GpuTimeline {
    template <typename Device>
    GpuTimeline(Device*, std::string_view, std::string_view)
    {
    }
};

} // namespace azl::profile

#define AZL_ZONE(name)
#define AZL_FRAME_MARK
#define AZL_FRAME_MARK_NAMED(name)
#define AZL_THREAD_NAME(name)
#define AZL_GPU_ZONE(timeline, desc, name)
#define AZL_GPU_COLLECT(timeline) static_cast<void>(timeline)

#endif
