#include "app/safe_mode.hpp"

#include "engine/machine_actions.hpp"
#include "net/websocket.hpp"
#include "protocol/build_info.hpp"
#include "supervisor/restart_request.hpp"

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

void stopApp()
{
    [NSApp stop:nil];
    NSEvent* wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                       location:NSZeroPoint
                                  modifierFlags:0
                                      timestamp:0
                                   windowNumber:0
                                        context:nil
                                        subtype:0
                                          data1:0
                                          data2:0];
    [NSApp postEvent:wake atStart:YES];
}

dispatch_source_t stopOnSignal(int sig)
{
    std::signal(sig, SIG_IGN);
    dispatch_source_t source =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, uintptr_t(sig), 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(source, ^{ stopApp(); });
    dispatch_resume(source);
    return source;
}

// The machine status and the guarded actions, and nothing of the document.
class SafeModeProtocol {
public:
    SafeModeProtocol(const app::EngineConfig& opt, MachineActions::Restart restart, MachineActions::Stop stop)
        : reason_(opt.safeMode), document_(Composition{}, [](auto) {}),
          actions_(opt.state.empty() ? std::filesystem::path() : std::filesystem::path(opt.state) / "show-lock.json",
                   std::move(restart), std::move(stop))
    {
        document_.setReadOnly("the engine is in safe mode: " + reason_, {"machine"});
        document_.handle("machine", [this](const Message& message, DocumentServer::Sink reply) {
            handle(message, reply);
        });
        try {
            server_ = std::make_unique<WebSocketServer>(document_, opt.port);
            std::printf("protocol on ws://0.0.0.0:%u (safe mode)\n", unsigned(server_->port()));
        } catch (const std::exception& e) {
            std::printf("protocol: %s\n", e.what());
        }
    }
    ~SafeModeProtocol()
    {
        server_.reset();
        document_.handle("machine", nullptr);
    }

private:
    void handle(const Message& message, const DocumentServer::Sink& reply)
    {
        const std::string op = message.value("op", "");
        if (op != "safeMode" && actions_.handle(message, reply))
            return;
        if (op == "unsubscribe")
            return;
        if (op != "status" && op != "subscribe")
            return reply({{"type", "error"}, {"message", "the engine is in safe mode: " + op + " is not served"}});
        reply({{"type", "machine"},
               {"op", "status"},
               {"mode", "safe"},
               {"reason", reason_},
               {"showLock", actions_.locked()},
               {"version", buildId()},
               {"outputs", Message::array()}});
    }

    std::string reason_;
    DocumentServer document_;
    MachineActions actions_;
    std::unique_ptr<WebSocketServer> server_;
};

} // namespace

MachineActions::Restart restartRequest(int* exitCode, void (*quit)())
{
    if (!std::getenv(restart::kSupervisedVariable))
        return nullptr;
    return [exitCode, quit](bool safeMode) {
        const int code = safeMode ? restart::kSafeMode : restart::kNormal;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 5), dispatch_get_main_queue(), ^{
            *exitCode = code;
            quit();
        });
    };
}

MachineActions::Stop stopRequest(int* exitCode, void (*quit)())
{
    if (!std::getenv(restart::kSupervisedVariable))
        return nullptr;
    return [exitCode, quit] {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 5), dispatch_get_main_queue(), ^{
            *exitCode = restart::kStop;
            quit();
        });
    };
}

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
    auto protocol =
        std::make_unique<SafeModeProtocol>(opt, restartRequest(&exitCode, stopApp), stopRequest(&exitCode, stopApp));

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
