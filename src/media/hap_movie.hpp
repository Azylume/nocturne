#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace azl {

// The HAP variants the engine plays, by the four-character code of their sample description.
enum class HapCodec { None, Hap, HapAlpha, HapQ, HapQAlpha };

HapCodec hapCodecOf(uint32_t fourcc);
uint32_t fourccOf(HapCodec codec);

// Writes HAP frames as they are into a QuickTime movie, one sync sample per frame. Throws std::runtime_error.
class HapMovieWriter {
public:
    HapMovieWriter(const std::filesystem::path& file, HapCodec codec, int width, int height, int fpsNum, int fpsDen);
    ~HapMovieWriter();
    void append(const std::vector<uint8_t>& frame);
    void finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The HAP codec of the file's video track; None for any other file.
HapCodec hapCodecOfFile(const std::filesystem::path& file);

// The frame `fraction` (0..1) of the way into a HAP movie as straight-alpha RGBA, decoded on the CPU, with its size;
// empty when the file is not HAP or the frame does not decode.
std::vector<uint8_t> readHapPicture(const std::filesystem::path& file, double fraction, int& width, int& height);

// The first `count` frames of a HAP movie's video track, as stored. Throws std::runtime_error.
std::vector<std::vector<uint8_t>> readHapFrames(const std::filesystem::path& file, int count);

} // namespace azl
