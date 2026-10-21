#include "platform/mac_displays.hpp"

#import <AppKit/AppKit.h>
#import <QuartzCore/CADisplayLink.h>
#import <QuartzCore/CAMetalLayer.h>

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// The link may run below the display's maximum rate (e.g. 60 on a 120 Hz display), so it records the period it
// actually runs at.
@interface AZLRefreshTarget : NSObject
@property(nonatomic) azl::Surface::Refresh callback;
@property(atomic) double period;
- (void)tick:(CADisplayLink*)link;
@end

@implementation AZLRefreshTarget
- (void)tick:(CADisplayLink*)link
{
    @autoreleasepool {
        self.period = link.targetTimestamp - link.timestamp;
        const auto age = std::chrono::duration<double>(CACurrentMediaTime() - link.timestamp);
        self.callback(std::chrono::steady_clock::now() -
                      std::chrono::duration_cast<std::chrono::steady_clock::duration>(age));
    }
}
@end

namespace {

using ConnectionFn = int (*)();
using SetPropertyFn = int (*)(int, int, CFStringRef, CFTypeRef);
using VisibleFn = int (*)();

// The WindowServer ignores cursor changes from an app that is not frontmost, and the engine is an accessory app that
// is rarely frontmost (never after login). This private connection property lets it hide the cursor anyway.
bool hideInBackground()
{
    static const bool enabled = [] {
        auto connection = reinterpret_cast<ConnectionFn>(dlsym(RTLD_DEFAULT, "_CGSDefaultConnection"));
        auto setProperty = reinterpret_cast<SetPropertyFn>(dlsym(RTLD_DEFAULT, "CGSSetConnectionProperty"));
        const int error = connection && setProperty ? setProperty(connection(), connection(),
                                                                  CFSTR("SetsCursorInBackground"), kCFBooleanTrue)
                                                    : -1;
        if (error)
            std::fprintf(stderr, "cursor: cannot hide it in the background (%d)\n", error);
        return error == 0;
    }();
    return enabled;
}

bool cursorVisible()
{
    static const auto visible = reinterpret_cast<VisibleFn>(dlsym(RTLD_DEFAULT, "CGCursorIsVisible"));
    return !visible || visible();
}

} // namespace

@class AZLCursorHider;

// Hides the cursor while the mouse is over any output window: from the tracking areas' events, and every half second
// against the WindowServer, which can show it again (a display reattach, wake). Falls back to a blank cursor, which
// only works while the engine is frontmost.
@interface AZLCursor : NSObject
+ (AZLCursor*)shared;
- (void)add:(AZLCursorHider*)hider;
- (void)remove:(AZLCursorHider*)hider;
- (void)sync;
@end

@interface AZLCursorHider : NSObject
- (instancetype)initWithView:(NSView*)view;
- (BOOL)mouseInside;
- (void)remove;
@end

@implementation AZLCursor {
    NSHashTable<AZLCursorHider*>* _hiders;
    NSTimer* _timer;
    NSCursor* _blank;
    bool _hidden;
}

+ (AZLCursor*)shared
{
    static AZLCursor* shared = [AZLCursor new];
    return shared;
}

- (instancetype)init
{
    self = [super init];
    _hiders = [NSHashTable weakObjectsHashTable];
    NSImage* image = [NSImage imageWithSize:NSMakeSize(1, 1)
                                    flipped:NO
                             drawingHandler:^BOOL(NSRect) {
                                 return YES;
                             }];
    _blank = [[NSCursor alloc] initWithImage:image hotSpot:NSZeroPoint];
    hideInBackground();
    return self;
}

- (void)add:(AZLCursorHider*)hider
{
    [_hiders addObject:hider];
    if (!_timer)
        _timer = [NSTimer scheduledTimerWithTimeInterval:0.5
                                                  target:self
                                                selector:@selector(sync)
                                                userInfo:nil
                                                 repeats:YES];
    [self sync];
}

- (void)remove:(AZLCursorHider*)hider
{
    [_hiders removeObject:hider];
    [self sync];
    if (_hiders.count == 0) {
        [_timer invalidate];
        _timer = nil;
    }
}

- (bool)mouseOverOutput
{
    for (AZLCursorHider* hider in _hiders)
        if ([hider mouseInside])
            return true;
    return false;
}

- (void)sync
{
    const bool over = [self mouseOverOutput];
    if (over)
        [_blank set];
    switch (azl::cursorStep(over, _hidden, cursorVisible())) {
    case azl::CursorStep::hide:
        CGDisplayHideCursor(kCGNullDirectDisplay);
        _hidden = true;
        break;
    case azl::CursorStep::show:
        for (int i = 0; i < 8 && !cursorVisible(); ++i)
            CGDisplayShowCursor(kCGNullDirectDisplay);
        [[NSCursor arrowCursor] set];
        _hidden = false;
        break;
    case azl::CursorStep::none:
        break;
    }
}
@end

@implementation AZLCursorHider {
    NSView* _view;
    NSTrackingArea* _area;
}

- (instancetype)initWithView:(NSView*)view
{
    self = [super init];
    _view = view;
    _area = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved | NSTrackingActiveAlways |
                     NSTrackingInVisibleRect
               owner:self
            userInfo:nil];
    [view addTrackingArea:_area];
    [AZLCursor.shared add:self];
    return self;
}

- (BOOL)mouseInside
{
    return NSMouseInRect(NSEvent.mouseLocation, _view.window.frame, NO);
}

- (void)remove
{
    [_view removeTrackingArea:_area];
    [AZLCursor.shared remove:self];
}

- (void)mouseEntered:(NSEvent*)event
{
    [AZLCursor.shared sync];
}

- (void)mouseMoved:(NSEvent*)event
{
    [AZLCursor.shared sync];
}

- (void)mouseExited:(NSEvent*)event
{
    [AZLCursor.shared sync];
}
@end

namespace azl {

bool hidesCursor(bool windowed, int virtualDisplays)
{
    return !windowed && virtualDisplays == 0;
}

CursorStep cursorStep(bool overOutput, bool hidden, bool visible)
{
    if (overOutput && (!hidden || visible))
        return CursorStep::hide;
    if (!overOutput && hidden)
        return CursorStep::show;
    return CursorStep::none;
}

namespace {

constexpr CGFloat kWindowedWidth = 960;
constexpr CGFloat kWindowedHeight = 540;
constexpr CGFloat kGap = 8;
constexpr CGFloat kTitleBar = 28;
constexpr const char* kVirtualPrefix = "window-";

CGDirectDisplayID displayIdOf(NSScreen* screen)
{
    return [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
}

std::string identityOf(CGDirectDisplayID display)
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

std::string identityOf(NSScreen* screen)
{
    return identityOf(displayIdOf(screen));
}

NSScreen* screenWithIdentity(const std::string& id)
{
    for (NSScreen* screen in [NSScreen screens])
        if (identityOf(screen) == id)
            return screen;
    return nil;
}

std::vector<CGDirectDisplayID> onlineDisplays()
{
    CGDirectDisplayID ids[32];
    uint32_t count = 0;
    if (CGGetOnlineDisplayList(32, ids, &count) != kCGErrorSuccess)
        return {};
    return {ids, ids + count};
}

// 1-based index of a virtual display id, or 0.
int virtualIndex(const std::string& id, int count)
{
    if (id.rfind(kVirtualPrefix, 0) != 0)
        return 0;
    const int index = std::atoi(id.c_str() + std::strlen(kVirtualPrefix));
    return index >= 1 && index <= count ? index : 0;
}

NSWindow* borderlessWindow(NSScreen* screen)
{
    NSWindow* window = [[NSWindow alloc] initWithContentRect:screen.frame
                                                   styleMask:NSWindowStyleMaskBorderless
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    window.level = NSMainMenuWindowLevel + 1;
    return window;
}

NSWindow* titledWindow(NSRect frame, const std::string& title)
{
    NSWindow* window = [[NSWindow alloc] initWithContentRect:frame
                                                   styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskMiniaturizable
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    window.title = [NSString stringWithUTF8String:("Nocturne " + title).c_str()];
    return window;
}

NSRect centred(NSScreen* screen)
{
    const NSRect area = screen.visibleFrame;
    return NSMakeRect(NSMidX(area) - kWindowedWidth / 2, NSMidY(area) - kWindowedHeight / 2, kWindowedWidth,
                      kWindowedHeight);
}

// Virtual display `index` of `count`, side by side along the top of the main display.
NSRect tile(NSScreen* screen, int index, int count)
{
    const NSRect area = screen.visibleFrame;
    const CGFloat width = std::min(kWindowedWidth, (area.size.width - kGap * (count + 1)) / count);
    const CGFloat height = width * 9 / 16;
    return NSMakeRect(area.origin.x + kGap + (index - 1) * (width + kGap), NSMaxY(area) - height - kGap - kTitleBar, width,
                      height);
}

CAMetalLayer* attachLayer(NSView* view, MTL::Device* device, CGSize drawableSize, CGFloat scale)
{
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.device = (__bridge id<MTLDevice>)device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    layer.displaySyncEnabled = YES;
    layer.maximumDrawableCount = 3;
    layer.contentsScale = scale;
    layer.drawableSize = drawableSize;
    view.wantsLayer = YES;
    view.layer = layer;
    return layer;
}

class MacSurface : public Surface {
public:
    MacSurface(NSWindow* window, MTL::Device* device, CGSize drawableSize, double refreshHz, bool hideCursor = false)
        : window_(window), refreshHz_(refreshHz)
    {
        window_.releasedWhenClosed = NO;
        layer_ = attachLayer(window_.contentView, device, drawableSize, window_.screen.backingScaleFactor);
        target_ = [AZLRefreshTarget new];
        link_ = [window_.contentView displayLinkWithTarget:target_ selector:@selector(tick:)];
        if (window_.styleMask & NSWindowStyleMaskTitled)
            [window_ orderBack:nil];
        else
            [window_ orderFrontRegardless];
        if (hideCursor)
            cursorHider_ = [[AZLCursorHider alloc] initWithView:window_.contentView];
    }

    ~MacSurface() override
    {
        [cursorHider_ remove];
        [link_ invalidate];
        [window_ close];
    }

    double refreshHz() const override
    {
        const double period = target_.period;
        return period > 0.0 ? 1.0 / period : refreshHz_;
    }

    void runRefreshes(const std::atomic<bool>& running, const Refresh& onRefresh) override
    {
        @autoreleasepool {
            target_.callback = onRefresh;
            [link_ addToRunLoop:NSRunLoop.currentRunLoop forMode:NSDefaultRunLoopMode];
            while (running) {
                @autoreleasepool {
                    [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode
                                           beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
                }
            }
            [link_ invalidate];
        }
    }

    MTL::Texture* nextTarget() override
    {
        drawable_ = [layer_ nextDrawable];
        return drawable_ ? (__bridge MTL::Texture*)drawable_.texture : nullptr;
    }

    void present(MTL::CommandBuffer* cmd, Shown shown) override
    {
        id<CAMetalDrawable> drawable = drawable_;
        drawable_ = nil;
        [drawable addPresentedHandler:^(id<MTLDrawable> presented) {
            shown(presented.presentedTime);
        }];
        cmd->presentDrawable((__bridge MTL::Drawable*)drawable);
    }

private:
    NSWindow* window_;
    double refreshHz_;
    CAMetalLayer* layer_ = nil;
    AZLRefreshTarget* target_ = nil;
    CADisplayLink* link_ = nil;
    id<CAMetalDrawable> drawable_ = nil;
    AZLCursorHider* cursorHider_ = nil;
};

double modeRefreshHz(CGDirectDisplayID display)
{
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
    const double hz = mode ? CGDisplayModeGetRefreshRate(mode) : 0.0;
    CGDisplayModeRelease(mode);
    return hz > 0.0 ? hz : 60.0;
}

// A mirroring display has no NSScreen, so no name of its own.
DisplayInfo infoOf(CGDirectDisplayID display, std::string name, double refreshHz)
{
    const CGRect bounds = CGDisplayBounds(display);
    const CGDirectDisplayID mirrored = CGDisplayMirrorsDisplay(display);
    return {.id = identityOf(display),
            .name = std::move(name),
            .width = uint32_t(CGDisplayPixelsWide(display)),
            .height = uint32_t(CGDisplayPixelsHigh(display)),
            .refreshHz = refreshHz,
            .vendor = CGDisplayVendorNumber(display),
            .model = CGDisplayModelNumber(display),
            .serial = CGDisplaySerialNumber(display),
            .unit = CGDisplayUnitNumber(display),
            .x = int(bounds.origin.x),
            .y = int(bounds.origin.y),
            .mirrorOf = mirrored == kCGNullDirectDisplay ? std::string() : identityOf(mirrored)};
}

DisplayInfo infoOf(NSScreen* screen)
{
    return infoOf(displayIdOf(screen), screen.localizedName.UTF8String, double(screen.maximumFramesPerSecond));
}

bool hasScreen(CGDirectDisplayID display)
{
    for (NSScreen* screen in [NSScreen screens])
        if (displayIdOf(screen) == display)
            return true;
    return false;
}

bool configureUnmirrored(const std::vector<CGDirectDisplayID>& displays)
{
    CGDisplayConfigRef config = nullptr;
    if (CGBeginDisplayConfiguration(&config) != kCGErrorSuccess)
        return false;
    for (CGDirectDisplayID display : displays)
        if (CGConfigureDisplayMirrorOfDisplay(config, display, kCGNullDirectDisplay) != kCGErrorSuccess) {
            CGCancelDisplayConfiguration(config);
            return false;
        }
    return CGCompleteDisplayConfiguration(config, kCGConfigurePermanently) == kCGErrorSuccess;
}

} // namespace

struct MacDisplays::Impl {
    MacDisplays* owner = nullptr;
    bool windowed = false;
    int virtualDisplays = 0;
    id observer = nil;
    std::atomic<bool> changePending{false};

    // One reconfiguration calls back once per display; they make one change.
    static void reconfigured(CGDirectDisplayID, CGDisplayChangeSummaryFlags flags, void* context)
    {
        auto* impl = static_cast<Impl*>(context);
        if ((flags & kCGDisplayBeginConfigurationFlag) || impl->changePending.exchange(true))
            return;
        dispatch_async(dispatch_get_main_queue(), ^{
            impl->changePending = false;
            impl->owner->changed();
        });
    }
};

MacDisplays::MacDisplays(bool windowed, int virtualDisplays) : impl_(std::make_unique<Impl>())
{
    impl_->owner = this;
    impl_->windowed = windowed;
    impl_->virtualDisplays = virtualDisplays;
    impl_->observer =
        [[NSNotificationCenter defaultCenter] addObserverForName:NSApplicationDidChangeScreenParametersNotification
                                                          object:nil
                                                           queue:[NSOperationQueue mainQueue]
                                                      usingBlock:^(NSNotification*) {
                                                          changed();
                                                      }];
    CGDisplayRegisterReconfigurationCallback(&Impl::reconfigured, impl_.get());
}

MacDisplays::~MacDisplays()
{
    CGDisplayRemoveReconfigurationCallback(&Impl::reconfigured, impl_.get());
    [[NSNotificationCenter defaultCenter] removeObserver:impl_->observer];
}

std::vector<DisplayInfo> MacDisplays::attached() const
{
    std::vector<DisplayInfo> displays;
    NSArray<NSScreen*>* screens = [NSScreen screens];
    if (impl_->virtualDisplays == 0) {
        for (NSScreen* screen in screens)
            displays.push_back(infoOf(screen));
        for (CGDirectDisplayID display : onlineDisplays())
            if (CGDisplayMirrorsDisplay(display) != kCGNullDirectDisplay && !hasScreen(display))
                displays.push_back(infoOf(display, "", modeRefreshHz(display)));
        return displays;
    }
    const double hz = screens.count ? double(screens[0].maximumFramesPerSecond) : 60.0;
    for (int i = 1; i <= impl_->virtualDisplays; ++i)
        displays.push_back({kVirtualPrefix + std::to_string(i), "Window " + std::to_string(i), 1920, 1080, hz});
    return displays;
}

std::unique_ptr<Surface> MacDisplays::open(const std::string& id, MTL::Device* device)
{
    if (impl_->virtualDisplays > 0) {
        const int index = virtualIndex(id, impl_->virtualDisplays);
        NSArray<NSScreen*>* screens = [NSScreen screens];
        if (!index || !screens.count)
            return nullptr;
        NSScreen* main = screens[0];
        return std::make_unique<MacSurface>(titledWindow(tile(main, index, impl_->virtualDisplays), id), device,
                                            CGSizeMake(1920, 1080), double(main.maximumFramesPerSecond));
    }
    NSScreen* screen = screenWithIdentity(id);
    if (!screen)
        return nullptr;
    NSWindow* window = impl_->windowed ? titledWindow(centred(screen), id) : borderlessWindow(screen);
    const CGFloat scale = screen.backingScaleFactor;
    const NSSize size = window.contentView.bounds.size;
    return std::make_unique<MacSurface>(window, device, CGSizeMake(size.width * scale, size.height * scale),
                                        double(screen.maximumFramesPerSecond),
                                        hidesCursor(impl_->windowed, impl_->virtualDisplays));
}

bool MacDisplays::unmirror(const std::vector<std::string>& ids)
{
    if (impl_->virtualDisplays > 0)
        return false;
    std::vector<CGDirectDisplayID> displays;
    for (CGDirectDisplayID display : onlineDisplays())
        if (std::find(ids.begin(), ids.end(), identityOf(display)) != ids.end())
            displays.push_back(display);
    return !displays.empty() && configureUnmirrored(displays);
}

} // namespace azl
