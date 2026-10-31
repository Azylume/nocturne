#pragma once

#include "test_files.hpp"

#include "media/hap.hpp"

#include <vector>

// HAP versions of the count clips (tests/media/make_media.sh), written by the engine's own encoder: 320x180 at 30 fps,
// the frame index as 8 bits across the top half, RGB (200, 40, 40) bottom left and (40, 160, 80) bottom right. With
// `alpha`, the bottom right patch is half transparent (alpha 128).
namespace {

constexpr int kHapWidth = 320;
constexpr int kHapHeight = 180;

inline std::vector<uint8_t> countPicture(int frame, bool alpha)
{
    std::vector<uint8_t> rgba(size_t(kHapWidth) * kHapHeight * 4);
    for (int y = 0; y < kHapHeight; ++y)
        for (int x = 0; x < kHapWidth; ++x) {
            uint8_t* p = &rgba[(size_t(y) * kHapWidth + x) * 4];
            p[3] = 255;
            if (y < kHapHeight / 2) {
                p[0] = p[1] = p[2] = (frame >> (7 - x / 40)) & 1 ? 255 : 0;
            } else if (x < kHapWidth / 2) {
                p[0] = 200, p[1] = 40, p[2] = 40;
            } else {
                p[0] = 40, p[1] = 160, p[2] = 80;
                p[3] = alpha ? 128 : 255;
            }
        }
    return rgba;
}

inline bool hasAlpha(azl::HapCodec codec)
{
    return codec == azl::HapCodec::HapAlpha || codec == azl::HapCodec::HapQAlpha;
}

inline fs::path writeHapCount(const fs::path& file, azl::HapCodec codec, int frames = 60, int chunks = 1)
{
    fs::create_directories(file.parent_path());
    azl::HapMovieWriter writer(file, codec, kHapWidth, kHapHeight, 30, 1);
    for (int i = 0; i < frames; ++i)
        writer.append(azl::encodeHapFrame(countPicture(i, hasAlpha(codec)).data(), kHapWidth, kHapHeight, codec,
                                          chunks));
    writer.finish();
    return file;
}

constexpr azl::HapCodec kHapCodecs[] = {azl::HapCodec::Hap, azl::HapCodec::HapAlpha, azl::HapCodec::HapQ,
                                        azl::HapCodec::HapQAlpha};

} // namespace
