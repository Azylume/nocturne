#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace azl {

// How an encoder takes its Y'CbCr pictures from system memory. Every layout is BT.709, video range.
enum class YCbCrLayout : uint8_t {
    // 8 bits 4:2:0: a luma plane of bytes, then interleaved CbCr at half width and height (CoreVideo's '420v').
    NV12,
    // The same layout with 16-bit samples holding 10 bits in their high bits (CoreVideo's 'x420').
    P010,
    // 10 bits 4:2:2 in three planes of 16-bit little-endian samples holding 10 bits in their low bits: luma, then Cb
    // and Cr at half width (FFmpeg's yuv422p10le, ProRes 422's input).
    Planar422P10,
};

// One picture an encoder takes as it is, in memory the encoder owns. Whoever fills it (the GPU side's Y'CbCr pass and
// readback, or a test) writes `height` rows of each plane, CbCr planes at their subsampled height, and never more than
// `strides` bytes apart.
struct YCbCrPicture {
    YCbCrLayout layout = YCbCrLayout::NV12;
    uint32_t width = 0;
    uint32_t height = 0;
    int planeCount = 0;
    std::array<uint8_t*, 3> planes{};
    std::array<size_t, 3> strides{};
};

} // namespace azl
