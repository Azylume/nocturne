#pragma once

#include "document/composition.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace azl {

// Autopilot's position on a layer: the cell it plays since `stepStart`, the cell it picked for the next step, and the
// document trigger it stepped from. It applies on a restart only while the layer still plays that trigger. `held`:
// the step is a trigger's hold.
struct ResumePilot {
    uint64_t layer = 0;
    std::optional<Play> trigger;
    Play play;
    double stepStart = 0.0;
    uint64_t next = 0;
    bool held = false;

    bool operator==(const ResumePilot&) const = default;
};

nlohmann::json pilotsJson(const std::vector<ResumePilot>& pilots);
std::vector<ResumePilot> pilotsFrom(const nlohmann::json& j);

// An audio input: an AudioInput request and its channels.
struct AudioChoice {
    std::string device;
    std::vector<int> channels;

    bool operator==(const AudioChoice&) const = default;
};

// The audio input open when the state was saved, and the command line's input then: only a start with the same
// command line input reopens it.
struct ResumeAudio {
    AudioChoice open;
    AudioChoice commandLine;

    bool operator==(const ResumeAudio&) const = default;
};

// Where the show was in time when it was saved: the beat at `savedAt` (seconds since the Unix epoch) at `tempo` BPM,
// so a restart continues the same beat grid.
struct Resume {
    double savedAt = 0.0;
    double beat = 0.0;
    double tempo = 120.0;
    std::vector<ResumePilot> pilots;
    std::optional<ResumeAudio> audio{};

    // The beat at Unix time `now`, had the show kept running.
    double beatAt(double now) const { return beat + (now - savedAt) * tempo / 60.0; }
};

// The show file the document was last loaded from or saved to, by absolute path, and whether it was edited since.
struct ShowFile {
    std::string path;
    bool unsaved = false;

    bool operator==(const ShowFile&) const = default;
};

// The composition document as one human-readable JSON file, with a format version. A file saved by a running engine
// also carries where the show was, so a restart resumes it, the content hash and absolute path of the --composition
// file it started from, and the show file the document belongs to; an authored file has none of them.
struct CompositionFile {
    Composition composition;
    std::optional<Resume> resume{};
    std::optional<std::string> source{};
    std::optional<std::string> show{};
    std::optional<ShowFile> file{};
};

inline constexpr int kCompositionFileVersion = 1;

nlohmann::json toJson(const CompositionFile& file);
// Throws ProtocolError for a file of another format or a newer version, or one that does not parse.
CompositionFile compositionFileFromJson(const nlohmann::json& j);
// Loads what it can of a show and never refuses one that has layers: an unversioned or newer file, and values this
// version does not know (a blend mode, a play mode, a whole cell or layer), are migrated or dropped, each with a
// warning. Throws ProtocolError only for JSON that is no composition at all.
CompositionFile compositionFileFromJson(const nlohmann::json& j, std::vector<std::string>& warnings);
// The same migration, as the file's JSON in the current format.
nlohmann::json migrateCompositionFile(const nlohmann::json& j, std::vector<std::string>& warnings);

// Writes a temporary file next to `path`, flushes it to disk and renames it over `path`, so a crash or power cut at
// any moment leaves either the old file or the new one; without `sync`, only a process crash. Throws
// std::runtime_error.
void writeFileAtomically(const std::filesystem::path& path, const std::string& contents, bool sync = true);
void saveCompositionFile(const std::filesystem::path& path, const CompositionFile& file);
// Nothing when the file does not exist; throws when it cannot be read or parsed.
std::optional<nlohmann::json> readJsonFile(const std::filesystem::path& path);
std::optional<CompositionFile> loadCompositionFile(const std::filesystem::path& path);
std::optional<CompositionFile> loadCompositionFile(const std::filesystem::path& path, std::vector<std::string>& warnings);

// Calls `f` with every cell of a composition document as JSON (the "composition" of a file), groups' layers included.
using CellJsonVisitor = std::function<void(uint64_t layer, uint64_t column, nlohmann::json& clip)>;
void forEachCellJson(nlohmann::json& composition, const CellJsonVisitor& f);

// Seconds since the Unix epoch.
double unixTime();

} // namespace azl
