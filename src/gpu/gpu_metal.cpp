#include "gpu/gpu.hpp"

#include <objc/message.h>
#include <objc/runtime.h>

#include <algorithm>

namespace azl::gpu {

namespace {

NS::String* str(const char* s)
{
    return NS::String::string(s, NS::UTF8StringEncoding);
}

void setError(std::string* error, const char* what, NS::Error* e)
{
    if (error)
        *error = std::string(what) + (e ? std::string(": ") + e->localizedDescription()->utf8String() : "");
}

bool responds(const void* object, const char* selector)
{
    return object && class_respondsToSelector(object_getClass((id)object), sel_registerName(selector));
}

const void* bytes(NS::Data* data)
{
    return reinterpret_cast<const void* (*)(id, SEL)>(objc_msgSend)((id)data, sel_registerName("bytes"));
}

MTL::TextureUsage usageOf(Usage usage)
{
    MTL::TextureUsage u = MTL::TextureUsageUnknown;
    if (has(usage, Usage::Sampled))
        u |= MTL::TextureUsageShaderRead;
    if (has(usage, Usage::RenderTarget))
        u |= MTL::TextureUsageRenderTarget;
    if (has(usage, Usage::Storage))
        u |= MTL::TextureUsageShaderWrite;
    return u;
}

MTL::StorageMode storageOf(Memory memory, bool unified)
{
    if (memory == Memory::Device)
        return MTL::StorageModePrivate;
    return unified ? MTL::StorageModeShared : MTL::StorageModeManaged;
}

MTL::LoadAction loadOf(Load load)
{
    switch (load) {
    case Load::Clear: return MTL::LoadActionClear;
    case Load::Keep: return MTL::LoadActionLoad;
    case Load::DontCare: break;
    }
    return MTL::LoadActionDontCare;
}

void setFactors(MTL::RenderPipelineColorAttachmentDescriptor* color, MTL::BlendFactor source,
                MTL::BlendFactor destination)
{
    color->setBlendingEnabled(true);
    color->setSourceRGBBlendFactor(source);
    color->setDestinationRGBBlendFactor(destination);
    color->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
    color->setDestinationAlphaBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
}

// With a premultiplied source s of alpha a over the target d:
//   normal s + d(1-a), add s + d, screen s(1-d) + d, multiply s·d + d(1-a), i.e. mix(d, d·s/a, a).
void setBlending(MTL::RenderPipelineColorAttachmentDescriptor* color, Blending blending)
{
    switch (blending) {
    case Blending::None: return;
    case Blending::Normal: return setFactors(color, MTL::BlendFactorOne, MTL::BlendFactorOneMinusSourceAlpha);
    case Blending::Add: return setFactors(color, MTL::BlendFactorOne, MTL::BlendFactorOne);
    case Blending::Screen: return setFactors(color, MTL::BlendFactorOneMinusDestinationColor, MTL::BlendFactorOne);
    case Blending::Multiply:
        return setFactors(color, MTL::BlendFactorDestinationColor, MTL::BlendFactorOneMinusSourceAlpha);
    }
}

MTL::CounterSet* timestampCounters(MTL::Device* device)
{
    if (!responds(device, "counterSets") || !responds(device, "supportsCounterSampling:") ||
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

bool samplesAtStages(MTL::Device* device)
{
    if (!device->supportsCounterSampling(MTL::CounterSamplingPointAtStageBoundary))
        return false;
    auto desc = NS::TransferPtr(MTL::RenderPassDescriptor::alloc()->init());
    if (!responds(desc.get(), "sampleBufferAttachments"))
        return false;
    auto* attachments = desc->sampleBufferAttachments();
    if (!responds(attachments, "objectAtIndexedSubscript:"))
        return false;
    auto* a = attachments->object(kTimestampSlots - 1);
    return responds(a, "setSampleBuffer:") && responds(a, "setStartOfVertexSampleIndex:") &&
           responds(a, "setEndOfVertexSampleIndex:") && responds(a, "setStartOfFragmentSampleIndex:") &&
           responds(a, "setEndOfFragmentSampleIndex:");
}

bool samplesAtDraws(MTL::Device* device)
{
    if (!device->supportsCounterSampling(MTL::CounterSamplingPointAtDrawBoundary))
        return false;
    Scope scope;
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
    return ok;
}

} // namespace

MTL::PixelFormat metalFormat(Format format)
{
    switch (format) {
    case Format::BGRA8: return MTL::PixelFormatBGRA8Unorm;
    case Format::RGBA8: return MTL::PixelFormatRGBA8Unorm;
    case Format::RGBA16F: return MTL::PixelFormatRGBA16Float;
    case Format::RGBA16: return MTL::PixelFormatRGBA16Unorm;
    case Format::R8: return MTL::PixelFormatR8Unorm;
    case Format::RG8: return MTL::PixelFormatRG8Unorm;
    case Format::R16: return MTL::PixelFormatR16Unorm;
    case Format::RG16: return MTL::PixelFormatRG16Unorm;
    case Format::BC1: return MTL::PixelFormatBC1_RGBA;
    case Format::BC3: return MTL::PixelFormatBC3_RGBA;
    case Format::BC4: return MTL::PixelFormatBC4_RUnorm;
    case Format::Invalid: break;
    }
    return MTL::PixelFormatInvalid;
}

Format fromMetal(MTL::PixelFormat format)
{
    switch (format) {
    case MTL::PixelFormatBGRA8Unorm: return Format::BGRA8;
    case MTL::PixelFormatRGBA8Unorm: return Format::RGBA8;
    case MTL::PixelFormatRGBA16Float: return Format::RGBA16F;
    case MTL::PixelFormatRGBA16Unorm: return Format::RGBA16;
    case MTL::PixelFormatR8Unorm: return Format::R8;
    case MTL::PixelFormatRG8Unorm: return Format::RG8;
    case MTL::PixelFormatR16Unorm: return Format::R16;
    case MTL::PixelFormatRG16Unorm: return Format::RG16;
    case MTL::PixelFormatBC1_RGBA: return Format::BC1;
    case MTL::PixelFormatBC3_RGBA: return Format::BC3;
    case MTL::PixelFormatBC4_RUnorm: return Format::BC4;
    default: break;
    }
    return Format::Invalid;
}

Device Device::systemDefault()
{
    Device d;
    d.handle_ = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    return d;
}

std::string Device::name() const
{
    return handle_->name()->utf8String();
}

Texture Device::makeTexture(const TextureDesc& desc) const
{
    const bool mipmapped = desc.mipLevels != 1;
    auto d = NS::RetainPtr(
        MTL::TextureDescriptor::texture2DDescriptor(metalFormat(desc.format), desc.width, desc.height, mipmapped));
    if (desc.mipLevels > 1)
        d->setMipmapLevelCount(std::min<NS::UInteger>(desc.mipLevels, d->mipmapLevelCount()));
    d->setUsage(usageOf(desc.usage));
    d->setStorageMode(storageOf(desc.memory, handle_->hasUnifiedMemory()));
    return Texture::adopt(handle_->newTexture(d.get()));
}

Buffer Device::makeBuffer(size_t bytes, Memory memory) const
{
    const auto mode = memory == Memory::Device ? MTL::ResourceStorageModePrivate : MTL::ResourceStorageModeShared;
    return Buffer::adopt(handle_->newBuffer(bytes, mode));
}

Queue Device::makeQueue() const
{
    Queue q;
    q.handle_ = NS::TransferPtr(handle_->newCommandQueue());
    return q;
}

Fence Device::makeFence() const
{
    Fence f;
    f.handle_ = NS::TransferPtr(handle_->newSharedEvent());
    return f;
}

TimestampMode Device::timestampMode() const
{
    if (!timestampCounters(handle_.get()))
        return TimestampMode::None;
    if (samplesAtStages(handle_.get()))
        return TimestampMode::StageBoundary;
    return samplesAtDraws(handle_.get()) ? TimestampMode::DrawBoundary : TimestampMode::None;
}

TimestampQueries Device::makeTimestampQueries(uint32_t count) const
{
    TimestampQueries queries;
    MTL::CounterSet* set = timestampCounters(handle_.get());
    if (!set)
        return queries;
    auto desc = NS::TransferPtr(MTL::CounterSampleBufferDescriptor::alloc()->init());
    desc->setCounterSet(set);
    desc->setStorageMode(MTL::StorageModeShared);
    desc->setSampleCount(count);
    NS::Error* error = nullptr;
    auto buffer = NS::TransferPtr(handle_->newCounterSampleBuffer(desc.get(), &error));
    if (buffer && responds(buffer.get(), "resolveCounterRange:"))
        queries.handle_ = std::move(buffer);
    return queries;
}

bool Device::upload(const Texture& texture, uint32_t level, const void* data, size_t rowBytes) const
{
    MTL::Texture* t = texture.native();
    if (!t || level >= t->mipmapLevelCount())
        return false;
    const auto width = std::max<NS::UInteger>(1, t->width() >> level);
    const auto height = std::max<NS::UInteger>(1, t->height() >> level);
    t->replaceRegion(MTL::Region(0, 0, width, height), level, data, rowBytes);
    return true;
}

ShaderModule Device::makeShaderModule(const ShaderSource& source, std::string* error) const
{
    NS::Error* e = nullptr;
    ShaderModule m;
    m.handle_ = NS::TransferPtr(handle_->newLibrary(str(source.msl.c_str()), nullptr, &e));
    if (!m)
        setError(error, "shader compile failed", e);
    return m;
}

RenderPipeline Device::makeRenderPipeline(const RenderPipelineDesc& desc, const PipelineCache& cache, bool* cached,
                                          std::string* error) const
{
    MTL::Library* library = desc.module.native();
    auto vertex = NS::TransferPtr(library->newFunction(str(desc.vertex.c_str())));
    auto fragment = NS::TransferPtr(library->newFunction(str(desc.fragment.c_str())));

    auto d = NS::TransferPtr(MTL::RenderPipelineDescriptor::alloc()->init());
    d->setVertexFunction(vertex.get());
    d->setFragmentFunction(fragment.get());
    auto* color = d->colorAttachments()->object(0);
    color->setPixelFormat(metalFormat(desc.format));
    setBlending(color, desc.blending);

    NS::Error* e = nullptr;
    RenderPipeline p;
    if (MTL::BinaryArchive* archive = cache.native()) {
        d->setBinaryArchives(NS::Array::array(archive));
        p.handle_ =
            NS::TransferPtr(handle_->newRenderPipelineState(d.get(), MTL::PipelineOptionFailOnBinaryArchiveMiss,
                                                            nullptr, &e));
        if (cached)
            *cached = bool(p);
        if (p)
            return p;
        e = nullptr;
        archive->addRenderPipelineFunctions(d.get(), &e);
        e = nullptr;
    }
    p.handle_ = NS::TransferPtr(handle_->newRenderPipelineState(d.get(), &e));
    if (!p)
        setError(error, "pipeline creation failed", e);
    return p;
}

ComputePipeline Device::makeComputePipeline(const ShaderModule& module, std::string_view function,
                                            std::string* error) const
{
    auto f = NS::TransferPtr(module.native()->newFunction(str(std::string(function).c_str())));
    NS::Error* e = nullptr;
    ComputePipeline p;
    if (f)
        p.handle_ = NS::TransferPtr(handle_->newComputePipelineState(f.get(), &e));
    if (!p)
        setError(error, "compute pipeline creation failed", e);
    return p;
}

PipelineCache Device::openPipelineCache(const std::string& path) const
{
    Scope scope;
    auto desc = NS::TransferPtr(MTL::BinaryArchiveDescriptor::alloc()->init());
    if (!path.empty() && access(path.c_str(), R_OK) == 0)
        desc->setUrl(NS::URL::fileURLWithPath(str(path.c_str())));
    NS::Error* error = nullptr;
    PipelineCache cache;
    cache.handle_ = NS::TransferPtr(handle_->newBinaryArchive(desc.get(), &error));
    if (!cache && desc->url()) {
        desc->setUrl(nullptr);
        cache.handle_ = NS::TransferPtr(handle_->newBinaryArchive(desc.get(), &error));
    }
    return cache;
}

bool Device::savePipelineCache(const PipelineCache& cache, const std::string& path, std::string* error) const
{
    Scope scope;
    NS::Error* e = nullptr;
    if (cache.native()->serializeToURL(NS::URL::fileURLWithPath(str(path.c_str())), &e))
        return true;
    setError(error, "pipeline cache not written", e);
    return false;
}

bool TimestampQueries::resolve(uint32_t first, std::span<uint64_t> ticks) const
{
    const NS::UInteger count = ticks.size();
    NS::Data* data = count ? handle_->resolveCounterRange(NS::Range(first, count)) : nullptr;
    if (!responds(data, "bytes") || !responds(data, "length") ||
        data->length() < count * sizeof(MTL::CounterResultTimestamp))
        return false;
    const auto* stamps = static_cast<const MTL::CounterResultTimestamp*>(bytes(data));
    for (size_t i = 0; i < count; ++i)
        ticks[i] = stamps[i].timestamp == MTL::CounterErrorValue ? 0 : stamps[i].timestamp;
    return true;
}

RenderPassSetup CommandBuffer::setup(const RenderTarget& target)
{
    auto* desc = MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* color = desc->colorAttachments()->object(0);
    color->setTexture(target.texture->native());
    color->setLoadAction(loadOf(target.load));
    color->setClearColor(MTL::ClearColor(target.clear.r, target.clear.g, target.clear.b, target.clear.a));
    color->setStoreAction(MTL::StoreActionStore);
    for (size_t slot = 0; slot < kTimestampSlots; ++slot) {
        const TimestampWrite& write = target.timestamps[slot];
        if (!write.queries)
            continue;
        auto* attachment = desc->sampleBufferAttachments()->object(slot);
        attachment->setSampleBuffer(write.queries->native());
        attachment->setStartOfVertexSampleIndex(write.start);
        attachment->setEndOfVertexSampleIndex(MTL::CounterDontSample);
        attachment->setStartOfFragmentSampleIndex(MTL::CounterDontSample);
        attachment->setEndOfFragmentSampleIndex(write.end);
    }
    return RenderPassSetup(desc);
}

void CommandBuffer::copy(const Texture& from, const Texture& to)
{
    MTL::BlitCommandEncoder* blit = cmd_->blitCommandEncoder();
    blit->copyFromTexture(from.native(), to.native());
    blit->endEncoding();
}

void CommandBuffer::copy(const Texture& from, const Buffer& to, size_t rowBytes, size_t offset, uint32_t level)
{
    MTL::Texture* t = from.native();
    const auto width = std::max<NS::UInteger>(1, t->width() >> level);
    const auto height = std::max<NS::UInteger>(1, t->height() >> level);
    MTL::BlitCommandEncoder* blit = cmd_->blitCommandEncoder();
    blit->copyFromTexture(t, 0, level, MTL::Origin(0, 0, 0), MTL::Size(width, height, 1), to.native(), offset,
                          rowBytes, rowBytes * rowCount(from.format(), uint32_t(height)));
    blit->endEncoding();
}

void CommandBuffer::synchronize(const Texture& texture)
{
    if (texture.native()->storageMode() != MTL::StorageModeManaged)
        return;
    MTL::BlitCommandEncoder* blit = cmd_->blitCommandEncoder();
    blit->synchronizeResource(texture.native());
    blit->endEncoding();
}

} // namespace azl::gpu
