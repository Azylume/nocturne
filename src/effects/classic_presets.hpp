#pragma once

#include "import/xml.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace azl {

// A video effect preset file of the app the Classic pack mirrors, mapped onto its Classic effect.
struct ClassicPreset {
    std::string effectType;
    std::string name;
    std::map<std::string, double> params;
    // What the preset holds that the Classic effect does not take, e.g. "Direction animation".
    std::vector<std::string> skipped;
};

// Null when the preset's effect has no Classic counterpart.
std::optional<ClassicPreset> classicPreset(const XmlNode& preset, const std::string& name);

// Reads "Video Effects/<effect>/<name>.xml" under each root; a name read from an earlier root wins. Unreadable files
// are skipped.
std::vector<ClassicPreset> readClassicPresets(const std::vector<std::filesystem::path>& roots);

// Our own looks for the Classic effects. Every parameter is set; the ones a look does not name take their defaults.
std::vector<ClassicPreset> classicBuiltinPresets();

// The built-in presets: our own looks for Nocturne's own effects, then the Classic ones. Every parameter is set.
std::vector<ClassicPreset> builtinPresets();

// The Presets folders of that app on this machine, found by their layout: the user's under ~/Documents first, then
// the ones installed with the app under /Applications.
std::vector<std::filesystem::path> classicPresetRoots();

} // namespace azl
