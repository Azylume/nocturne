#pragma once

// The Metal backend's handles: metal-cpp objects, held the way the code before the interface held them.

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

namespace azl::gpu {

using NativeDevice = MTL::Device*;
using NativeQueue = MTL::CommandQueue*;
using NativeTexture = MTL::Texture*;
using NativeBuffer = MTL::Buffer*;
using NativeShaderModule = MTL::Library*;
using NativePipelineCache = MTL::BinaryArchive*;
using NativeRenderPipeline = MTL::RenderPipelineState*;
using NativeComputePipeline = MTL::ComputePipelineState*;
using NativeFence = MTL::SharedEvent*;
using NativeTimestampQueries = MTL::CounterSampleBuffer*;
using NativeCommandBuffer = MTL::CommandBuffer*;
using NativeRenderPassSetup = MTL::RenderPassDescriptor*;
using NativeRenderPass = MTL::RenderCommandEncoder*;
using NativeComputePass = MTL::ComputeCommandEncoder*;

using DeviceHandle = NS::SharedPtr<MTL::Device>;
using QueueHandle = NS::SharedPtr<MTL::CommandQueue>;
using TextureHandle = NS::SharedPtr<MTL::Texture>;
using BufferHandle = NS::SharedPtr<MTL::Buffer>;
using ShaderModuleHandle = NS::SharedPtr<MTL::Library>;
using PipelineCacheHandle = NS::SharedPtr<MTL::BinaryArchive>;
using RenderPipelineHandle = NS::SharedPtr<MTL::RenderPipelineState>;
using ComputePipelineHandle = NS::SharedPtr<MTL::ComputePipelineState>;
using FenceHandle = NS::SharedPtr<MTL::SharedEvent>;
using TimestampQueriesHandle = NS::SharedPtr<MTL::CounterSampleBuffer>;
using ScopeHandle = NS::AutoreleasePool*;

} // namespace azl::gpu
