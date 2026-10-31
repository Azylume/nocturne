#pragma once

// The Vulkan backend's inline methods. Binding state is kept on the command buffer and pushed as one descriptor set
// at the next draw or dispatch, so a setTexture or setBytes costs a store (setBytes: and a copy).

#include "gpu/gpu_vulkan_objects.hpp"

#include <utility>

namespace azl::gpu {

inline Texture::operator bool() const
{
    return bool(handle_);
}
inline uint32_t Texture::width() const
{
    return handle_->width;
}
inline uint32_t Texture::height() const
{
    return handle_->height;
}
inline Format Texture::format() const
{
    return Format(handle_->format);
}
inline uint32_t Texture::mipLevels() const
{
    return handle_->mipLevels;
}
inline size_t Texture::allocatedBytes() const
{
    return handle_->bytes;
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
    if (texture)
        t.handle_ = texture->shared_from_this();
    return t;
}
inline Texture Texture::adopt(NativeTexture texture)
{
    Texture t;
    t.handle_ = TextureHandle(texture);
    return t;
}

inline Buffer::operator bool() const
{
    return bool(handle_);
}
inline size_t Buffer::size() const
{
    return handle_->size;
}
inline void* Buffer::contents() const
{
    return handle_->memory.mapped;
}
inline NativeBuffer Buffer::native() const
{
    return handle_.get();
}
inline Buffer Buffer::adopt(NativeBuffer buffer)
{
    Buffer b;
    b.handle_ = BufferHandle(buffer);
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
    if (module)
        m.handle_ = module->shared_from_this();
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
    if (cache)
        c.handle_ = cache->shared_from_this();
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
    if (pipeline)
        p.handle_ = pipeline->shared_from_this();
    return p;
}

inline ComputePipeline::operator bool() const
{
    return bool(handle_);
}
inline uint32_t ComputePipeline::maxThreads() const
{
    return vk::maxThreads(handle_.get());
}
inline NativeComputePipeline ComputePipeline::native() const
{
    return handle_.get();
}
inline ComputePipeline ComputePipeline::retain(NativeComputePipeline pipeline)
{
    ComputePipeline p;
    if (pipeline)
        p.handle_ = pipeline->shared_from_this();
    return p;
}

inline Fence::operator bool() const
{
    return bool(handle_);
}
inline uint64_t Fence::value() const
{
    return vk::fenceValue(handle_.get());
}
inline bool Fence::wait(uint64_t value, uint64_t timeoutMs) const
{
    return vk::waitFence(handle_.get(), value, timeoutMs);
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
    return handle_->count;
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
    vk::bindPipeline(pass_, pipeline.native());
}
inline void RenderPass::setTexture(uint32_t slot, const Texture& texture)
{
    vk::bindTexture(pass_, slot, texture.native());
}
inline void RenderPass::setTexture(uint32_t slot, const Texture* texture)
{
    if (texture)
        return vk::bindTexture(pass_, slot, texture->native());
    pass_->dirty = pass_->dirty || pass_->textures[slot];
    pass_->textures[slot] = nullptr;
}
inline void RenderPass::setBytes(uint32_t index, const void* data, size_t size)
{
    vk::pushBytes(pass_, index, data, size);
}
inline void RenderPass::setVertexBytes(uint32_t index, const void* data, size_t size)
{
    vk::pushBytes(pass_, vk::kVertexBuffers + index, data, size);
}
inline void RenderPass::setBuffer(uint32_t index, const Buffer& buffer, size_t offset)
{
    vk::bindBuffer(pass_, index, buffer.native(), offset);
}
inline void RenderPass::setVertexBuffer(uint32_t index, const Buffer& buffer, size_t offset)
{
    vk::bindBuffer(pass_, vk::kVertexBuffers + index, buffer.native(), offset);
}
inline void RenderPass::setScissor(const Scissor& s)
{
    vk::setScissor(pass_, s.x, s.y, s.width, s.height);
}
inline void RenderPass::draw(uint32_t vertices, uint32_t instances, uint32_t firstInstance)
{
    if (pass_->dirty)
        vk::flush(pass_);
    vkCmdDraw(pass_->cmd, vertices, instances, 0, firstInstance);
}
inline void RenderPass::drawIndexed(const Buffer& indices, uint32_t count, size_t offset, uint32_t instances,
                                    uint32_t firstInstance)
{
    vk::drawIndexed(pass_, indices.native(), count, offset, instances, firstInstance);
}
inline void RenderPass::writeTimestamp(const TimestampQueries& queries, uint32_t index)
{
    vk::writeTimestamp(pass_, queries.native(), index);
}
inline void RenderPass::end()
{
    vk::endRendering(pass_);
}

inline ComputePass::operator bool() const
{
    return pass_ != nullptr;
}
inline void ComputePass::setPipeline(const ComputePipeline& pipeline)
{
    vk::bindPipeline(pass_, pipeline.native());
}
inline void ComputePass::setTexture(uint32_t slot, const Texture& texture)
{
    vk::bindTexture(pass_, slot, texture.native());
}
inline void ComputePass::setBytes(uint32_t index, const void* data, size_t size)
{
    vk::pushBytes(pass_, index, data, size);
}
inline void ComputePass::dispatch(uint32_t width, uint32_t height, uint32_t, uint32_t)
{
    vk::dispatch(pass_, width, height);
}
inline void ComputePass::dispatchGroups(uint32_t groupsX, uint32_t groupsY, uint32_t, uint32_t)
{
    vk::dispatchGroups(pass_, groupsX, groupsY);
}
inline void ComputePass::end()
{
    vk::endCompute(pass_);
}

inline CommandBuffer::operator bool() const
{
    return cmd_ != nullptr;
}
inline ComputePass CommandBuffer::beginCompute()
{
    cmd_->pipeline = nullptr;
    cmd_->textures = {};
    cmd_->buffers = {};
    cmd_->dirty = true;
    cmd_->dispatched = false;
    return ComputePass(cmd_);
}
inline void CommandBuffer::transition(const Texture& texture, Layout layout)
{
    vk::transition(cmd_, texture.native(), uint8_t(layout));
}
inline void CommandBuffer::signal(const Fence& fence, uint64_t value)
{
    vk::signal(cmd_, fence.native(), value);
}
inline void CommandBuffer::wait(const Fence& fence, uint64_t value)
{
    vk::wait(cmd_, fence.native(), value);
}
template <class F>
inline void CommandBuffer::onCompleted(F&& handler)
{
    vk::addHandler(cmd_, std::function<void(const Completion&)>(std::forward<F>(handler)));
}
inline void CommandBuffer::commit()
{
    vk::commit(cmd_);
}
inline void CommandBuffer::waitUntilCompleted()
{
    vk::waitUntilCompleted(cmd_);
}
inline Completion CommandBuffer::completion() const
{
    return vk::completion(cmd_);
}

inline Queue::operator bool() const
{
    return bool(handle_);
}
inline CommandBuffer Queue::commandBuffer() const
{
    return CommandBuffer(vk::commandBuffer(handle_.get()));
}
inline NativeQueue Queue::native() const
{
    return handle_.get();
}

inline Scope::Scope() : handle_(vk::openScope())
{
}
inline Scope::~Scope()
{
    vk::closeScope(handle_);
}

inline Device::operator bool() const
{
    return bool(handle_);
}
inline bool Device::unifiedMemory() const
{
    return vk::unifiedMemory(handle_.get());
}
inline NativeDevice Device::native() const
{
    return handle_.get();
}

} // namespace azl::gpu
