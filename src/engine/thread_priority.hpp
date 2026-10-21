#pragma once

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <pthread/qos.h>

#include <atomic>
#include <cstdint>

namespace azl {

// Who wins when the CPU is short: the render and present threads, whose late frames are visible, before the decode
// threads, which hold a second ahead and can wait a few milliseconds, before compiling and opening files.
enum class ThreadRole { Frames, Decode, Background };

namespace detail {

inline uint32_t machTicks(double ms)
{
    mach_timebase_info_data_t base;
    mach_timebase_info(&base);
    return uint32_t(ms * 1e6 * base.denom / base.numer);
}

// Even at the highest QoS a timed wait wakes about 3 ms late, and 20 ms and more on a busy machine; a time-constraint
// thread wakes on time. Each frame thread works a few milliseconds of every 60 Hz refresh.
inline void makeTimeConstrained()
{
    thread_time_constraint_policy_data_t policy{
        .period = machTicks(1000.0 / 60.0),
        .computation = machTicks(4.0),
        .constraint = machTicks(1000.0 / 60.0),
        .preemptible = 1,
    };
    thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                      reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}

} // namespace detail

// On: frame threads run as time-constraint threads, which wake on time but ignore nice and can starve the others.
inline std::atomic<bool>& realtimeFrames()
{
    static std::atomic<bool> on{false};
    return on;
}

// On the calling thread.
inline void setThreadRole(ThreadRole role)
{
    const qos_class_t qos = role == ThreadRole::Frames   ? QOS_CLASS_USER_INTERACTIVE
                            : role == ThreadRole::Decode ? QOS_CLASS_USER_INITIATED
                                                         : QOS_CLASS_UTILITY;
    pthread_set_qos_class_self_np(qos, 0);
    if (role == ThreadRole::Frames && realtimeFrames())
        detail::makeTimeConstrained();
}

} // namespace azl
