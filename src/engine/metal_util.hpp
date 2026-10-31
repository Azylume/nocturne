#pragma once

#include "gpu/gpu.hpp"

#include <string_view>

namespace azl {

using Blending = gpu::Blending;

// Shaders compile from source at start-up, so the build needs no Xcode `metal` tool.
NS::SharedPtr<MTL::Library> makeLibrary(MTL::Device* device, const char* source);
// With an archive, a pipeline it holds builds without compiling and sets `archived`; one it lacks is compiled into it.
NS::SharedPtr<MTL::RenderPipelineState> makePipeline(MTL::Device* device, MTL::Library* library, const char* vertexName,
                                                     const char* fragmentName, MTL::PixelFormat format,
                                                     Blending blending = Blending::None,
                                                     MTL::BinaryArchive* archive = nullptr, bool* archived = nullptr);
// Whether the device can keep a pipeline cache. Apple's Paravirtual device (a macOS VM, as on CI runners) traps inside
// Metal's compiler adding to an archive after an engine on it was killed (issue #29), so there it runs without one.
bool archivesWork(std::string_view deviceName);
NS::SharedPtr<MTL::RenderPipelineState> makePipeline(MTL::Device* device, const char* source, const char* vertexName,
                                                     const char* fragmentName, MTL::PixelFormat format);

} // namespace azl
