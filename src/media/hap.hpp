#pragma once

#include "media/hap_movie.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace azl {

// The compressed texture formats a HAP frame holds (the low nibble of its section type).
enum class HapTexture : uint8_t { BC1 = 0x0b, BC3 = 0x0e, YCoCgBC3 = 0x0f, BC4 = 0x01 };

struct HapImage {
    HapTexture texture = HapTexture::BC1;
    std::vector<uint8_t> blocks;
};

// The largest side of a picture a movie may declare: Metal's texture limit.
inline constexpr int kMaxPictureSide = 16384;
bool pictureSizeOk(int width, int height);

// Bytes of a `texture` of that size: 4x4 blocks, the last row and column padded.
size_t hapTextureBytes(HapTexture texture, int width, int height);

// The frame's compressed textures, second-stage decompressed (Snappy, chunked or not): one, or for HAP Q Alpha the
// colour and the alpha. Returns what is wrong with a malformed frame, else an empty string.
std::string decodeHapFrame(const uint8_t* data, size_t size, int width, int height, std::vector<HapImage>& images);

// One HAP frame of `codec` from straight-alpha RGBA pixels, its texture data split into `chunks` Snappy chunks.
std::vector<uint8_t> encodeHapFrame(const uint8_t* rgba, int width, int height, HapCodec codec, int chunks = 1);

// The pictures of a decoded frame as straight-alpha RGBA, for stills and tests (playback decodes on the GPU).
std::vector<uint8_t> hapToRgba(const std::vector<HapImage>& images, int width, int height);

} // namespace azl
