#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace azl {

// Seconds on the steady clock.
inline double traceSeconds(std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double>(t.time_since_epoch()).count();
}

inline double traceNow()
{
    return traceSeconds(std::chrono::steady_clock::now());
}

// A host time (Metal's GPU and presented times, mach_absolute_time in seconds) on the steady clock, which also counts
// the time the machine slept.
double traceFromHost(double hostSeconds);

// A timeline of every rendered frame and every present, kept in memory and written as JSON lines, to see what the
// frames around a hitch waited on. Any thread.
class FrameTrace {
public:
    struct Render {
        uint64_t index = 0;
        double started = 0.0;
        // When the render thread had stepped the clock, Autopilot and triggers, updated the layers' frames and encoded
        // the frame.
        double stepped = 0.0;
        double updated = 0.0;
        double encoded = 0.0;
        double gpuStart = 0.0;
        double gpuEnd = 0.0;
        // When the completion handler published it.
        double done = 0.0;
        uint64_t fading = 0;
    };

    struct Present {
        std::string display;
        double vsync = 0.0;
        double woke = 0.0;
        // After the surface handed out its target.
        double targeted = 0.0;
        double committed = 0.0;
        double gpuStart = 0.0;
        double gpuEnd = 0.0;
        // As the surface reported it.
        double shown = 0.0;
        uint64_t index = 0;
        bool fresh = false;
        double started = 0.0;
    };

    explicit FrameTrace(std::string path) : path_(std::move(path)) {}

    void add(const Render& render);
    void add(const Present& present);
    // Writes everything kept to the file; false when it could not.
    bool write() const;

private:
    static constexpr size_t kMaxRecords = 2'000'000;

    const std::string path_;
    mutable std::mutex mutex_;
    std::vector<Render> renders_;
    std::vector<Present> presents_;
};

} // namespace azl
