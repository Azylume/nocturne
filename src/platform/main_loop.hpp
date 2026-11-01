#pragma once

#include <chrono>
#include <functional>
#include <memory>

namespace azl {

// The run loop of the thread that owns windows and displays (the engine's main thread): on Windows a message loop that
// also runs work handed to it. One per process, made, run and destroyed on that thread; post, after and quit work from
// any thread while it exists.
class MainLoop {
public:
    MainLoop();
    ~MainLoop();
    MainLoop(const MainLoop&) = delete;
    MainLoop& operator=(const MainLoop&) = delete;

    // The loop of this process; null before one is made.
    static MainLoop* current();

    // Any thread, returns at once; `work` runs on the loop's thread, in the order posted.
    void post(std::function<void()> work);
    void after(std::chrono::milliseconds delay, std::function<void()> work);
    // Pumps messages and runs work until quit() is called. The loop's thread only.
    void run();
    // Any thread: run() returns once the work posted before it has run.
    void quit();
    bool onLoopThread() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace azl
