#include "platform/thread_priority.hpp"

#include "platform/qos_apple.hpp"

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <sys/resource.h>

#include <cstdint>

namespace azl {

namespace {

uint32_t machTicks(double ms)
{
    mach_timebase_info_data_t base;
    mach_timebase_info(&base);
    return uint32_t(ms * 1e6 * base.denom / base.numer);
}

// Even at the highest QoS a timed wait wakes about 3 ms late, and 20 ms and more on a busy machine; a time-constraint
// thread wakes on time. Each frame thread works a few milliseconds of every 60 Hz refresh.
void makeTimeConstrained()
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

} // namespace

void setThreadRole(ThreadRole role)
{
    pthread_set_qos_class_self_np(qosClass(role), 0);
    if (role == ThreadRole::Idle)
        setiopolicy_np(IOPOL_TYPE_DISK, IOPOL_SCOPE_THREAD, IOPOL_THROTTLE);
    if (role == ThreadRole::Frames && realtimeFrames())
        makeTimeConstrained();
}

} // namespace azl
