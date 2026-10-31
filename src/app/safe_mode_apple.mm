#include "app/safe_mode.hpp"

#include "app/main_thread.hpp"
#include "app/safe_mode_protocol.hpp"

#import <AppKit/AppKit.h>

#include <csignal>
#include <cstdio>
#include <memory>

namespace azl {

namespace {

NSWindow* safeModeWindow(NSScreen* screen, bool windowed)
{
    const NSRect frame = windowed ? NSMakeRect(NSMidX(screen.visibleFrame) - 320, NSMidY(screen.visibleFrame) - 180, 640, 360)
                                  : screen.frame;
    NSWindow* window = [[NSWindow alloc] initWithContentRect:frame
                                                   styleMask:windowed ? NSWindowStyleMaskTitled : NSWindowStyleMaskBorderless
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    if (!windowed)
        window.level = NSMainMenuWindowLevel + 1;
    window.title = @"Nocturne safe mode";
    window.backgroundColor = NSColor.blackColor;
    window.releasedWhenClosed = NO;
    return window;
}

void fill(NSWindow* window, NSImage* image, NSString* reason)
{
    NSView* content = window.contentView;
    const NSRect bounds = content.bounds;
    if (image) {
        NSImageView* picture = [NSImageView imageViewWithImage:image];
        picture.frame = bounds;
        picture.imageScaling = NSImageScaleProportionallyUpOrDown;
        picture.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [content addSubview:picture];
    }
    NSTextField* label = [NSTextField wrappingLabelWithString:reason];
    label.textColor = [NSColor colorWithWhite:0.6 alpha:1.0];
    label.font = [NSFont monospacedSystemFontOfSize:14 weight:NSFontWeightRegular];
    label.frame = NSMakeRect(16, 12, bounds.size.width - 32, 60);
    label.autoresizingMask = NSViewWidthSizable;
    [content addSubview:label];
}

dispatch_source_t stopOnSignal(int sig)
{
    std::signal(sig, SIG_IGN);
    dispatch_source_t source =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, uintptr_t(sig), 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(source, ^{ app::stopMain(); });
    dispatch_resume(source);
    return source;
}

} // namespace

int runSafeMode(const app::EngineConfig& opt)
{
    const std::string& reason = opt.safeMode;
    const bool windowed = opt.windowed || opt.virtualDisplays > 0;
    const bool offscreen = opt.offscreen;
    const std::string& fallbackImage = opt.fallbackImage;
    std::printf("safe mode: %s\n", reason.c_str());
    std::fflush(stdout);
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:offscreen ? NSApplicationActivationPolicyProhibited
                                         : NSApplicationActivationPolicyAccessory];
    id activity = [NSProcessInfo.processInfo beginActivityWithOptions:NSActivityIdleDisplaySleepDisabled
                                                               reason:@"safe mode notice"];

    NSImage* image = nil;
    if (!fallbackImage.empty()) {
        image = [[NSImage alloc] initWithContentsOfFile:[NSString stringWithUTF8String:fallbackImage.c_str()]];
        if (!image)
            std::printf("safe mode: cannot read the fallback image %s\n", fallbackImage.c_str());
    }
    NSString* text = [NSString stringWithFormat:@"Nocturne safe mode: %s", reason.c_str()];
    NSMutableArray<NSWindow*>* windows = [NSMutableArray array];
    NSArray<NSScreen*>* screens = offscreen ? @[] : windowed ? @[ NSScreen.mainScreen ] : NSScreen.screens;
    for (NSScreen* screen in screens) {
        NSWindow* window = safeModeWindow(screen, windowed);
        fill(window, image, text);
        if (windowed)
            [window orderBack:nil];
        else
            [window orderFrontRegardless];
        [windows addObject:window];
    }
    std::fflush(stdout);

    int exitCode = 0;
    auto protocol = std::make_unique<SafeModeProtocol>(opt, restartRequest(&exitCode, app::stopMain),
                                                       stopRequest(&exitCode, app::stopMain));

    dispatch_source_t onInt = stopOnSignal(SIGINT);
    dispatch_source_t onTerm = stopOnSignal(SIGTERM);
    [NSApp run];
    protocol.reset();
    dispatch_source_cancel(onInt);
    dispatch_source_cancel(onTerm);
    [NSProcessInfo.processInfo endActivity:activity];
    std::printf("stopped\n");
    return exitCode;
}

} // namespace azl
