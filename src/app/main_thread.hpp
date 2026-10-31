#pragma once

#include <chrono>
#include <functional>
#include <memory>

namespace azl::app {

// The engine's main thread owns the displays, the output windows and their changes, and runs work handed to it: the
// main dispatch queue under the application's run loop on Apple, a MainLoop elsewhere. Any thread may call these.
void onMain(std::function<void()> work);
void onMainAfter(std::chrono::milliseconds delay, std::function<void()> work);
bool onMainThread();
// Ends the main thread's run loop, so the call that runs it returns.
void stopMain();

// Runs `work` on the main thread every `interval`, the first time one interval from now, until it is destroyed. Made
// and destroyed on the main thread.
class MainTimer {
public:
    MainTimer(std::chrono::milliseconds interval, std::function<void()> work);
    ~MainTimer();
    MainTimer(const MainTimer&) = delete;
    MainTimer& operator=(const MainTimer&) = delete;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace azl::app
