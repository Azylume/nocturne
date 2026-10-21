#pragma once

#include <Metal/Metal.hpp>

namespace azl {

// Fixed-function blending of a premultiplied colour into the target.
enum class Blending { None, Normal, Add, Screen, Multiply };

// Shaders compile from source at start-up, so the build needs no Xcode `metal` tool.
NS::SharedPtr<MTL::Library> makeLibrary(MTL::Device* device, const char* source);
// With an archive, a pipeline it holds builds without compiling and sets `archived`; one it lacks is compiled into it.
NS::SharedPtr<MTL::RenderPipelineState> makePipeline(MTL::Device* device, MTL::Library* library, const char* vertexName,
                                                     const char* fragmentName, MTL::PixelFormat format,
                                                     Blending blending = Blending::None,
                                                     MTL::BinaryArchive* archive = nullptr, bool* archived = nullptr);
NS::SharedPtr<MTL::RenderPipelineState> makePipeline(MTL::Device* device, const char* source, const char* vertexName,
                                                     const char* fragmentName, MTL::PixelFormat format);

} // namespace azl
