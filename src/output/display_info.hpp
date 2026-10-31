#pragma once

#include <cstdint>
#include <string>

namespace azl {

// An attached display. The id is stable across reconnects and reboots, unlike the display's index, except for a display
// without a serial number, whose id names its port (unit), which a splitter or another socket changes.
struct DisplayInfo {
    std::string id;
    std::string name;
    uint32_t width = 0;
    uint32_t height = 0;
    double refreshHz = 60.0;
    // 0 where the display does not report it.
    uint32_t vendor = 0;
    uint32_t model = 0;
    uint32_t serial = 0;
    uint32_t unit = 0;
    // Its top-left corner in the desktop arrangement.
    int x = 0;
    int y = 0;
    // The display it mirrors, empty when it shows its own picture. A mirroring display cannot be opened.
    std::string mirrorOf = {};
};

} // namespace azl
