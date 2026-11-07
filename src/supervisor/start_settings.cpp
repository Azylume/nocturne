#include "supervisor/start_settings.hpp"

#include "document/composition_file.hpp"
#include "platform/app_paths.hpp"
#include "protocol/remote_files.hpp"

#include <algorithm>
#include <stdexcept>

namespace azl {

namespace {

bool hasControl(const std::string& text)
{
    return std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

std::optional<std::string> pathProblem(const char* field, const std::string& path, const char* folder)
{
    if (path.empty())
        return std::nullopt;
    if (path.size() > kMaxSettingPath || hasControl(path))
        return std::string(field) + " is not a usable path";
    const std::filesystem::path home = homeFolder();
    if (home.empty())
        return std::string(field) + " cannot be checked: this machine has no home folder";
    if (const std::optional<std::string> refused = pathRefusal(path, {home / "Nocturne" / folder}))
        return std::string(field) + ": " + *refused;
    return std::nullopt;
}

std::string text(const nlohmann::json& j, const char* field)
{
    const auto found = j.find(field);
    if (found == j.end())
        return {};
    if (!found->is_string())
        throw std::runtime_error(std::string(field) + " must be a string");
    return found->get<std::string>();
}

} // namespace

std::optional<std::string> startSettingsRefusal(const StartSettings& settings)
{
    if (const auto refused = pathProblem("composition", settings.composition, "shows"))
        return refused;
    if (const auto refused = pathProblem("mediaRoot", settings.mediaRoot, "media"))
        return refused;
    if (settings.audioInput.size() > kMaxAudioInput || hasControl(settings.audioInput))
        return "audioInput is not a usable device name";
    return std::nullopt;
}

std::optional<StartSettings> readStartSettings(const std::filesystem::path& file)
{
    std::optional<nlohmann::json> j;
    try {
        j = readJsonFile(file);
    } catch (const std::exception& e) {
        throw std::runtime_error(file.string() + ": " + e.what());
    }
    if (!j)
        return std::nullopt;
    if (!j->is_object())
        throw std::runtime_error(file.string() + ": not a JSON object");
    StartSettings settings{text(*j, "composition"), text(*j, "mediaRoot"), text(*j, "audioInput")};
    if (const std::optional<std::string> refused = startSettingsRefusal(settings))
        throw std::runtime_error(*refused);
    return settings;
}

void writeStartSettings(const std::filesystem::path& file, const StartSettings& settings)
{
    const nlohmann::json j{{"composition", settings.composition},
                           {"mediaRoot", settings.mediaRoot},
                           {"audioInput", settings.audioInput}};
    writeFileAtomically(file, j.dump(2) + "\n", false);
}

std::vector<std::string> startSettingsFlags(const StartSettings& settings)
{
    std::vector<std::string> flags;
    for (const auto& [flag, value] : {std::pair{"--composition", &settings.composition},
                                      std::pair{"--media-root", &settings.mediaRoot},
                                      std::pair{"--audio-input", &settings.audioInput}})
        if (!value->empty())
            flags.insert(flags.end(), {flag, *value});
    return flags;
}

} // namespace azl
