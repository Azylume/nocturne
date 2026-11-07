#include "ui/app/window_display.hpp"

#include "platform/display_identity_apple.hpp"

#import <AppKit/AppKit.h>

namespace azl::ui {

std::string displayOfWindow(void* nativeWindow)
{
    NSScreen* screen = static_cast<NSView*>(nativeWindow).window.screen;
    if (!screen)
        return {};
    return displayIdentity([screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue]);
}

void zoomWindow(void* nativeWindow)
{
    [static_cast<NSView*>(nativeWindow).window performZoom:nil];
}

} // namespace azl::ui
