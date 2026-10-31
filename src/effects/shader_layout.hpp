#pragma once

#include <cstdint>
#include <string>

namespace azl {

// Where every GLSL shader finds its resources: uniform and storage blocks in set kBufferSet at their buffer index,
// textures and images in set kTextureSet at their texture index, samplers in set kSamplerSet by SamplerKind. A
// backend binds buffers and textures at those indices and creates one fixed sampler per kind.
inline constexpr uint32_t kBufferSet = 0;
inline constexpr uint32_t kTextureSet = 1;
inline constexpr uint32_t kSamplerSet = 2;

// The binding of each sampler in kSamplerSet, named in kGlslSamplers.
enum class SamplerKind : uint32_t {
    // `lin`: linear, clamp to edge, normalised coordinates.
    Linear,
    // `pix`: linear, clamp to edge, pixel coordinates; read only with an explicit level 0 (samplePixel).
    Pixel,
    // `textSampler`: linear between and within mip levels, clamp to zero, normalised coordinates.
    Text,
};
inline constexpr uint32_t kSamplerKinds = 3;

// GLSL declarations of every SamplerKind.
extern const char* const kGlslSamplers;

// A vertex shader drawing one triangle over the whole target from three vertices and no buffers.
extern const char* const kFullscreenVertex;

// A shader holding several entry points, each a main inside `#ifdef ENTRY_<name>`, as the shader of one of them: its
// first line (#version) followed by the define.
std::string withEntry(const std::string& glsl, const std::string& entry);

} // namespace azl
