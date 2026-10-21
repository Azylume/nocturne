#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace azl::app {

// Every flag of the engine, as the command line or a saved configuration sets it.
struct EngineConfig {
    EngineConfig();

    std::string display;
    std::string main;
    bool windowed = false;
    int virtualDisplays = 0;
    bool offscreen = false;
    bool listDisplays = false;
    std::string slowDisplay;
    int slowMs = 0;
    double seconds = 0.0;
    double fps = 60.0;
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint16_t port;
    std::string library;
    // Unset: what the state keeps, else the engine's default.
    std::optional<size_t> clipCacheMB;
    std::optional<size_t> preloadMB;
    std::optional<size_t> keptPreviews;
    std::string profile;
    std::optional<bool> protectTransitions;
    std::optional<bool> bakeStatic;
    bool clipPreviews = false;
    uint32_t seed;
    std::string presets;
    std::string macroSets;
    std::string statsJson;
    std::string frameTrace;
    bool realtimeFrames = false;
    bool debugFrameBits = false;
    bool opaqueOutputs = false;
    bool gpuProfile = false;
    bool frameGuard = true;
    bool fsrUpscale = false;
    std::string audioInput;
    std::vector<int> audioChannels;
    std::string audioLog;
    std::string audioGain = "auto";
    double audioLatencyMs = 0.0;
    bool micCheck = false;
    std::string state;
    std::string composition;
    std::string mediaRoot;
    std::string safeMode;
    std::string fallbackImage;
    bool node = false;
    std::string follow;
    std::string name;
    // Unset: what the state keeps, else the recorder's default.
    std::string recordPath;
    std::optional<uint64_t> recordReserveMB;
    std::optional<double> recordReservePercent;
};

extern const char* const kEngineUsage;

// The command line's configuration, or a request for the usage: --help, or a flag it does not know or cannot read.
struct ParsedArgs {
    enum class Outcome { Run, Help, Error };
    Outcome outcome = Outcome::Run;
    EngineConfig config;
};

// The arguments after the program name.
ParsedArgs parseEngineArgs(const std::vector<std::string>& args);

// The saved configuration beside the state in `stateDir`.
std::filesystem::path engineConfigFile(const std::filesystem::path& stateDir);

// Unknown keys are ignored and missing ones keep their defaults; throws on a file that cannot be read or parsed.
EngineConfig loadEngineConfig(const std::filesystem::path& file);
void saveEngineConfig(const std::filesystem::path& file, const EngineConfig& config);

} // namespace azl::app
