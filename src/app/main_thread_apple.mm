#include "app/main_thread.hpp"

#import <AppKit/AppKit.h>

#include <memory>

namespace azl::app {

void onMain(std::function<void()> work)
{
    auto held = std::make_shared<std::function<void()>>(std::move(work));
    dispatch_async(dispatch_get_main_queue(), ^{ (*held)(); });
}

void onMainAfter(std::chrono::milliseconds delay, std::function<void()> work)
{
    auto held = std::make_shared<std::function<void()>>(std::move(work));
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, std::chrono::nanoseconds(delay).count()), dispatch_get_main_queue(),
                   ^{ (*held)(); });
}

bool onMainThread()
{
    return [NSThread isMainThread];
}

void stopMain()
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

struct MainTimer::Impl {
    dispatch_source_t source = nullptr;
    std::function<void()> work;
};

MainTimer::MainTimer(std::chrono::milliseconds interval, std::function<void()> work) : impl_(std::make_unique<Impl>())
{
    impl_->work = std::move(work);
    const int64_t nanos = std::chrono::nanoseconds(interval).count();
    impl_->source = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(impl_->source, dispatch_time(DISPATCH_TIME_NOW, nanos), uint64_t(nanos),
                              uint64_t(nanos / 100));
    Impl* impl = impl_.get();
    dispatch_source_set_event_handler(impl_->source, ^{ impl->work(); });
    dispatch_resume(impl_->source);
}

MainTimer::~MainTimer()
{
    dispatch_source_cancel(impl_->source);
}

} // namespace azl::app
