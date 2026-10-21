#pragma once

#include <Metal/Metal.hpp>

namespace azl {

// AMD FidelityFX Super Resolution 1 on premultiplied pictures: EASU scales a picture up, RCAS sharpens the result.
class FsrUpscaler {
public:
    FsrUpscaler(MTL::Device* device, MTL::PixelFormat format);

    // Into a render pass whose target is the output size, from the smaller `input`.
    void encodeEasu(MTL::RenderCommandEncoder* enc, MTL::Texture* input, MTL::Texture* target) const;
    // Into a render pass whose target is the size of `input`.
    void encodeRcas(MTL::RenderCommandEncoder* enc, MTL::Texture* input) const;

    // In stops below the strongest sharpening, as FsrRcasCon takes it.
    static constexpr float kSharpness = 0.25f;

private:
    NS::SharedPtr<MTL::RenderPipelineState> easu_;
    NS::SharedPtr<MTL::RenderPipelineState> rcas_;
};

} // namespace azl
