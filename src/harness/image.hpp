#pragma once

#include <cstdint>
#include <vector>

namespace azl {

struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
};

} // namespace azl
