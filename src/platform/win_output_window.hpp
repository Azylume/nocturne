#pragma once

#include <functional>
#include <memory>
#include <string>

namespace azl::win {

// A rectangle in desktop coordinates, physical pixels.
struct Area {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// A key pressed in an output window: its virtual-key code and the modifiers held.
struct WindowKey {
    unsigned virtualKey = 0;
    bool alt = false;
    bool control = false;
    bool shift = false;
    bool windows = false;
};

// An output's window. It lives on the thread that made it, which must pump its messages (the engine's main thread);
// its swapchain may present from any thread. It shows without taking focus: a click on it makes it the foreground
// window, the engine never does by itself. Closing it is the owner's call: the window ignores close requests.
class OutputWindow {
public:
    enum class Style {
        // Covers `area` exactly, above the taskbar.
        Borderless,
        // A titled window whose client area is `area`, behind other windows.
        Titled,
    };

    // `onKey` runs on the owning thread for every key pressed while the window has focus; the window consumes Alt+F4.
    OutputWindow(Style style, Area area, const std::string& title, bool hideCursor,
                 std::function<void(const WindowKey&)> onKey);
    ~OutputWindow();
    OutputWindow(const OutputWindow&) = delete;
    OutputWindow& operator=(const OutputWindow&) = delete;

    // The HWND; null when Windows refused to make it.
    void* handle() const;
    // The client area, in desktop coordinates.
    Area clientArea() const;
    // The HMONITOR showing most of the window.
    void* monitor() const;
    // Draws the client area with GDI whenever Windows asks: `paint` gets the HDC and the client size. Without it the
    // window is black wherever nothing presents.
    void onPaint(std::function<void(void* dc, int width, int height)> paint);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace azl::win
