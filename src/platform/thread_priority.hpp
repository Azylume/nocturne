#pragma once

#include <atomic>
#include <chrono>

namespace azl {

// Who wins when the CPU is short: the render and present threads, whose late frames are visible, before the decode
// threads, which hold a second ahead and can wait a few milliseconds, before compiling and opening files, before work
// nobody waits for (Idle, whose disk reads and writes also yield to everyone else's).
enum class ThreadRole { Frames, Decode, Background, Idle };

// On: frame threads run as time-constraint threads (MMCSS on Windows), which wake on time but can starve the others.
inline std::atomic<bool>& realtimeFrames()
{
    static std::atomic<bool> on{false};
    return on;
}

// On the calling thread. Never blocks.
void setThreadRole(ThreadRole role);

// Blocks the calling thread until `deadline`. The frame threads' timed wait: on Windows a high-resolution waitable
// timer, which wakes within a fraction of a millisecond where a plain timed wait wakes up to a scheduler tick late.
void sleepUntil(std::chrono::steady_clock::time_point deadline);

} // namespace azl
