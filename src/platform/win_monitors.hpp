#pragma once

#include "output/display_info.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace azl::win {

// What a monitor's EDID block says about it, with the numbers macOS reports for the same monitor
// (CGDisplayVendorNumber, ModelNumber, SerialNumber). Nothing when the bytes are not an EDID block.
struct Edid {
    uint32_t vendor = 0;
    uint32_t model = 0;
    uint32_t serial = 0;
    // The monitor name descriptor; empty when the block has none.
    std::string name;
};
std::optional<Edid> parseEdid(std::span<const uint8_t> bytes);

// A display's id, formed as macOS forms it, so a monitor keeps its id when it moves between a Mac and a PC: vendor,
// model and serial number in hex, or the unit (here the connector's target id) in place of a missing serial.
std::string displayIdentity(uint32_t vendor, uint32_t model, uint32_t serial, uint32_t unit);

struct Monitor {
    DisplayInfo info;
    // The GDI device name (\\.\DISPLAY1) of the desktop area it shows.
    std::wstring gdiName;
};

// The monitors showing the desktop, from Windows' display configuration: the primary first, then left to right. A
// monitor that duplicates another names it in mirrorOf. Sizes and positions are physical pixels. Any thread.
std::vector<Monitor> activeMonitors();

// The HMONITOR of the desktop area with that GDI name; null when there is none.
void* monitorHandle(const std::wstring& gdiName);

// Extends the desktop over every attached monitor, which ends duplicate mode for all of them, as Settings would and
// for good. False when Windows refused.
bool extendDesktop();

// Makes this process see physical pixels on every monitor (per-monitor DPI awareness), so window and monitor
// rectangles agree. Call before the first window; harmless when it is already set.
void usePhysicalPixels();

// Calls `changed` on the thread that made the watcher once monitors were attached, detached or reconfigured: a quarter
// second after the last such message, so one reconfiguration makes one call. That thread must pump its messages.
class DisplayWatcher {
public:
    explicit DisplayWatcher(std::function<void()> changed);
    ~DisplayWatcher();
    DisplayWatcher(const DisplayWatcher&) = delete;
    DisplayWatcher& operator=(const DisplayWatcher&) = delete;

    // Delivers what WM_DISPLAYCHANGE would, as if Windows had sent it: for tests.
    void simulateChange();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Paces a present thread by one monitor's refreshes: DXGI's vertical blank wait on the output showing it, or a timer
// at its refresh rate where no output is found (a remote session).
class VblankWaiter {
public:
    VblankWaiter(void* monitor, double refreshHz);
    ~VblankWaiter();
    VblankWaiter(const VblankWaiter&) = delete;
    VblankWaiter& operator=(const VblankWaiter&) = delete;

    // Blocks until the monitor's next refresh begins and returns when it did, on the steady clock.
    std::chrono::steady_clock::time_point wait();
    // False when it paces by a timer.
    bool hardware() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace azl::win
