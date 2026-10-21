#include "engine/render_thread.hpp"

#include "engine/thread_priority.hpp"
#include "platform/profile.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>

namespace azl {

namespace {

using Clock = std::chrono::steady_clock;

const Clock::time_point kEpoch = Clock::now();

double secondsSinceEpoch(Clock::time_point t)
{
    return std::chrono::duration<double>(t - kEpoch).count();
}

// Sleeps most of the period and yields through the last stretch for sub-millisecond accuracy. A late frame
// re-anchors the schedule instead of bursting to catch up. With a vsync phase it keeps its own period but drifts each
// tick to start just after a refresh, so a finished frame never lands on the edge where an output picks it up.
class FrameClock {
public:
    FrameClock(double fps, const VsyncPhase* vsync)
        : period_(std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / fps))),
          vsync_(vsync)
    {
    }

    Clock::time_point wait()
    {
        next_ += period_ + phaseCorrection(next_ + period_);
        const auto now = Clock::now();
        if (now >= next_) {
            next_ = now;
            return now;
        }
        std::this_thread::sleep_until(next_ - kSpinMargin);
        while (Clock::now() < next_)
            std::this_thread::yield();
        return next_;
    }

private:
    Clock::duration phaseCorrection(Clock::time_point tick)
    {
        if (!vsync_)
            return {};
        const Clock::duration refresh = vsync_->period();
        const Clock::time_point last = vsync_->last();
        if (refresh.count() <= 0 || Clock::now() - last > kStale)
            return {};
        const double ratio = double(period_.count()) / double(refresh.count());
        if (ratio < 0.98 || std::abs(ratio - std::round(ratio)) > 0.02)
            return {};
        const Clock::time_point target = last + kLead;
        if (target_ && std::chrono::abs(wrap((target - *target_) % refresh, refresh)) > kTrack)
            later_ = true;
        target_ = target;
        auto error = wrap((tick - target) % refresh, refresh);
        if (later_ && error > kTrack)
            error -= refresh;
        if (std::chrono::abs(error) <= kTrack)
            later_ = false;
        return -error / 8;
    }

    static Clock::duration wrap(Clock::duration d, Clock::duration refresh)
    {
        if (d > refresh / 2)
            return d - refresh;
        if (d < -refresh / 2)
            return d + refresh;
        return d;
    }

    static constexpr auto kSpinMargin = std::chrono::microseconds(1500);
    static constexpr auto kLead = std::chrono::microseconds(500);
    // When the main display's refreshes move by more than this, as when it comes back, frames start later until they
    // are in phase again: starting them sooner would render faster than the other displays refresh, so each of them
    // would fall a frame behind, until it drops one.
    static constexpr auto kTrack = std::chrono::microseconds(2000);
    std::optional<Clock::time_point> target_;
    bool later_ = false;
    static constexpr auto kStale = std::chrono::milliseconds(500);
    Clock::duration period_;
    const VsyncPhase* vsync_;
    Clock::time_point next_ = Clock::now();
};

} // namespace

double engineTime()
{
    return secondsSinceEpoch(Clock::now());
}

RenderThread::RenderThread(Engine& engine, double fps, const VsyncPhase* vsync)
    : engine_(engine), thread_([this, fps, vsync] {
          setThreadRole(ThreadRole::Frames);
          AZL_THREAD_NAME("render");
          run(fps, vsync);
      })
{
}

RenderThread::~RenderThread()
{
    running_ = false;
    thread_.join();
    engine_.waitUntilIdle();
}

void RenderThread::run(double fps, const VsyncPhase* vsync)
{
    FrameClock clock(fps, vsync);
    std::optional<Clock::time_point> last;
    while (running_) {
        const Clock::time_point start = clock.wait();
        if (last) {
            const auto gap = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(start - *last).count());
            auto& longest = engine_.stats().maxGapMicros;
            longest = std::max(longest.load(), gap);
        }
        last = start;
        engine_.renderFrame(secondsSinceEpoch(start));
    }
}

} // namespace azl
