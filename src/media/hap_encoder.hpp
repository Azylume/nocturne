#pragma once

#include "media/hap_movie.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace azl {

// "hap", "hap-alpha", "hap-q" or "hap-q-alpha"; None for anything else.
HapCodec hapCodecNamed(const std::string& name);

// Encodes raw straight-alpha RGBA frames, fed in pieces of any size, into a HAP movie: the transcode path.
class HapEncoder {
public:
    HapEncoder(const std::filesystem::path& file, HapCodec codec, int width, int height, int fpsNum, int fpsDen,
               int chunks = 1);
    void feed(const uint8_t* data, size_t size);
    // Throws std::runtime_error when no frame came, or a partial frame is left over.
    void finish();
    int frames() const { return frames_; }

private:
    HapMovieWriter writer_;
    HapCodec codec_;
    int width_;
    int height_;
    int chunks_;
    std::vector<uint8_t> pending_;
    int frames_ = 0;
};

} // namespace azl
