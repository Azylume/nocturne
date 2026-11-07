#include "platform/autostart.hpp"

#include "platform/app_paths.hpp"
#include "platform/run_command.hpp"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>

namespace azl {

namespace {

constexpr const char* kLabel = "ai.azylume.nocturne";

// NOCTURNE_LAUNCHCTL names another program, so a test never touches the real launchd.
std::filesystem::path launchctl()
{
    const char* other = std::getenv("NOCTURNE_LAUNCHCTL");
    return other && *other ? other : "/bin/launchctl";
}

std::string domain()
{
    return "gui/" + std::to_string(::getuid());
}

bool installed()
{
    const std::filesystem::path home = homeFolder();
    return !home.empty() && std::filesystem::exists(home / "Library/LaunchAgents" / (std::string(kLabel) + ".plist"));
}

} // namespace

AutostartState autostartState()
{
    if (!installed())
        return AutostartState::Unsupported;
    const CommandResult result = runCommand(launchctl(), {"print-disabled", domain()}, false);
    if (!result.ended || result.exitCode != 0)
        return AutostartState::Unsupported;
    const size_t at = result.output.find(std::string("\"") + kLabel + "\"");
    if (at == std::string::npos)
        return AutostartState::On;
    const std::string line = result.output.substr(at, result.output.find('\n', at) - at);
    return line.find("=> disabled") != std::string::npos || line.find("=> true") != std::string::npos
               ? AutostartState::Off
               : AutostartState::On;
}

std::optional<std::string> setAutostart(bool on)
{
    if (!installed())
        return "autostart is not installed on this machine (tools/autostart.sh install)";
    const CommandResult result =
        runCommand(launchctl(), {on ? "enable" : "disable", domain() + "/" + kLabel}, false);
    if (result.ended && result.exitCode == 0)
        return std::nullopt;
    return "launchctl " + std::string(on ? "enable" : "disable") + " failed: " + result.output;
}

} // namespace azl
