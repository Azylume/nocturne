#include "platform/profile.hpp"

#import <Metal/Metal.h>

#include <tracy/TracyMetal.hmm>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>

namespace azl::profile {

// Metal allows few counter sample buffers per device and Tracy 255 GPU contexts, so each timeline name gets one
// context, made on the first zone while a profiler is connected and kept for the process.
struct GpuTimeline::Context {
    id<MTLDevice> device;
    std::string name;
    std::once_flag made;
    std::atomic<tracy::MetalCtx*> tracy{nullptr};

    tracy::MetalCtx* get()
    {
        std::call_once(made, [this] {
            tracy::MetalCtx* context = TracyMetalContext(device);
            TracyMetalContextName(context, name.c_str(), uint16_t(name.size()));
            tracy = context;
        });
        return tracy;
    }
};

GpuTimeline::GpuTimeline(MTL::Device* device, std::string_view prefix, std::string_view name)
{
    id<MTLDevice> metal = (__bridge id<MTLDevice>)device;
    if (![metal supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary])
        return;
    static std::mutex mutex;
    static std::map<std::string, std::unique_ptr<Context>, std::less<>> contexts;
    std::string full = std::string(prefix) + std::string(name);
    std::lock_guard lock(mutex);
    std::unique_ptr<Context>& context = contexts[full];
    if (!context) {
        context = std::make_unique<Context>();
        context->device = metal;
        context->name = std::move(full);
    }
    context_ = context.get();
}

void GpuTimeline::collect() const
{
    if (tracy::MetalCtx* context = context_ ? context_->tracy.load() : nullptr)
        context->Collect();
}

namespace {

template <class Descriptor>
void* begin(const GpuTimeline* timeline, Descriptor* desc, const tracy::SourceLocationData* where)
{
    if (!timeline || !timeline->context() || !TracyIsConnected)
        return nullptr;
    return new tracy::MetalZoneScope(timeline->context()->get(), desc, where, true);
}

} // namespace

GpuZone::GpuZone(const GpuTimeline* timeline, MTL::RenderPassDescriptor* desc, const tracy::SourceLocationData* where)
    : scope_(begin(timeline, (__bridge MTLRenderPassDescriptor*)desc, where))
{
}

GpuZone::GpuZone(const GpuTimeline* timeline, MTL::ComputePassDescriptor* desc, const tracy::SourceLocationData* where)
    : scope_(begin(timeline, (__bridge MTLComputePassDescriptor*)desc, where))
{
}

GpuZone::GpuZone(const GpuTimeline* timeline, MTL::BlitPassDescriptor* desc, const tracy::SourceLocationData* where)
    : scope_(begin(timeline, (__bridge MTLBlitPassDescriptor*)desc, where))
{
}

GpuZone::~GpuZone()
{
    delete static_cast<tracy::MetalZoneScope*>(scope_);
}

} // namespace azl::profile
