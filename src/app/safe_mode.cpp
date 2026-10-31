#include "app/safe_mode.hpp"

#include "app/main_thread.hpp"
#include "supervisor/restart_request.hpp"

#include <chrono>
#include <cstdlib>

namespace azl {

namespace {

// Long enough for the reply to go out before the engine quits.
constexpr std::chrono::milliseconds kReplyMoment{200};

} // namespace

MachineActions::Restart restartRequest(int* exitCode, void (*quit)())
{
    if (!std::getenv(restart::kSupervisedVariable))
        return nullptr;
    return [exitCode, quit](bool safeMode) {
        const int code = safeMode ? restart::kSafeMode : restart::kNormal;
        app::onMainAfter(kReplyMoment, [exitCode, quit, code] {
            *exitCode = code;
            quit();
        });
    };
}

MachineActions::Stop stopRequest(int* exitCode, void (*quit)())
{
    if (!std::getenv(restart::kSupervisedVariable))
        return nullptr;
    return [exitCode, quit] {
        app::onMainAfter(kReplyMoment, [exitCode, quit] {
            *exitCode = restart::kStop;
            quit();
        });
    };
}

} // namespace azl
