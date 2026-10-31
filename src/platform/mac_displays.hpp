#pragma once

#include "output/displays.hpp"

#include <memory>
#include <optional>
#include <string>

namespace azl {

// The Mac's displays, followed through screen-parameter notifications and display reconfigurations (mirroring too). A
// display's id is its vendor, model and serial number; displays that report no serial use their unit number (the port)
// instead. Each surface is a borderless window covering its display, or a regular window on it when windowed.
//
// With `virtualDisplays` > 0 there are instead that many always-attached displays "window-1", "window-2", ...: windows
// side by side on the main display, each with a 1920x1080 drawable, so several outputs can be tried on one monitor.
// Whether an output's window hides the cursor while it is over it: only a borderless one covering its display. A parked
// or removed output has no window, so the cursor shows there again.
bool hidesCursor(bool windowed, int virtualDisplays);

// What to do with the cursor given whether the mouse is over an output window, whether the engine hid it, and whether
// the WindowServer shows it (it can show it again behind the engine's back).
enum class CursorStep { none, hide, show };
CursorStep cursorStep(bool overOutput, bool hidden, bool visible);

// The modifiers held with a key, as the decision below sees them.
struct KeyModifiers {
    bool command = false;
    bool option = false;
    bool control = false;
    bool shift = false;
};

// What a key down in an output window asks for, given the key's code, the modifiers held and the characters without
// modifiers: Escape alone releases the display, Q with Command alone quits, anything else (Cmd+Option+Q, Cmd+Shift+Q,
// which is log out) nothing.
std::optional<Displays::OutputKey> outputKeyFor(unsigned short keyCode, KeyModifiers modifiers,
                                                const std::string& characters);

class MacDisplays : public Displays {
public:
    // Main thread only, like everything else here.
    MacDisplays(bool windowed, int virtualDisplays);
    ~MacDisplays() override;

    std::vector<DisplayInfo> attached() const override;
    std::unique_ptr<Surface> open(const std::string& id, const gpu::Device& device) override;
    // Through CoreGraphics, permanently, as System Settings would.
    bool unmirror(const std::vector<std::string>& ids) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace azl
