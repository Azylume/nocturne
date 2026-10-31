#include "engine/metal_util.hpp"

#include <stdexcept>
#include <string>

namespace azl {

namespace {

[[noreturn]] void fail(const std::string& error)
{
    throw std::runtime_error(error);
}

} // namespace

NS::SharedPtr<MTL::Library> makeLibrary(MTL::Device* device, const char* source)
{
    std::string error;
    const gpu::ShaderModule module = gpu::Device::retain(device).makeShaderModule({.msl = source}, &error);
    if (!module)
        fail(error);
    return NS::RetainPtr(module.native());
}

NS::SharedPtr<MTL::RenderPipelineState> makePipeline(MTL::Device* device, MTL::Library* library, const char* vertexName,
                                                     const char* fragmentName, MTL::PixelFormat format,
                                                     Blending blending, MTL::BinaryArchive* archive, bool* archived)
{
    std::string error;
    const gpu::RenderPipeline pipeline = gpu::Device::retain(device).makeRenderPipeline(
        {gpu::ShaderModule::retain(library), vertexName, fragmentName, gpu::fromMetal(format), blending},
        archive ? gpu::PipelineCache::retain(archive) : gpu::PipelineCache{}, archived, &error);
    if (!pipeline)
        fail(error);
    return NS::RetainPtr(pipeline.native());
}

NS::SharedPtr<MTL::RenderPipelineState> makePipeline(MTL::Device* device, const char* source, const char* vertexName,
                                                     const char* fragmentName, MTL::PixelFormat format)
{
    return makePipeline(device, makeLibrary(device, source).get(), vertexName, fragmentName, format);
}

} // namespace azl
