#include "media/hap.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

// The first four bytes are the picture's width and height (up to 1024 each); the rest is the frame.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 4)
        return 0;
    const int width = 1 + ((data[0] | data[1] << 8) & 0x3ff);
    const int height = 1 + ((data[2] | data[3] << 8) & 0x3ff);
    std::vector<azl::HapImage> images;
    if (azl::decodeHapFrame(data + 4, size - 4, width, height, images).empty())
        azl::hapToRgba(images, width, height);
    return 0;
}
