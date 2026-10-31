#pragma once

#include "gpu/gpu.hpp"
#include "output/ycbcr_picture.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace azl {

// How the samples of a Y'CbCr picture lie in its planes. Every packing is BT.709, video range.
enum class YCbCrPacking : uint8_t {
    // A luma plane, then interleaved CbCr at half width and height: '420v', 'x420', NV12, P010.
    BiPlanar420,
    // One plane of Cb Y0 Cr Y1 per pixel pair: '2vuy', 'v216'.
    Pairs422,
    // Three planes, luma then Cb and Cr at half width: yuv422p10le.
    Planar422,
};

struct YCbCrPlane {
    gpu::Format format;
    // Picture pixels per texel across and down.
    uint32_t across;
    uint32_t down;
};

struct YCbCrFormat {
    YCbCrPacking packing;
    std::vector<YCbCrPlane> planes;
    // The largest code, and the texel value of one code.
    float top;
    float step;
};

// How a conversion makes its samples: how each chroma sample mixes its block's pixels (top-left, top-right,
// bottom-left, bottom-right; a 4:2:2 pair uses the first two), and the format's codes per 8-bit code.
struct YCbCrCalibration {
    std::array<float, 4> weights;
    float steps;
};

// The block average, and 8-bit codes scaled by 4 into 10 bits.
YCbCrCalibration averageCalibration(const YCbCrFormat& format);

// The planes an encoder's picture layout takes from the pass.
YCbCrFormat ycbcrFormat(YCbCrLayout layout);
// Copies planes the pass wrote, read back in `format`'s order, into `picture` of the same size. Not on the render
// thread.
void copyPlanes(const YCbCrFormat& format, std::span<const gpu::Readback> planes, YCbCrPicture& picture);

// One compute pass that reads a BGRA picture once and writes its Y'CbCr planes. The planes are storage textures of
// `format.planes`, each the picture's size divided by the plane's `across` and `down`, rounded up; samples past the
// picture's edge read transparent black.
class YCbCrPass {
public:
    // Not on the render thread: compiles its pipeline. Throws std::runtime_error when it cannot.
    YCbCrPass(const gpu::Device& device, YCbCrFormat format, YCbCrCalibration calibration);

    const YCbCrFormat& format() const { return format_; }
    // Makes the storage textures of one picture `width` x `height`; empty when the device refuses one.
    std::vector<gpu::Texture> makePlanes(const gpu::Device& device, uint32_t width, uint32_t height) const;
    // Render thread: encodes the conversion of `source` into `planes`. Never waits.
    void encode(gpu::CommandBuffer& cmd, const gpu::Texture& source, std::span<const gpu::Texture> planes) const;

private:
    YCbCrFormat format_;
    gpu::ComputePipeline pipeline_;
    // The shader's: chroma weights, and the format's precision.
    struct alignas(16) Params {
        std::array<float, 4> weights;
        float steps;
        float step;
    } params_{};
};

} // namespace azl
