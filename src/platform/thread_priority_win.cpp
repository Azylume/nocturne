#include "platform/thread_priority.hpp"

#include <windows.h>

#include <avrt.h>
#include <timeapi.h>

#include <mutex>

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

} // namespace azl
