#include "engine/gpu_profile.hpp"

#include <objc/message.h>
#include <objc/runtime.h>

#include <algorithm>
#include <atomic>
#include <utility>

namespace azl {

namespace {

constexpr size_t kBuffers = 6;
// Counter ticks are calibrated against command buffer start times this many seconds apart before anything counts.
constexpr double kCalibration = 0.5;

std::atomic<bool> noCounterSampling{false};

bool responds(const void* object, const char* selector)
{
    return object && class_respondsToSelector(object_getClass((id)object), sel_registerName(selector));
}

const void* bytes(NS::Data* data)
{
    return reinterpret_cast<const void* (*)(id, SEL)>(objc_msgSend)((id)data, sel_registerName("bytes"));
}

MTL::CounterSet* timestampCounters(MTL::Device* device)
{
    if (noCounterSampling || !responds(device, "counterSets") || !responds(device, "supportsCounterSampling:") ||
        !responds(device, "newCounterSampleBufferWithDescriptor:error:"))
        return nullptr;
    NS::Array* sets = device->counterSets();
    for (NS::UInteger i = 0; sets && i < sets->count(); ++i) {
        auto* set = sets->object<MTL::CounterSet>(i);
        if (responds(set, "name") && set->name()->isEqualToString(MTL::CommonCounterSetTimestamp))
            return set;
    }
    return nullptr;
}

bool samplesAtStages(MTL::Device* device, NS::UInteger attachment)
{
    if (!device->supportsCounterSampling(MTL::CounterSamplingPointAtStageBoundary))
        return false;
    auto desc = NS::TransferPtr(MTL::RenderPassDescriptor::alloc()->init());
    if (!responds(desc.get(), "sampleBufferAttachments"))
        return false;
    auto* attachments = desc->sampleBufferAttachments();
    if (!responds(attachments, "objectAtIndexedSubscript:"))
        return false;
    auto* a = attachments->object(attachment);
    return responds(a, "setSampleBuffer:") && responds(a, "setStartOfVertexSampleIndex:") &&
           responds(a, "setEndOfVertexSampleIndex:") && responds(a, "setStartOfFragmentSampleIndex:") &&
           responds(a, "setEndOfFragmentSampleIndex:");
}

bool samplesAtDraws(MTL::Device* device)
{
    if (!device->supportsCounterSampling(MTL::CounterSamplingPointAtDrawBoundary))
        return false;
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    auto queue = NS::TransferPtr(device->newCommandQueue());
    auto* texDesc = MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatBGRA8Unorm, 1, 1, false);
    texDesc->setUsage(MTL::TextureUsageRenderTarget);
    texDesc->setStorageMode(MTL::StorageModePrivate);
    auto texture = NS::TransferPtr(device->newTexture(texDesc));
    auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    pass->colorAttachments()->object(0)->setTexture(texture.get());
    auto* enc = queue->commandBuffer()->renderCommandEncoder(pass);
    const bool ok = responds(enc, "sampleCountersInBuffer:atSampleIndex:withBarrier:");
    if (enc)
        enc->endEncoding();
    pool->release();
    return ok;
}

} // namespace

void GpuProfile::forceNoCounterSampling(bool force)
{
    noCounterSampling = force;
}

GpuProfile::GpuProfile(MTL::Device* device, NS::UInteger attachment, bool commandBuffer)
    : attachment_(attachment), device_(NS::RetainPtr(device))
{
    if (MTL::CounterSet* set = timestampCounters(device)) {
        timestamps_ = NS::RetainPtr(set);
        mode_ = samplesAtStages(device, attachment) ? Mode::Stage : samplesAtDraws(device) ? Mode::Draw : Mode::Off;
    }
    if (mode_ != Mode::Off) {
        auto desc = NS::TransferPtr(MTL::CounterSampleBufferDescriptor::alloc()->init());
        desc->setCounterSet(timestamps_.get());
        desc->setStorageMode(MTL::StorageModeShared);
        desc->setSampleCount(2 * kMaxPasses);
        for (size_t i = 0; i < kBuffers; ++i) {
            NS::Error* error = nullptr;
            auto buffer = NS::TransferPtr(device->newCounterSampleBuffer(desc.get(), &error));
            if (!buffer || !responds(buffer.get(), "resolveCounterRange:")) {
                mode_ = Mode::Off;
                free_.clear();
                break;
            }
            free_.push_back(std::move(buffer));
        }
    }
    if (mode_ == Mode::Off && commandBuffer)
        mode_ = Mode::CommandBuffer;
}

const char* GpuProfile::mode() const
{
    switch (mode_) {
    case Mode::Stage: return "stage boundary";
    case Mode::Draw: return "draw boundary";
    case Mode::CommandBuffer: return "command buffer";
    case Mode::Off: break;
    }
    return "off";
}

std::shared_ptr<GpuProfile::Frame> GpuProfile::begin()
{
    if (mode_ == Mode::Off)
        return nullptr;
    if (mode_ == Mode::CommandBuffer)
        return std::make_shared<Frame>();
    std::lock_guard lock(mutex_);
    if (free_.empty())
        return nullptr;
    auto frame = std::make_shared<Frame>();
    frame->buffer = std::move(free_.back());
    free_.pop_back();
    return frame;
}

void GpuProfile::attach(Frame& frame, MTL::RenderPassDescriptor* desc, std::string label)
{
    if (frame.labels.size() == kMaxPasses)
        return;
    const NS::UInteger index = 2 * frame.labels.size();
    frame.labels.push_back(std::move(label));
    if (mode_ != Mode::Stage)
        return;
    auto* attachment = desc->sampleBufferAttachments()->object(attachment_);
    attachment->setSampleBuffer(frame.buffer.get());
    attachment->setStartOfVertexSampleIndex(index);
    attachment->setEndOfVertexSampleIndex(MTL::CounterDontSample);
    attachment->setStartOfFragmentSampleIndex(MTL::CounterDontSample);
    attachment->setEndOfFragmentSampleIndex(index + 1);
}

void GpuProfile::sample(Frame& frame, MTL::RenderCommandEncoder* enc, bool start)
{
    if (mode_ == Mode::Draw && !frame.labels.empty())
        enc->sampleCountersInBuffer(frame.buffer.get(), 2 * (frame.labels.size() - 1) + (start ? 0 : 1), true);
}

uint64_t GpuProfile::finish(Frame& frame, bool ok, double gpuStart, double gpuEnd)
{
    if (mode_ == Mode::CommandBuffer) {
        if (!ok || frame.labels.empty() || frame.passes == 0 || gpuEnd <= gpuStart)
            return 0;
        const double share = double(std::min(frame.labels.size(), frame.passes)) / double(frame.passes);
        return uint64_t((gpuEnd - gpuStart) * 1e6 * share);
    }
    const NS::UInteger count = 2 * frame.labels.size();
    NS::Data* data = ok && count ? frame.buffer->resolveCounterRange(NS::Range(0, count)) : nullptr;
    const auto* stamps = responds(data, "bytes") && responds(data, "length") &&
                                 data->length() >= count * sizeof(MTL::CounterResultTimestamp)
                             ? static_cast<const MTL::CounterResultTimestamp*>(bytes(data))
                             : nullptr;
    if (stamps && (stamps[0].timestamp == 0 || stamps[0].timestamp == MTL::CounterErrorValue))
        stamps = nullptr;
    std::lock_guard lock(mutex_);
    free_.push_back(std::move(frame.buffer));
    if (!stamps)
        return 0;
    if (!anchor_)
        anchor_ = {gpuStart, stamps[0].timestamp};
    else if (gpuStart - anchor_->first >= kCalibration && stamps[0].timestamp > anchor_->second)
        microsPerTick_ = (gpuStart - anchor_->first) * 1e6 / double(stamps[0].timestamp - anchor_->second);
    if (microsPerTick_ == 0.0)
        return 0;
    std::map<std::string, uint64_t> micros;
    uint64_t total = 0;
    for (size_t i = 0; i < frame.labels.size(); ++i) {
        const uint64_t start = stamps[2 * i].timestamp;
        const uint64_t end = stamps[2 * i + 1].timestamp;
        if (start == 0 || end <= start || end == MTL::CounterErrorValue)
            continue;
        const std::string& label = frame.labels[i];
        const auto us = uint64_t(double(end - start) * microsPerTick_);
        micros[label] += us;
        total += us;
        if (const size_t bar = label.find(" | "); bar != std::string::npos)
            micros[label.substr(0, bar)] += us;
    }
    for (const auto& [label, us] : micros) {
        totals_[label].micros += us;
        totals_[label].frames++;
    }
    return total;
}

std::map<std::string, GpuProfile::Total> GpuProfile::take()
{
    std::lock_guard lock(mutex_);
    return std::exchange(totals_, {});
}

} // namespace azl
