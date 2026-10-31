#pragma once

#include "output/displays.hpp"
#include "platform/win_output_window.hpp"

#include <memory>
#include <optional>
#include <string>

namespace azl {

// What a key in an output window asks for: Escape alone releases the display, Alt+F4 (Windows' quit key) ends the
// engine, anything else nothing.
std::optional<Displays::OutputKey> outputKeyFor(const win::WindowKey& key);

// Windows' monitors as outputs see them, followed through display changes, with the ids macOS gives the same monitors.
// Each surface is a borderless window covering its monitor, or a titled one on it when windowed, presenting through a
// Vulkan swapchain (FIFO) paced by the monitor's vertical blank.
//
// With `virtualDisplays` > 0 there are instead that many always-attached displays "window-1", "window-2", ...: titled
// windows side by side along the top of the primary monitor, so several outputs can be tried on one monitor. Their
// swapchains are the windows' size, not 1920x1080.
class WinDisplays : public Displays {
public:
    // On the thread that runs the MainLoop, like every other call here.
    WinDisplays(bool windowed, int virtualDisplays);
    ~WinDisplays() override;

    std::vector<DisplayInfo> attached() const override;
    std::unique_ptr<Surface> open(const std::string& id, const gpu::Device& device) override;
    // Extends the desktop over every monitor, as Settings would and for good.
    bool unmirror(const std::vector<std::string>& ids) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace azl
