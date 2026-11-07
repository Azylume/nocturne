#pragma once

#include "gpu/gpu.hpp"

#include <array>
#include <memory>

extern "C" {
#include <libavutil/hwcontext.h>
}

namespace azl {

// FFmpeg's Vulkan Video decoder on the renderer's own VkDevice, so a decoded picture is already a VkImage the renderer
// can copy from: nothing visits system memory. One per device, shared by every clip, because FFmpeg locks the decode
// queues it was given on its own and two contexts over the same queues would submit to them unsynchronised.
class GpuDecode {
public:
    // Null when NOCTURNE_SOFTWARE_DECODE is set, the device was made without Vulkan Video decode or FFmpeg cannot use
    // it. Logs which, once. Any thread.
    static std::shared_ptr<GpuDecode> open(const gpu::Device& device);

    ~GpuDecode();
    GpuDecode(const GpuDecode&) = delete;
    GpuDecode& operator=(const GpuDecode&) = delete;

    // The hardware device context a decoder is opened with.
    AVBufferRef* device() const { return device_; }
    // Whether `textures` can take this decoded frame as it is: an 8-bit NV12 picture of exactly width x height (both
    // even) that no other queue family owns. A frame it refuses comes back through av_hwframe_transfer_data. Any
    // thread.
    bool takes(const AVFrame& frame, int width, int height) const;
    // The frame's luma (R8) and chroma (RG8) as textures of their own, copied on the GPU, waiting for the copy; null
    // on failure. Decode thread, never the render thread.
    std::array<gpu::Texture, 2> textures(const AVFrame& frame) const;

private:
    GpuDecode(const gpu::Device& device, AVBufferRef* context) : gpu_(device), device_(context) {}

    gpu::Device gpu_;
    AVBufferRef* device_ = nullptr;
};

} // namespace azl
