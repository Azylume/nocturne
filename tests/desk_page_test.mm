// The desk page in a real browser engine, WebKit.
#include "desk_rig.hpp"

#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

#include <chrono>
#include <functional>
#include <memory>

namespace {

// Runs the main run loop until `done` or the timeout.
bool spinUntil(const std::function<bool()>& done, double seconds = 15.0)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < until) {
        @autoreleasepool {
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                     beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
        }
        if (done())
            return true;
    }
    return false;
}

std::string evaluate(WKWebView* view, NSString* script)
{
    auto answer = std::make_shared<std::pair<bool, std::string>>();
    [view evaluateJavaScript:script
           completionHandler:^(id value, NSError*) {
               if ([value isKindOfClass:[NSString class]])
                   answer->second = [(NSString*)value UTF8String];
               else if ([value isKindOfClass:[NSNumber class]])
                   answer->second = [[(NSNumber*)value stringValue] UTF8String];
               answer->first = true;
           }];
    spinUntil([&] { return answer->first; }, 5.0);
    return answer->second;
}

// A web view in no window, so nothing shows on the screen.
WKWebView* hiddenPage(uint16_t port)
{
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
    WKWebView* view = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, 390, 844)
                                         configuration:[[WKWebViewConfiguration alloc] init]];
    NSString* url = [NSString stringWithFormat:@"http://127.0.0.1:%u/", unsigned(port)];
    [view loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:url]]];
    return view;
}

} // namespace

TEST_CASE("desk: the page draws a button per slot and marks the live one, in a web view no window shows")
{
    Desk desk;
    WKWebView* view = hiddenPage(desk.server.port());
    REQUIRE(spinUntil([&] { return evaluate(view, @"document.querySelectorAll('#lineup .slot').length") == "4"; }));
    CHECK(evaluate(view, @"document.querySelector('.slot.live .dj').textContent") == "Opener");
    CHECK(evaluate(view, @"document.querySelector('.slot.next .dj').textContent") == "Headliner");
    CHECK(evaluate(view, @"document.querySelector('.slot.missing .meta').textContent").find("deck missing") !=
          std::string::npos);
    CHECK(evaluate(view, @"document.getElementById('status').textContent").starts_with("Next: Headliner at 03:00"));
    CHECK(evaluate(view, @"String(document.body.classList.contains('offline'))") == "false");
    const bool windowless = view.window == nil;
    CHECK(windowless);

    Desk none(azl::defaultComposition());
    WKWebView* empty = hiddenPage(none.server.port());
    REQUIRE(spinUntil([&] { return evaluate(empty, @"String(document.body.classList.contains('empty'))") == "true"; }));
    CHECK(evaluate(empty, @"document.querySelectorAll('#lineup .slot').length") == "0");
    CHECK(evaluate(empty, @"document.getElementById('liveDeck').textContent").starts_with("Live: "));
}
