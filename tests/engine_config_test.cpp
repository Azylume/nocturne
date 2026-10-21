#include "app/engine_config.hpp"
#include "test_client.hpp"

#include <doctest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using azl::app::EngineConfig;
using azl::app::ParsedArgs;
using Outcome = azl::app::ParsedArgs::Outcome;

namespace {

EngineConfig parsed(const std::vector<std::string>& args)
{
    const ParsedArgs result = azl::app::parseEngineArgs(args);
    REQUIRE(result.outcome == Outcome::Run);
    return result.config;
}

Outcome outcome(const std::vector<std::string>& args)
{
    return azl::app::parseEngineArgs(args).outcome;
}

std::string readFile(const fs::path& file)
{
    std::ifstream in(file);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

} // namespace

TEST_CASE("engine config: no flags give the defaults")
{
    const EngineConfig c = parsed({});
    CHECK(c.fps == 60.0);
    CHECK(c.width == 1920);
    CHECK(c.height == 1080);
    CHECK(c.port == 7400);
    CHECK_FALSE(c.clipCacheMB);
    CHECK_FALSE(c.preloadMB);
    CHECK_FALSE(c.keptPreviews);
    CHECK(c.profile.empty());
    CHECK_FALSE(c.protectTransitions);
    CHECK(c.frameGuard);
    CHECK_FALSE(c.fsrUpscale);
    CHECK(c.audioGain == "auto");
    CHECK(c.library.ends_with("Nocturne/MediaLibrary"));
    CHECK(c.presets.ends_with("Nocturne/Presets.json"));
    CHECK(c.macroSets.ends_with("Nocturne/MacroSets.json"));
    CHECK_FALSE(c.offscreen);
    CHECK_FALSE(c.realtimeFrames);
    CHECK_FALSE(c.opaqueOutputs);
    CHECK(c.state.empty());
    CHECK(c.recordPath.empty());
    CHECK_FALSE(c.recordReserveMB);
    CHECK_FALSE(c.recordReservePercent);
}

TEST_CASE("engine config: the recording folder and reserve, by flags and in the saved file")
{
    const EngineConfig c =
        parsed({"--record-path", "/rec", "--record-reserve", "4096", "--record-reserve-percent", "2.5"});
    CHECK(c.recordPath == "/rec");
    CHECK(c.recordReserveMB == 4096);
    CHECK(c.recordReservePercent == 2.5);
    CHECK(outcome({"--record-reserve"}) == Outcome::Error);
    CHECK(outcome({"--record-path"}) == Outcome::Error);
    TempDir dir;
    const fs::path file = dir.path / "engine.json";
    std::ofstream(file) << R"({"recordReserveMB": 512, "recordReservePercent": 0})";
    const EngineConfig loaded = azl::app::loadEngineConfig(file);
    CHECK(loaded.recordReserveMB == 512);
    CHECK(loaded.recordReservePercent == 0.0);
    azl::app::saveEngineConfig(file, c);
    CHECK(azl::app::loadEngineConfig(file).recordReserveMB == 4096);
    CHECK(azl::app::loadEngineConfig(file).recordPath == "/rec");
}

TEST_CASE("engine config: display and output flags")
{
    const EngineConfig c = parsed({"--display", "d1", "--main", "d2", "--displays", "--windowed", "--virtual", "3",
                                   "--offscreen", "--slow", "window-2:40", "--debug-frame-bits", "--opaque-outputs"});
    CHECK(c.display == "d1");
    CHECK(c.main == "d2");
    CHECK(c.listDisplays);
    CHECK(c.windowed);
    CHECK(c.virtualDisplays == 3);
    CHECK(c.offscreen);
    CHECK(c.slowDisplay == "window-2");
    CHECK(c.slowMs == 40);
    CHECK(c.debugFrameBits);
    CHECK(c.opaqueOutputs);
    CHECK(parsed({"--slow", "10ac:43b0:12"}).slowDisplay == "10ac:43b0");
}

TEST_CASE("engine config: render flags")
{
    const EngineConfig c = parsed({"--seconds", "2.5", "--fps", "30", "--size", "1280x720", "--clip-cache", "512",
                                   "--preload", "256", "--seed", "4000000000", "--clip-previews", "--stats-json",
                                   "s.json", "--frame-trace", "t.json", "--realtime-frames", "--gpu-profile",
                                   "--no-frame-guard", "--fsr-upscale"});
    CHECK(c.seconds == 2.5);
    CHECK(c.fps == 30.0);
    CHECK(c.width == 1280);
    CHECK(c.height == 720);
    CHECK(c.clipCacheMB == 512);
    CHECK(c.preloadMB == 256);
    CHECK(c.seed == 4000000000u);
    CHECK(c.clipPreviews);
    CHECK(c.statsJson == "s.json");
    CHECK(c.frameTrace == "t.json");
    CHECK(c.realtimeFrames);
    CHECK(c.gpuProfile);
    CHECK_FALSE(c.frameGuard);
    CHECK(c.fsrUpscale);
    TempDir dir;
    const fs::path file = dir.path / "engine.json";
    azl::app::saveEngineConfig(file, c);
    CHECK(azl::app::loadEngineConfig(file).fsrUpscale);
}

TEST_CASE("engine config: the profile, protect-transitions and kept previews, by flags and in the saved file")
{
    const EngineConfig c =
        parsed({"--profile", "sharp", "--protect-transitions", "on", "--kept-previews", "3", "--preload", "0"});
    CHECK(c.profile == "sharp");
    CHECK(c.protectTransitions == true);
    CHECK(c.keptPreviews == 3);
    CHECK(c.preloadMB == 0);
    CHECK(parsed({"--protect-transitions", "off"}).protectTransitions == false);
    CHECK(outcome({"--profile", "fast"}) == Outcome::Error);
    CHECK(outcome({"--protect-transitions", "yes"}) == Outcome::Error);
    CHECK(outcome({"--kept-previews"}) == Outcome::Error);
    TempDir dir;
    const fs::path file = dir.path / "engine.json";
    azl::app::saveEngineConfig(file, c);
    const EngineConfig back = azl::app::loadEngineConfig(file);
    CHECK(back.profile == "sharp");
    CHECK(back.protectTransitions == true);
    CHECK(back.keptPreviews == 3);
    CHECK(back.preloadMB == 0);
    CHECK_FALSE(back.clipCacheMB);
    CHECK_FALSE(back.bakeStatic);

    CHECK_FALSE(parsed({}).bakeStatic);
    CHECK(parsed({"--bake-static", "on"}).bakeStatic == true);
    CHECK(parsed({"--bake-static", "off"}).bakeStatic == false);
    CHECK(outcome({"--bake-static", "yes"}) == Outcome::Error);
    azl::app::saveEngineConfig(file, parsed({"--bake-static", "on"}));
    CHECK(azl::app::loadEngineConfig(file).bakeStatic == true);
}

TEST_CASE("engine config: audio flags")
{
    const EngineConfig c = parsed({"--audio-input", "app:Music", "--audio-channels", "1,2", "--audio-channels", "4",
                                   "--audio-gain", "-6", "--audio-latency", "120", "--audio-log", "a.jsonl",
                                   "--mic-check"});
    CHECK(c.audioInput == "app:Music");
    CHECK(c.audioChannels == std::vector<int>{1, 2, 4});
    CHECK(c.audioGain == "-6");
    CHECK(c.audioLatencyMs == 120.0);
    CHECK(c.audioLog == "a.jsonl");
    CHECK(c.micCheck);
}

TEST_CASE("engine config: file and folder flags")
{
    const EngineConfig c = parsed({"--library", "lib", "--presets", "p.json", "--macro-sets", "m.json", "--state",
                                   "st", "--composition", "show.json", "--media-root", "media", "--safe-mode",
                                   "crashed", "--fallback-image", "f.png"});
    CHECK(c.library == "lib");
    CHECK(c.presets == "p.json");
    CHECK(c.macroSets == "m.json");
    CHECK(c.state == "st");
    CHECK(c.composition == "show.json");
    CHECK(c.mediaRoot == "media");
    CHECK(c.safeMode == "crashed");
    CHECK(c.fallbackImage == "f.png");
}

TEST_CASE("engine config: network flags, and --follow makes a node")
{
    const EngineConfig c = parsed({"--port", "7500", "--name", "left"});
    CHECK(c.port == 7500);
    CHECK(c.name == "left");
    CHECK_FALSE(c.node);
    CHECK(parsed({"--node"}).node);
    const EngineConfig follower = parsed({"--follow", "host:7500"});
    CHECK(follower.node);
    CHECK(follower.follow == "host:7500");
}

TEST_CASE("engine config: --help, and flags it cannot read, ask for the usage at the first such flag")
{
    CHECK(outcome({"--help"}) == Outcome::Help);
    CHECK(outcome({"-h"}) == Outcome::Help);
    CHECK(outcome({"--help", "--bogus"}) == Outcome::Help);
    CHECK(outcome({"--bogus", "--help"}) == Outcome::Error);
    CHECK(outcome({"--state"}) == Outcome::Error);
    CHECK(outcome({"--slow", "nocolon"}) == Outcome::Error);
    CHECK(outcome({"--size", "big"}) == Outcome::Error);
    CHECK(outcome({"offscreen"}) == Outcome::Error);
}

TEST_CASE("engine config: the usage text is unchanged")
{
    const char* dir = std::getenv("NOCTURNE_GOLDEN_DIR");
    const fs::path golden = fs::path(dir ? dir : NOCTURNE_GOLDEN_DIR) / "engine_usage.txt";
    CHECK(std::string(azl::app::kEngineUsage) == readFile(golden));
}

TEST_CASE("engine config: saved beside the state and loaded back the same")
{
    TempDir dir;
    const EngineConfig c = parsed({"--offscreen", "--virtual", "2", "--audio-channels", "1,2", "--seed", "7", "--port",
                                   "7500", "--clip-cache", "100", "--audio-latency", "35.5", "--follow", "m"});
    const fs::path file = azl::app::engineConfigFile(dir.path);
    CHECK(file == dir.path / "engine.json");
    azl::app::saveEngineConfig(file, c);
    const EngineConfig back = azl::app::loadEngineConfig(file);
    azl::app::saveEngineConfig(dir.path / "again.json", back);
    CHECK(readFile(dir.path / "again.json") == readFile(file));
    CHECK(back.audioChannels == std::vector<int>{1, 2});
    CHECK(back.seed == 7);
    CHECK(back.offscreen);
}

TEST_CASE("engine config: a saved file ignores keys it does not know and defaults the ones it lacks")
{
    TempDir dir;
    const fs::path file = dir.path / "engine.json";
    std::ofstream(file) << R"({"fps": 50, "somethingNew": [1, 2], "offscreen": true})";
    const EngineConfig c = azl::app::loadEngineConfig(file);
    CHECK(c.fps == 50.0);
    CHECK(c.offscreen);
    CHECK(c.width == 1920);
    CHECK(c.port == 7400);
    CHECK(c.frameGuard);
    CHECK(c.audioGain == "auto");
    CHECK_THROWS(azl::app::loadEngineConfig(dir.path / "missing.json"));
}
