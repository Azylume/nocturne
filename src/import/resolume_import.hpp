#pragma once

#include "document/composition.hpp"
#include "import/xml.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace azl {

// Where in the source something was, and what became of it.
struct ImportNote {
    std::string where;
    std::string what;

    bool operator==(const ImportNote&) const = default;
};

// What an import did with every element of the source: mapped as it was, mapped with a conversion that may not match
// Resolume exactly, not imported at all, or left out because it does not change the picture.
struct ImportReport {
    std::vector<std::string> mapped;
    std::vector<ImportNote> approximated;
    std::vector<ImportNote> unsupported;
    std::vector<std::string> ignored;
    std::vector<std::string> media;
};

std::string formatReport(const ImportReport& report, const std::string& title);

struct MediaFile {
    std::filesystem::path path;
    uint64_t size = 0;
    std::string hash;
};

// The file to play for a clip whose source names `original`; none when there is none.
using MediaFinder = std::function<std::optional<MediaFile>(const std::filesystem::path& original)>;

// Finds a clip's media in `dir` by name: its file name, else its stem with ".mp4" or ".mov" (an H.264 or HAP transcode
// from tools/transcode-media.sh). Hashes each file once.
MediaFinder mediaIn(const std::filesystem::path& dir);

struct ImportedComposition {
    Composition composition;
    // Each cell's media by layer and column id: the file found, or the source's own path when none was.
    std::map<std::pair<uint64_t, uint64_t>, MediaFile> media;
};

struct AvcOptions {
    // The column of the shown deck every layer plays on load (1-based); 0 plays nothing, as Resolume saved it.
    size_t startColumn = 1;
    // A chain every clip of a layer carries alike runs as the layer's, ahead of the layer's own effects, so the layer
    // compiles one chain and not one per clip.
    bool hoistClipChains = true;
};

// Imports a Resolume Arena composition (.avc). Every element ends up in the composition or in the report.
ImportedComposition importAvc(const XmlNode& avc, const MediaFinder& media, ImportReport& report,
                              const AvcOptions& options = {});

// Imports the screens and slices of an Advanced Output preset or AdvancedOutput.xml. Screens take `displays` in order,
// the source's output device ids after those run out.
std::vector<Screen> importAdvancedOutput(const XmlNode& preset, const std::vector<std::string>& displays,
                                         ImportReport& report);

// The portable composition file (media/composition_media.hpp): each cell carries its media's path, relative to
// `mediaRoot` when inside it, and size.
nlohmann::json portableFile(const ImportedComposition& imported, const std::filesystem::path& mediaRoot);

} // namespace azl
