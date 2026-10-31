#include <doctest.h>

#include "platform/thread_priority.hpp"

#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread/qos.h>
#include <sys/resource.h>
#endif

namespace {

// Runs the check on a fresh thread after giving it the role.
template <class Check>
bool onThreadWithRole(azl::ThreadRole role, Check check)
{
    bool passed = false;
    std::thread([&] {
        azl::setThreadRole(role);
        passed = check();
    }).join();
    return passed;
}

} // namespace

TEST_CASE("platform: each thread role sets the calling thread's priority")
{
    using azl::ThreadRole;
#ifdef _WIN32
    const std::vector<std::pair<ThreadRole, int>> expected{{ThreadRole::Frames, THREAD_PRIORITY_HIGHEST},
                                                           {ThreadRole::Decode, THREAD_PRIORITY_ABOVE_NORMAL},
                                                           {ThreadRole::Background, THREAD_PRIORITY_BELOW_NORMAL}};
    const auto current = [] { return GetThreadPriority(GetCurrentThread()); };
    // A thread already in background mode cannot enter it again.
    const auto idle = [] {
        return !SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) &&
               GetLastError() == ERROR_THREAD_MODE_ALREADY_BACKGROUND;
    };
#else
    const std::vector<std::pair<ThreadRole, qos_class_t>> expected{{ThreadRole::Frames, QOS_CLASS_USER_INTERACTIVE},
                                                                   {ThreadRole::Decode, QOS_CLASS_USER_INITIATED},
                                                                   {ThreadRole::Background, QOS_CLASS_UTILITY}};
    const auto current = [] { return qos_class_self(); };
    const auto idle = [] { return getiopolicy_np(IOPOL_TYPE_DISK, IOPOL_SCOPE_THREAD) == IOPOL_THROTTLE; };
#endif
    for (const auto& [role, priority] : expected)
        CHECK_MESSAGE(onThreadWithRole(role, [&, p = priority] { return current() == p; }), "role " << int(role));
    CHECK(onThreadWithRole(ThreadRole::Idle, idle));
}
