#pragma once

#include "gpu/gpu.hpp"

namespace azl {

// AMD FidelityFX Super Resolution 1 on premultiplied pictures: EASU scales a picture up, RCAS sharpens the result.
class FsrUpscaler {
public:
    // Compiles its pipelines: never on the render thread.
    FsrUpscaler(const gpu::Device& device, gpu::Format format);

    // Into a render pass whose target is the output size, from the smaller `input`.
    void encodeEasu(gpu::RenderPass& pass, const gpu::Texture& input, const gpu::Texture& target) const;
    // Into a render pass whose target is the size of `input`.
    void encodeRcas(gpu::RenderPass& pass, const gpu::Texture& input) const;

    // In stops below the strongest sharpening, as FsrRcasCon takes it.
    static constexpr float kSharpness = 0.25f;

private:
    gpu::RenderPipeline easu_;
    gpu::RenderPipeline rcas_;
};

} // namespace azl
