#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace azl {

// How the frame guard trades sharpness for smoothness. Smooth lowers resolution as soon as the GPU time nears the
// frame budget; Sharp keeps full resolution until frames overrun it.
enum class GuardProfile { Smooth, Sharp };

const char* guardProfileName(GuardProfile profile);
std::optional<GuardProfile> guardProfileFromName(const std::string& name);

// The machine's performance and cache settings, kept in the state folder across restarts and deploys.
struct MachineSettings {
    GuardProfile profile = GuardProfile::Smooth;
    // During a transition the guard lowers resolution before a frame would drop.
    bool protectTransitions = false;
    // The bound on the frames each playing clip holds, and on the frames of clips preloaded for the next trigger.
    uint64_t frameCacheMB = 2048;
    uint64_t preloadMB = 3072;
    // Clip previews kept for a selection back besides the watched ones.
    uint64_t clipPreviews = 8;
    // A chain's static leading passes are drawn once and reused while their inputs stay the same.
    bool bakeStatic = false;
    // The size the composition renders at, in percent of the output's (100, 83 or 67), scaled up once at the output.
    uint64_t renderScale = 100;
    // How many rendered frames may be on the GPU at once (EngineConfig::maxInFlight): 1, 2, or 0 for as many as the
    // frame ring holds. Under GPU overload a bound lets the outputs' presents through sooner, at some render rate.
    uint64_t framesInFlight = 0;

    bool operator==(const MachineSettings&) const = default;
};

// Settings the command line gives, each overriding the kept value on this start.
struct SettingsFlags {
    std::optional<GuardProfile> profile;
    std::optional<bool> protectTransitions;
    std::optional<uint64_t> frameCacheMB;
    std::optional<uint64_t> preloadMB;
    std::optional<uint64_t> clipPreviews;
    std::optional<bool> bakeStatic;
};

// The settings a start runs with, and the flag that overrode each field, by field name.
struct StartingSettings {
    MachineSettings settings;
    std::map<std::string, std::string> overridden;
};

StartingSettings startingSettings(const MachineSettings& kept, const SettingsFlags& flags);

nlohmann::json toJson(const MachineSettings& settings);
// `settings` with the fields `j` names; throws std::invalid_argument naming a field that is out of range.
MachineSettings applied(const nlohmann::json& j, MachineSettings settings);

// `defaults` with what `file` keeps; a file that cannot be read is logged and left out.
MachineSettings loadMachineSettings(const std::filesystem::path& file, MachineSettings defaults = {});
void saveMachineSettings(const std::filesystem::path& file, const MachineSettings& settings);

} // namespace azl
