#include "audio/audio_device_settings.hpp"
#include "engine/machine_settings.hpp"
#include "test_client.hpp"

#include <doctest.h>

#include <fstream>

using azl::GuardProfile;
using azl::MachineSettings;

TEST_CASE("machine settings: kept in a file, defaults for what it lacks, a bad file ignored")
{
    TempDir dir;
    const fs::path file = dir.path / "settings.json";
    CHECK(azl::loadMachineSettings(file) == MachineSettings{});
    const MachineSettings kept{GuardProfile::Sharp, true, 512, 256, 2};
    azl::saveMachineSettings(file, kept);
    CHECK(azl::loadMachineSettings(file) == kept);

    std::ofstream(file) << R"({"profile": "sharp", "somethingNew": 1})";
    const MachineSettings partial = azl::loadMachineSettings(file);
    CHECK(partial.profile == GuardProfile::Sharp);
    CHECK(partial.frameCacheMB == MachineSettings{}.frameCacheMB);

    std::ofstream(file) << "{not json";
    CHECK(azl::loadMachineSettings(file) == MachineSettings{});
    std::ofstream(file) << R"({"profile": "fast"})";
    CHECK(azl::loadMachineSettings(file) == MachineSettings{});
    CHECK(azl::loadMachineSettings({}) == MachineSettings{});
}

TEST_CASE("machine settings: fields are validated")
{
    const MachineSettings s;
    CHECK(azl::applied({{"profile", "sharp"}}, s).profile == GuardProfile::Sharp);
    CHECK(azl::applied({{"frameCacheMB", 64}}, s).frameCacheMB == 64);
    CHECK(azl::applied({{"preloadMB", 0}}, s).preloadMB == 0);
    CHECK_THROWS_AS(azl::applied({{"profile", "fast"}}, s), std::invalid_argument);
    CHECK_THROWS_AS(azl::applied({{"protectTransitions", "on"}}, s), std::invalid_argument);
    CHECK_THROWS_AS(azl::applied({{"frameCacheMB", 10}}, s), std::invalid_argument);
    CHECK_THROWS_AS(azl::applied({{"preloadMB", -1}}, s), std::invalid_argument);
    CHECK_THROWS_AS(azl::applied({{"clipPreviews", 100}}, s), std::invalid_argument);
    CHECK_THROWS_AS(azl::applied({{"clipPreviews", "4"}}, s), std::invalid_argument);
    CHECK(s.renderScale == 100);
    CHECK(azl::applied({{"renderScale", 83}}, s).renderScale == 83);
    CHECK(azl::applied({{"renderScale", 67}}, s).renderScale == 67);
    CHECK_THROWS_AS(azl::applied({{"renderScale", 50}}, s), std::invalid_argument);
    CHECK_THROWS_AS(azl::applied({{"renderScale", 83.5}}, s), std::invalid_argument);
    CHECK_THROWS_AS(azl::applied({{"renderScale", "83"}}, s), std::invalid_argument);
}

TEST_CASE("machine settings: a flag overrides the kept value and is named as the override")
{
    const MachineSettings kept{GuardProfile::Sharp, true, 512, 256, 2};
    const azl::StartingSettings none = azl::startingSettings(kept, {});
    CHECK(none.settings == kept);
    CHECK(none.overridden.empty());

    const azl::StartingSettings flagged =
        azl::startingSettings(kept, {.profile = GuardProfile::Smooth, .protectTransitions = false, .preloadMB = 1024});
    CHECK(flagged.settings.profile == GuardProfile::Smooth);
    CHECK_FALSE(flagged.settings.protectTransitions);
    CHECK(flagged.settings.preloadMB == 1024);
    CHECK(flagged.settings.frameCacheMB == 512);
    CHECK(flagged.overridden.size() == 3);
    CHECK(flagged.overridden.at("profile") == "--profile smooth");
    CHECK(flagged.overridden.at("protectTransitions") == "--protect-transitions off");
    CHECK(flagged.overridden.at("preloadMB") == "--preload 1024");

    const azl::StartingSettings baked = azl::startingSettings(kept, {.bakeStatic = true});
    CHECK(baked.settings.bakeStatic);
    CHECK(baked.overridden.at("bakeStatic") == "--bake-static on");
    CHECK(azl::applied({{"bakeStatic", true}}, kept).bakeStatic);
    CHECK_THROWS_AS(azl::applied({{"bakeStatic", 1}}, kept), std::invalid_argument);
}

TEST_CASE("the audio input a client picked is kept with the command line it was picked under")
{
    TempDir dir;
    const fs::path file = dir.path / "audio-devices.json";
    const azl::ResumeAudio picked{{"uid-scarlett", {2}}, {"Focusrite", {}}};
    {
        azl::AudioDeviceSettings settings(file, true, 0.0);
        CHECK_FALSE(settings.picked());
        settings.pick(picked);
    }
    CHECK(azl::AudioDeviceSettings(file, true, 0.0).picked() == picked);

    CHECK_FALSE(azl::flagsOverridePick({"Focusrite", {}}, picked));
    CHECK(azl::flagsOverridePick({"BlackHole", {}}, picked));
    CHECK_FALSE(azl::flagsOverridePick({"uid-scarlett", {2}}, picked));
    CHECK_FALSE(azl::flagsOverridePick({"BlackHole", {}}, std::nullopt));
}
