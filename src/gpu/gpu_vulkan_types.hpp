#pragma once

// The Vulkan backend's handles: reference-counted backend objects (gpu_vulkan_objects.hpp) for the owning classes,
// raw pointers for the views. A render or compute pass is recorded into its command buffer, so its handle is the
// command buffer's.

#include <memory>

namespace azl::gpu {

namespace vk {
struct DeviceObject;
struct QueueObject;
struct TextureObject;
struct BufferObject;
struct ShaderModuleObject;
struct PipelineCacheObject;
struct PipelineObject;
struct FenceObject;
struct QueryObject;
struct CommandObject;
} // namespace vk

using NativeDevice = vk::DeviceObject*;
using NativeQueue = vk::QueueObject*;
using NativeTexture = vk::TextureObject*;
using NativeBuffer = vk::BufferObject*;
using NativeShaderModule = vk::ShaderModuleObject*;
using NativePipelineCache = vk::PipelineCacheObject*;
using NativeRenderPipeline = vk::PipelineObject*;
using NativeComputePipeline = vk::PipelineObject*;
using NativeFence = vk::FenceObject*;
using NativeTimestampQueries = vk::QueryObject*;
using NativeCommandBuffer = vk::CommandObject*;
using NativeRenderPassSetup = vk::CommandObject*;
using NativeRenderPass = vk::CommandObject*;
using NativeComputePass = vk::CommandObject*;

using DeviceHandle = std::shared_ptr<vk::DeviceObject>;
using QueueHandle = std::shared_ptr<vk::QueueObject>;
using TextureHandle = std::shared_ptr<vk::TextureObject>;
using BufferHandle = std::shared_ptr<vk::BufferObject>;
using ShaderModuleHandle = std::shared_ptr<vk::ShaderModuleObject>;
using PipelineCacheHandle = std::shared_ptr<vk::PipelineCacheObject>;
using RenderPipelineHandle = std::shared_ptr<vk::PipelineObject>;
using ComputePipelineHandle = std::shared_ptr<vk::PipelineObject>;
using FenceHandle = std::shared_ptr<vk::FenceObject>;
using TimestampQueriesHandle = std::shared_ptr<vk::QueryObject>;
// How many command buffers the thread's scopes held when this one opened.
using ScopeHandle = size_t;

} // namespace azl::gpu
