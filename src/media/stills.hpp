#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace azl {

// JPEG of premultiplied BGRA pixels (Metal's BGRA8Unorm layout). Empty when encoding fails.
std::vector<uint8_t> encodeJpeg(const uint8_t* bgra, uint32_t width, uint32_t height, size_t bytesPerRow,
                                double quality = 0.7);

// A JPEG still of the video a third of the way in, at most `width` pixels wide. Throws std::runtime_error when the file
// has no frame to show.
std::vector<uint8_t> videoStill(const std::filesystem::path& file, uint32_t width);

// Pictures of a video at `count` evenly spaced points, each taken at the keyframe nearest that point when one is within
// half a spacing or half a second, so a picture costs about one frame's decode. Each fills `width` x `height` (cropped to its aspect)
// as premultiplied BGRA. Blocking: never call it on the render thread.
class VideoSampler {
public:
    // Throws std::runtime_error when the file has no video.
    VideoSampler(const std::filesystem::path& file, uint32_t width, uint32_t height, int count);
    ~VideoSampler();
    // Picture `i` of `count`; empty when that point has no frame.
    std::vector<uint8_t> picture(int i);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace azl
