#pragma once

#include <atomic>
#include <chrono>

namespace azl {

// When the display the render clock locks to last refreshed, written by that display's output.
class VsyncPhase {
public:
    using Clock = std::chrono::steady_clock;

    void refreshed(Clock::time_point at, Clock::duration period)
    {
        period_ = period.count();
        last_ = at.time_since_epoch().count();
    }

    Clock::time_point last() const { return Clock::time_point(Clock::duration(last_.load())); }
    Clock::duration period() const { return Clock::duration(period_.load()); }

private:
    std::atomic<Clock::rep> last_{0};
    std::atomic<Clock::rep> period_{0};
};

} // namespace azl
