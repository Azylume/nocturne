#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace azl {

// What the Supervisor starts the Engine with besides its launch flags, kept in the state folder as machine-settings.json.
// An empty field sets nothing, so the launch flag (or the Engine's default) stays.
struct StartSettings {
    // The show file.
    std::string composition;
    std::string mediaRoot;
    std::string audioInput;

    bool operator==(const StartSettings&) const = default;
};

constexpr const char* kStartSettingsFile = "machine-settings.json";
constexpr size_t kMaxSettingPath = 1024;
constexpr size_t kMaxAudioInput = 256;

// Why `settings` may not be saved: a path that is relative, has a ".." part, is longer than kMaxSettingPath, holds a
// control character, or lies outside ~/Nocturne/shows (composition) or ~/Nocturne/media (mediaRoot) once symbolic
// links are resolved; an audio input that is too long or holds a control character. None when it may.
std::optional<std::string> startSettingsRefusal(const StartSettings& settings);

// Nothing for a missing file; throws std::runtime_error naming what is wrong with one that cannot be read, is not a
// JSON object, has a field that is not a string, or fails startSettingsRefusal.
std::optional<StartSettings> readStartSettings(const std::filesystem::path& file);
// Atomically; throws std::runtime_error.
void writeStartSettings(const std::filesystem::path& file, const StartSettings& settings);

// The engine flags for the fields that are set: --composition, --media-root and --audio-input, each with its value.
std::vector<std::string> startSettingsFlags(const StartSettings& settings);

} // namespace azl
