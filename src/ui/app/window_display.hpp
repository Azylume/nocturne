#pragma once

#include <string>

namespace azl::ui {

// The id the engine gives the display that shows most of this window: `nativeWindow` is a JUCE peer's native handle
// (an NSView on macOS, an HWND on Windows). Empty when there is no window or no such display. Message thread.
std::string displayOfWindow(void* nativeWindow);

} // namespace azl::ui
