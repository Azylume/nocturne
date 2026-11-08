// Not tested on Windows yet (ADR 0017, open question 5): whether the task, registered at run level Limited, may be
// enabled and disabled by the engine running as the same user has to be tried on a real machine.
#include "platform/autostart.hpp"

#include "platform/run_command.hpp"

namespace azl {

namespace {

constexpr const char* kTask = "Nocturne";

CommandResult powershell(const std::string& script)
{
    return runCommand("powershell.exe",
                      {"-NoProfile", "-NonInteractive", "-Command", "$ErrorActionPreference='Stop'; " + script}, true);
}

} // namespace

AutostartState autostartState()
{
    const CommandResult result =
        powershell(std::string("(Get-ScheduledTask -TaskName ") + kTask + ").Settings.Enabled");
    if (!result.ended || result.exitCode != 0)
        return AutostartState::Unsupported;
    if (result.output.find("True") != std::string::npos)
        return AutostartState::On;
    return result.output.find("False") != std::string::npos ? AutostartState::Off : AutostartState::Unsupported;
}

std::optional<std::string> setAutostart(bool on)
{
    const CommandResult result =
        powershell(std::string(on ? "Enable" : "Disable") + "-ScheduledTask -TaskName " + kTask + " | Out-Null");
    if (result.ended && result.exitCode == 0)
        return std::nullopt;
    return std::string(on ? "Enable" : "Disable") + "-ScheduledTask failed: " + result.output;
}

} // namespace azl
