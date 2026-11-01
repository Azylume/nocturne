#include "ui/app/window_display.hpp"

#include "platform/win_monitors.hpp"

#include <windows.h>

namespace azl::ui {

std::string displayOfWindow(void* nativeWindow)
{
    if (!nativeWindow)
        return {};
    const HMONITOR shown = MonitorFromWindow(static_cast<HWND>(nativeWindow), MONITOR_DEFAULTTONULL);
    for (const win::Monitor& monitor : win::activeMonitors())
        if (monitor.info.mirrorOf.empty() && win::monitorHandle(monitor.gdiName) == shown)
            return monitor.info.id;
    return {};
}

} // namespace azl::ui
