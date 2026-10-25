#include "engine/metal_util.hpp"

#include <stdexcept>
#include <string>

namespace azl {

namespace {

NS::String* str(const char* s)
{
    return NS::String::string(s, NS::UTF8StringEncoding);
}

[[noreturn]] void fail(const char* what, NS::Error* error)
{
    throw std::runtime_error(std::string(what) + ": " + error->localizedDescription()->utf8String());
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

} // namespace

NS::SharedPtr<MTL::Library> makeLibrary(MTL::Device* device, const char* source)
{
    NS::Error* error = nullptr;
    auto library = NS::TransferPtr(device->newLibrary(str(source), nullptr, &error));
    if (!library)
        fail("shader compile failed", error);
    return library;
}

NS::SharedPtr<MTL::RenderPipelineState> makePipeline(MTL::Device* device, MTL::Library* library, const char* vertexName,
                                                     const char* fragmentName, MTL::PixelFormat format,
                                                     Blending blending, MTL::BinaryArchive* archive, bool* archived)
{
    auto vertex = NS::TransferPtr(library->newFunction(str(vertexName)));
    auto fragment = NS::TransferPtr(library->newFunction(str(fragmentName)));

    auto desc = NS::TransferPtr(MTL::RenderPipelineDescriptor::alloc()->init());
    desc->setVertexFunction(vertex.get());
    desc->setFragmentFunction(fragment.get());
    auto* color = desc->colorAttachments()->object(0);
    color->setPixelFormat(format);
    setBlending(color, blending);

    NS::Error* error = nullptr;
    if (archive) {
        desc->setBinaryArchives(NS::Array::array(archive));
        auto cached = NS::TransferPtr(
            device->newRenderPipelineState(desc.get(), MTL::PipelineOptionFailOnBinaryArchiveMiss, nullptr, &error));
        if (archived)
            *archived = bool(cached);
        if (cached)
            return cached;
        error = nullptr;
        archive->addRenderPipelineFunctions(desc.get(), &error);
        error = nullptr;
    }
    auto pipeline = NS::TransferPtr(device->newRenderPipelineState(desc.get(), &error));
    if (!pipeline)
        fail("pipeline creation failed", error);
    return pipeline;
}

bool archivesWork(std::string_view deviceName)
{
    return deviceName.find("Paravirtual") == std::string_view::npos;
}

NS::SharedPtr<MTL::RenderPipelineState> makePipeline(MTL::Device* device, const char* source, const char* vertexName,
                                                     const char* fragmentName, MTL::PixelFormat format)
{
    return makePipeline(device, makeLibrary(device, source).get(), vertexName, fragmentName, format);
}

} // namespace azl
