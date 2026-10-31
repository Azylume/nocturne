#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace azl {

enum class YuvMatrix { Rec601, Rec709, Rec2020 };

// Rows of the affine Y'CbCr -> R'G'B' transform for a matrix and range: rgb[i] = dot(row.xyz, yuv) + w.
std::array<float, 12> yuvToRgbRows(YuvMatrix matrix, bool fullRange);

// How one plane's texels are stored, which is also the texture format a GPU backend uploads it into.
enum class PlaneFormat : uint8_t {
    // NV12: 8-bit luma, then interleaved CbCr at half width and height.
    R8,
    RG8,
    // P010: the same layout with 16-bit texels holding 10 bits in their high bits.
    R16,
    RG16,
    BGRA8,
    // A HAP frame's compressed textures: rows of 4x4 blocks.
    BC1,
    BC3,
    BC4,
};

struct PicturePlane {
    PlaneFormat format = PlaneFormat::R8;
    // In texels; for the BC formats in pixels.
    int width = 0;
    int height = 0;
    // Bytes from one row of texels (BC: of blocks) to the next.
    size_t stride = 0;
    const uint8_t* data = nullptr;
};

// One decoded picture in system memory, as a decoder hands it to a GPU backend to upload: NV12 or P010 planes of
// Y'CbCr video in the stream's own range, BGRA, or a HAP frame's compressed textures. `storage` owns the bytes the
// planes point into, so the picture is valid as long as anyone holds it. Immutable once made: any thread may read it.
struct DecodedPicture {
    std::array<PicturePlane, 2> planes{};
    int planeCount = 0;
    // Presentation time in seconds from the start of the track.
    double seconds = 0.0;
    YuvMatrix matrix = YuvMatrix::Rec709;
    bool fullRange = false;
    std::shared_ptr<const void> storage;
};

} // namespace azl
