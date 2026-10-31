#include "platform/thread_priority.hpp"

#include <windows.h>

#include <avrt.h>
#include <timeapi.h>

#include <algorithm>
#include <mutex>
#include <thread>

namespace azl {

namespace {

int threadPriority(ThreadRole role)
{
    switch (role) {
    case ThreadRole::Frames: return THREAD_PRIORITY_HIGHEST;
    case ThreadRole::Decode: return THREAD_PRIORITY_ABOVE_NORMAL;
    case ThreadRole::Background: return THREAD_PRIORITY_BELOW_NORMAL;
    case ThreadRole::Idle: return THREAD_PRIORITY_LOWEST;
    }
    return THREAD_PRIORITY_NORMAL;
}

// The scheduler and every timed wait tick at 1 ms instead of 15.6 ms for the rest of the process's life, so a frame
// thread's wait for the next refresh wakes within a millisecond.
void fineTimerResolution()
{
    static std::once_flag once;
    std::call_once(once, [] { timeBeginPeriod(1); });
}

// MMCSS raises the thread into the real-time range for its slice of every period, the way a time-constraint thread
// does on macOS. Undone by Windows when the thread ends.
void joinMultimediaScheduler()
{
    DWORD task = 0;
    if (HANDLE handle = AvSetMmThreadCharacteristicsW(L"Games", &task))
        AvSetMmThreadPriority(handle, AVRT_PRIORITY_HIGH);
}

// A high-resolution waitable timer for the thread that waits on it, closed when the thread ends.
struct WaitTimer {
    HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    ~WaitTimer()
    {
        if (handle)
            CloseHandle(handle);
    }
};

// False when the timer could not be set: the caller waits another way.
bool waitOn(HANDLE timer, std::chrono::steady_clock::duration span)
{
    using Ticks = std::chrono::duration<LONGLONG, std::ratio<1, 10'000'000>>;
    LARGE_INTEGER due;
    // Negative: relative to now.
    due.QuadPart = -std::max<LONGLONG>(1, std::chrono::duration_cast<Ticks>(span).count());
    return timer && SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0) &&
           WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0;
}

} // namespace

void setThreadRole(ThreadRole role)
{
    SetThreadPriority(GetCurrentThread(), threadPriority(role));
    if (role == ThreadRole::Idle)
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    if (role != ThreadRole::Frames)
        return;
    fineTimerResolution();
    if (realtimeFrames())
        joinMultimediaScheduler();
}

void sleepUntil(std::chrono::steady_clock::time_point deadline)
{
    thread_local const WaitTimer timer;
    const auto span = deadline - std::chrono::steady_clock::now();
    if (span.count() <= 0)
        return;
    if (!waitOn(timer.handle, span))
        std::this_thread::sleep_until(deadline);
}

} // namespace azl
