#pragma once

#include "engine/engine.hpp"
#include "engine/screen_painter.hpp"
#include "engine/vsync_phase.hpp"
#include "output/displays.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace azl {

struct OutputStats {
    std::atomic<uint64_t> presents{0};
    std::atomic<uint64_t> shown{0};
    std::atomic<uint64_t> newFrames{0};
    // Rendered frames this output never showed: gaps in the frame index between consecutive new frames while attached.
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> maxIntervalMicros{0};
    // New frames whose gap on screen to the previous new frame exceeds the gap between their render starts by more
    // than half a refresh.
    std::atomic<uint64_t> hitches{0};
    // The longest one present took on the present thread.
    std::atomic<uint64_t> maxPresentMicros{0};
    // GPU time of the output's own drawing (its slices and test card), summed, and the frames it covers.
    std::atomic<uint64_t> gpuMicros{0};
    std::atomic<uint64_t> gpuFrames{0};
    // The display's refresh rate as its surface last reported it.
    std::atomic<double> refreshHz{0.0};
    double lastShown = 0.0;
    double lastNewShown = 0.0;
    double lastNewStarted = 0.0;
};

// The output of one display, identified by the display's id. Its own present thread, paced by that display's
// refreshes, draws the latest finished frame through its screen's slices, so a slow display never stalls rendering
// or other outputs. While its display is detached the thread parks: it waits, drawing and polling nothing, until the
// display is attached again.
class DisplayOutput {
public:
    // `vsync` is where the main output reports its refreshes to the render clock.
    DisplayOutput(Engine& engine, std::string display, VsyncPhase& vsync);
    ~DisplayOutput();

    // On the thread that owns the displays. Detaching returns once the present thread has parked and the surface is
    // closed.
    void attach(std::unique_ptr<Surface> surface);
    void detach();
    bool attached() const { return presenting_; }

    // Any thread.
    void setScreen(std::shared_ptr<const ScreenView> screen);
    void setMain(bool main) { main_ = main; }
    bool main() const { return main_; }
    // Holds every present back by `ms` milliseconds, to show that a slow output stays local.
    void setDelay(int ms) { delayMs_ = ms; }

    const std::string& display() const { return display_; }
    uint64_t screen() const;
    OutputStats& stats() { return *stats_; }

private:
    void run();
    void present(Surface& surface, std::chrono::steady_clock::time_point vsync);
    void presentFrame(Surface& surface, std::chrono::steady_clock::time_point vsync);

    Engine& engine_;
    const std::string display_;
    VsyncPhase& vsync_;
    ScreenPainter painter_;
    gpu::Queue queue_;
    profile::GpuTimeline gpu_;
    // Shared with presented handlers, which can fire after the output is gone.
    std::shared_ptr<OutputStats> stats_ = std::make_shared<OutputStats>();
    uint64_t lastIndex_ = UINT64_MAX;
    // Present thread only: set while (re)attaching, so frames rendered while parked are not counted as dropped.
    bool resumed_ = true;
    std::atomic<bool> main_{false};
    std::atomic<int> delayMs_{0};

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::shared_ptr<const ScreenView> screen_;
    std::unique_ptr<Surface> surface_;
    std::atomic<bool> presenting_{false};
    bool inRefreshes_ = false;
    bool stopping_ = false;
    std::thread thread_;
};

} // namespace azl
