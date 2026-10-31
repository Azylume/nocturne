#pragma once

// The Metal backend's inline methods: each one the metal-cpp call the code made before the interface.

#include <utility>

namespace azl::gpu {

constexpr MTL::PixelFormat metalFormat(Format format)
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

constexpr Format fromMetal(MTL::PixelFormat format)
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

inline Texture::operator bool() const
{
    return bool(handle_);
}
inline uint32_t Texture::width() const
{
    return uint32_t(handle_->width());
}
inline uint32_t Texture::height() const
{
    return uint32_t(handle_->height());
}
inline Format Texture::format() const
{
    return fromMetal(handle_->pixelFormat());
}
inline uint32_t Texture::mipLevels() const
{
    return uint32_t(handle_->mipmapLevelCount());
}
inline size_t Texture::allocatedBytes() const
{
    return handle_->allocatedSize();
}
inline NativeTexture Texture::native() const
{
    return handle_.get();
}
inline NativeTexture native(const Texture* texture)
{
    return texture ? texture->native() : nullptr;
}
inline Texture Texture::retain(NativeTexture texture)
{
    Texture t;
    t.handle_ = NS::RetainPtr(texture);
    return t;
}
inline Texture Texture::adopt(NativeTexture texture)
{
    Texture t;
    t.handle_ = NS::TransferPtr(texture);
    return t;
}

inline Buffer::operator bool() const
{
    return bool(handle_);
}
inline size_t Buffer::size() const
{
    return handle_->length();
}
inline void* Buffer::contents() const
{
    return handle_->contents();
}
inline NativeBuffer Buffer::native() const
{
    return handle_.get();
}
inline Buffer Buffer::adopt(NativeBuffer buffer)
{
    Buffer b;
    b.handle_ = NS::TransferPtr(buffer);
    return b;
}

inline ShaderModule::operator bool() const
{
    return bool(handle_);
}
inline NativeShaderModule ShaderModule::native() const
{
    return handle_.get();
}
inline ShaderModule ShaderModule::retain(NativeShaderModule module)
{
    ShaderModule m;
    m.handle_ = NS::RetainPtr(module);
    return m;
}

inline PipelineCache::operator bool() const
{
    return bool(handle_);
}
inline NativePipelineCache PipelineCache::native() const
{
    return handle_.get();
}
inline PipelineCache PipelineCache::retain(NativePipelineCache cache)
{
    PipelineCache c;
    c.handle_ = NS::RetainPtr(cache);
    return c;
}

inline RenderPipeline::operator bool() const
{
    return bool(handle_);
}
inline NativeRenderPipeline RenderPipeline::native() const
{
    return handle_.get();
}
inline RenderPipeline RenderPipeline::retain(NativeRenderPipeline pipeline)
{
    RenderPipeline p;
    p.handle_ = NS::RetainPtr(pipeline);
    return p;
}

inline ComputePipeline::operator bool() const
{
    return bool(handle_);
}
inline uint32_t ComputePipeline::maxThreads() const
{
    return uint32_t(handle_->maxTotalThreadsPerThreadgroup());
}
inline NativeComputePipeline ComputePipeline::native() const
{
    return handle_.get();
}
inline ComputePipeline ComputePipeline::retain(NativeComputePipeline pipeline)
{
    ComputePipeline p;
    p.handle_ = NS::RetainPtr(pipeline);
    return p;
}

inline Fence::operator bool() const
{
    return bool(handle_);
}
inline uint64_t Fence::value() const
{
    return handle_->signaledValue();
}
inline bool Fence::wait(uint64_t value, uint64_t timeoutMs) const
{
    return handle_->waitUntilSignaledValue(value, timeoutMs);
}
inline NativeFence Fence::native() const
{
    return handle_.get();
}

inline TimestampQueries::operator bool() const
{
    return bool(handle_);
}
inline uint32_t TimestampQueries::count() const
{
    return uint32_t(handle_->sampleCount());
}
inline NativeTimestampQueries TimestampQueries::native() const
{
    return handle_.get();
}

inline RenderPass::operator bool() const
{
    return pass_ != nullptr;
}
inline void RenderPass::setPipeline(const RenderPipeline& pipeline)
{
    pass_->setRenderPipelineState(pipeline.native());
}
inline void RenderPass::setTexture(uint32_t slot, const Texture& texture)
{
    pass_->setFragmentTexture(texture.native(), slot);
}
inline void RenderPass::setTexture(uint32_t slot, const Texture* texture)
{
    pass_->setFragmentTexture(gpu::native(texture), slot);
}
inline void RenderPass::setBytes(uint32_t index, const void* data, size_t size)
{
    pass_->setFragmentBytes(data, size, index);
}
inline void RenderPass::setVertexBytes(uint32_t index, const void* data, size_t size)
{
    pass_->setVertexBytes(data, size, index);
}
inline void RenderPass::setBuffer(uint32_t index, const Buffer& buffer, size_t offset)
{
    pass_->setFragmentBuffer(buffer.native(), offset, index);
}
inline void RenderPass::setVertexBuffer(uint32_t index, const Buffer& buffer, size_t offset)
{
    pass_->setVertexBuffer(buffer.native(), offset, index);
}
inline void RenderPass::setScissor(const Scissor& s)
{
    pass_->setScissorRect(MTL::ScissorRect{s.x, s.y, s.width, s.height});
}
inline void RenderPass::draw(uint32_t vertices, uint32_t instances, uint32_t firstInstance)
{
    if (instances == 1 && firstInstance == 0)
        pass_->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(vertices));
    else
        pass_->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(vertices),
                              NS::UInteger(instances), NS::UInteger(firstInstance));
}
inline void RenderPass::drawIndexed(const Buffer& indices, uint32_t count, size_t offset, uint32_t instances,
                                    uint32_t firstInstance)
{
    pass_->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(count), MTL::IndexTypeUInt32,
                                 indices.native(), NS::UInteger(offset), NS::UInteger(instances), NS::Integer(0),
                                 NS::UInteger(firstInstance));
}
inline void RenderPass::writeTimestamp(const TimestampQueries& queries, uint32_t index)
{
    pass_->sampleCountersInBuffer(queries.native(), index, true);
}
inline void RenderPass::end()
{
    pass_->endEncoding();
}

inline ComputePass::operator bool() const
{
    return pass_ != nullptr;
}
inline void ComputePass::setPipeline(const ComputePipeline& pipeline)
{
    pass_->setComputePipelineState(pipeline.native());
}
inline void ComputePass::setTexture(uint32_t slot, const Texture& texture)
{
    pass_->setTexture(texture.native(), slot);
}
inline void ComputePass::setBytes(uint32_t index, const void* data, size_t size)
{
    pass_->setBytes(data, size, index);
}
inline void ComputePass::dispatch(uint32_t width, uint32_t height, uint32_t groupWidth, uint32_t groupHeight)
{
    pass_->dispatchThreads(MTL::Size(width, height, 1), MTL::Size(groupWidth, groupHeight, 1));
}
inline void ComputePass::dispatchGroups(uint32_t groupsX, uint32_t groupsY, uint32_t groupWidth, uint32_t groupHeight)
{
    pass_->dispatchThreadgroups(MTL::Size(groupsX, groupsY, 1), MTL::Size(groupWidth, groupHeight, 1));
}
inline void ComputePass::end()
{
    pass_->endEncoding();
}

inline CommandBuffer::operator bool() const
{
    return cmd_ != nullptr;
}
inline RenderPass CommandBuffer::begin(const RenderPassSetup& setup)
{
    return RenderPass(cmd_->renderCommandEncoder(setup.native()));
}
inline ComputePass CommandBuffer::beginCompute()
{
    return ComputePass(cmd_->computeCommandEncoder());
}
inline void CommandBuffer::transition(const Texture&, Layout)
{
}
inline void CommandBuffer::signal(const Fence& fence, uint64_t value)
{
    cmd_->encodeSignalEvent(fence.native(), value);
}
inline void CommandBuffer::wait(const Fence& fence, uint64_t value)
{
    cmd_->encodeWait(fence.native(), value);
}
inline Completion completionOf(MTL::CommandBuffer* done)
{
    return {done->status() == MTL::CommandBufferStatusCompleted, done->GPUStartTime(), done->GPUEndTime()};
}
template <class F>
inline void CommandBuffer::onCompleted(F&& handler)
{
    cmd_->addCompletedHandler(
        [handler = std::forward<F>(handler)](MTL::CommandBuffer* done) mutable { handler(completionOf(done)); });
}
inline void CommandBuffer::commit()
{
    cmd_->commit();
}
inline void CommandBuffer::waitUntilCompleted()
{
    cmd_->waitUntilCompleted();
}
inline Completion CommandBuffer::completion() const
{
    return completionOf(cmd_);
}

inline Queue::operator bool() const
{
    return bool(handle_);
}
inline CommandBuffer Queue::commandBuffer() const
{
    return CommandBuffer(handle_->commandBuffer());
}
inline NativeQueue Queue::native() const
{
    return handle_.get();
}

inline Scope::Scope() : handle_(NS::AutoreleasePool::alloc()->init())
{
}
inline Scope::~Scope()
{
    handle_->release();
}

inline Device Device::retain(NativeDevice device)
{
    Device d;
    d.handle_ = NS::RetainPtr(device);
    return d;
}
inline Device::operator bool() const
{
    return bool(handle_);
}
inline bool Device::unifiedMemory() const
{
    return handle_->hasUnifiedMemory();
}
inline NativeDevice Device::native() const
{
    return handle_.get();
}

} // namespace azl::gpu
