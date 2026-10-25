#include "engine/machine_settings.hpp"

#include "document/composition_file.hpp"

#include <cstdio>
#include <stdexcept>

namespace azl {

namespace {

uint64_t limited(const nlohmann::json& j, const char* field, uint64_t low, uint64_t high)
{
    const nlohmann::json& v = j[field];
    if (!v.is_number() || v.get<double>() < double(low) || v.get<double>() > double(high))
        throw std::invalid_argument(std::string(field) + " must be a number from " + std::to_string(low) + " to " +
                                    std::to_string(high));
    return v.get<uint64_t>();
}

} // namespace

const char* guardProfileName(GuardProfile profile)
{
    return profile == GuardProfile::Sharp ? "sharp" : "smooth";
}

std::optional<GuardProfile> guardProfileFromName(const std::string& name)
{
    if (name == "smooth")
        return GuardProfile::Smooth;
    if (name == "sharp")
        return GuardProfile::Sharp;
    return std::nullopt;
}

StartingSettings startingSettings(const MachineSettings& kept, const SettingsFlags& flags)
{
    StartingSettings start{kept, {}};
    MachineSettings& s = start.settings;
    if (flags.profile) {
        s.profile = *flags.profile;
        start.overridden["profile"] = std::string("--profile ") + guardProfileName(s.profile);
    }
    if (flags.protectTransitions) {
        s.protectTransitions = *flags.protectTransitions;
        start.overridden["protectTransitions"] =
            std::string("--protect-transitions ") + (s.protectTransitions ? "on" : "off");
    }
    if (flags.frameCacheMB) {
        s.frameCacheMB = *flags.frameCacheMB;
        start.overridden["frameCacheMB"] = "--clip-cache " + std::to_string(s.frameCacheMB);
    }
    if (flags.preloadMB) {
        s.preloadMB = *flags.preloadMB;
        start.overridden["preloadMB"] = "--preload " + std::to_string(s.preloadMB);
    }
    if (flags.clipPreviews) {
        s.clipPreviews = *flags.clipPreviews;
        start.overridden["clipPreviews"] = "--kept-previews " + std::to_string(s.clipPreviews);
    }
    if (flags.bakeStatic) {
        s.bakeStatic = *flags.bakeStatic;
        start.overridden["bakeStatic"] = std::string("--bake-static ") + (s.bakeStatic ? "on" : "off");
    }
    return start;
}

nlohmann::json toJson(const MachineSettings& settings)
{
    return {{"profile", guardProfileName(settings.profile)},
            {"protectTransitions", settings.protectTransitions},
            {"frameCacheMB", settings.frameCacheMB},
            {"preloadMB", settings.preloadMB},
            {"clipPreviews", settings.clipPreviews},
            {"bakeStatic", settings.bakeStatic},
            {"renderScale", settings.renderScale},
            {"framesInFlight", settings.framesInFlight}};
}

MachineSettings applied(const nlohmann::json& j, MachineSettings settings)
{
    if (j.contains("profile")) {
        const std::optional<GuardProfile> profile =
            j["profile"].is_string() ? guardProfileFromName(j["profile"].get<std::string>()) : std::nullopt;
        if (!profile)
            throw std::invalid_argument("profile must be smooth or sharp");
        settings.profile = *profile;
    }
    if (j.contains("protectTransitions")) {
        if (!j["protectTransitions"].is_boolean())
            throw std::invalid_argument("protectTransitions must be true or false");
        settings.protectTransitions = j["protectTransitions"].get<bool>();
    }
    if (j.contains("bakeStatic")) {
        if (!j["bakeStatic"].is_boolean())
            throw std::invalid_argument("bakeStatic must be true or false");
        settings.bakeStatic = j["bakeStatic"].get<bool>();
    }
    if (j.contains("renderScale")) {
        const nlohmann::json& v = j["renderScale"];
        if (!v.is_number_integer() || (v != 100 && v != 83 && v != 67))
            throw std::invalid_argument("renderScale must be 100, 83 or 67");
        settings.renderScale = v.get<uint64_t>();
    }
    if (j.contains("framesInFlight"))
        settings.framesInFlight = limited(j, "framesInFlight", 0, 2);
    if (j.contains("frameCacheMB"))
        settings.frameCacheMB = limited(j, "frameCacheMB", 64, 65536);
    if (j.contains("preloadMB"))
        settings.preloadMB = limited(j, "preloadMB", 0, 65536);
    if (j.contains("clipPreviews"))
        settings.clipPreviews = limited(j, "clipPreviews", 0, 64);
    return settings;
}

MachineSettings loadMachineSettings(const std::filesystem::path& file, MachineSettings defaults)
{
    if (file.empty())
        return defaults;
    try {
        if (const std::optional<nlohmann::json> kept = readJsonFile(file))
            return applied(*kept, defaults);
    } catch (const std::exception& e) {
        std::printf("settings: %s: %s; using the defaults\n", file.string().c_str(), e.what());
    }
    return defaults;
}

void saveMachineSettings(const std::filesystem::path& file, const MachineSettings& settings)
{
    if (file.empty())
        return;
    try {
        writeFileAtomically(file, toJson(settings).dump(2) + "\n", false);
    } catch (const std::exception& e) {
        std::printf("settings: cannot keep them in %s: %s\n", file.string().c_str(), e.what());
    }
}

} // namespace azl
