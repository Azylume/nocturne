#pragma once

#include "gpu/gpu.hpp"

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
}

#include <array>
#include <memory>

namespace azl {

// Hardware video decode whose pictures reach the GPU without a trip through system memory. On Windows D3D11VA decodes
// into D3D11 textures and the Vulkan device copies out of them directly; the D3D11 device is shared by every source.
// One per source.
class GpuDecode {
public:
    // Null when this machine cannot, or NOCTURNE_SOFTWARE_DECODE is set: the caller decodes on the CPU. Any thread.
    static std::unique_ptr<GpuDecode> open(const gpu::Device& device);
    ~GpuDecode();

    // The D3D11VA device a decoder opens with. Any thread.
    AVBufferRef* device() const;

    // Whether textures() takes this decoded frame: a hardware frame of 8-bit 4:2:0.
    bool takes(const AVFrame& frame) const;
    // The frame's luma (R8) and chroma (RG8) as textures; null ones when it cannot. Waits for the GPU to copy the
    // picture and for nothing else. One thread at a time (the source's decode thread), never the render thread.
    std::array<gpu::Texture, 2> textures(const AVFrame& frame);

private:
    struct Impl;
    explicit GpuDecode(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace azl
