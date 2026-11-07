#pragma once

#include <optional>
#include <string>

namespace azl {

enum class AutostartState { On, Off, Unsupported };

inline const char* autostartName(AutostartState state)
{
    return state == AutostartState::On ? "on" : state == AutostartState::Off ? "off" : "unsupported";
}

// Whether the installed autostart job (the LaunchAgent ai.azylume.nocturne on macOS, the scheduled task Nocturne on
// Windows) is enabled. Unsupported when the job is not installed or the platform has none. Runs a system command, so
// it takes a moment: never on the render thread.
AutostartState autostartState();
// Enables or disables the installed job, never installing or removing it; it does not start or stop the running show.
// The reason it could not, or none. Same threading as autostartState.
std::optional<std::string> setAutostart(bool on);

} // namespace azl
