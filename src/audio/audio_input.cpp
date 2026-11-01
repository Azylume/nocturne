#include "audio/audio_input.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace azl {

bool containsIgnoringCase(std::string_view text, std::string_view part)
{
    const auto it = std::search(text.begin(), text.end(), part.begin(), part.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != text.end() || part.empty();
}

bool AudioInput::disabled()
{
    const char* value = std::getenv("NOCTURNE_NO_AUDIO_INPUT");
    return value && std::string(value) == "1";
}

AudioChoice startingInput(const AudioChoice& commandLine, const std::optional<ResumeAudio>& saved,
                          const std::vector<AudioDeviceInfo>& devices, std::string& log)
{
    log.clear();
    if (!saved || saved->commandLine != commandLine || saved->open == commandLine)
        return commandLine;
    const std::string& device = saved->open.device;
    const bool named = !device.empty() && device != "off" && device != "system" && !device.starts_with("app:");
    const bool attached = std::any_of(devices.begin(), devices.end(), [&](const AudioDeviceInfo& d) {
        return d.uid == device || containsIgnoringCase(d.name, device);
    });
    if (named && !attached) {
        log = "'" + device + "', open before the restart, is not attached; opening the command line's input";
        return commandLine;
    }
    log = "reopening '" + device + "', the input open before the restart";
    return saved->open;
}

} // namespace azl
