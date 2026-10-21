#pragma once

#include "test_client.hpp"

#include <cstdlib>
#include <string>

// Reads the test clips (tests/media/make_media.sh) off the screen at 960x540: the frame index as 8 bits across the top
// half, each bit 120 pixels wide, and two colour patches in the bottom half.
namespace {

constexpr uint32_t kBitWidth = 120;
constexpr uint32_t kBitRow = 135;
constexpr int kPatchTolerance = 4;

// The clip frame on screen, read from its bar code. Fails unless every bit is clearly black or white.
inline int shownFrame(const azl::Image& image)
{
    int index = 0;
    for (uint32_t bit = 0; bit < 8; ++bit) {
        const uint8_t* p = pixel(image, kBitWidth / 2 + bit * kBitWidth, kBitRow);
        CHECK_MESSAGE((p[0] < 24 || p[0] > 231), "bit " << bit << " is not black or white: " << int(p[0]));
        index = index * 2 + (p[0] > 128);
    }
    return index;
}

inline void checkColour(const azl::Image& image, uint32_t x, uint32_t y, int r, int g, int b)
{
    const uint8_t* p = pixel(image, x, y);
    INFO("pixel " << x << "," << y << " is " << int(p[0]) << "," << int(p[1]) << "," << int(p[2]));
    CHECK(std::abs(p[0] - r) <= kPatchTolerance);
    CHECK(std::abs(p[1] - g) <= kPatchTolerance);
    CHECK(std::abs(p[2] - b) <= kPatchTolerance);
}

inline void checkPatches(const azl::Image& image)
{
    checkColour(image, 240, 405, 200, 40, 40);
    checkColour(image, 720, 405, 40, 160, 80);
}

// The test card's white edge and dark background.
inline void checkTestCard(const azl::Image& image)
{
    CHECK(pixel(image, 1, 1)[0] > 250);
    CHECK(std::abs(pixel(image, 90, 30)[0] - 20) <= 2);
}

inline int frameAfterStep(azl::EngineHarness& harness, int frames = 1)
{
    harness.step(frames);
    return shownFrame(harness.readScreen());
}

const std::string kUnknownHash(64, 'a');

} // namespace
