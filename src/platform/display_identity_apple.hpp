#pragma once

#include <CoreGraphics/CoreGraphics.h>

#include <cstdio>
#include <string>

namespace azl {

// A display's id: vendor, model and serial number in hex, or the unit number in place of a missing serial. The engine
// names its displays so, and a UI on the same Mac names the display it is on the same way.
inline std::string displayIdentity(CGDirectDisplayID display)
{
    const uint32_t serial = CGDisplaySerialNumber(display);
    char id[64];
    if (serial)
        std::snprintf(id, sizeof(id), "%x-%x-%x", CGDisplayVendorNumber(display), CGDisplayModelNumber(display), serial);
    else
        std::snprintf(id, sizeof(id), "%x-%x-u%u", CGDisplayVendorNumber(display), CGDisplayModelNumber(display),
                      CGDisplayUnitNumber(display));
    return id;
}

} // namespace azl
