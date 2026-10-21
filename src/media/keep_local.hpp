#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace azl {

// ~/Library/Application Support/Nocturne/keep-local.json
std::filesystem::path defaultKeepLocalFile();
// ~/Library/Application Support/Nocturne/Stills
std::filesystem::path defaultStillsFolder();
// ~/Library/Application Support/Nocturne/Fetched: media this machine fetched from the bucket.
std::filesystem::path defaultFetchFolder();

// A show as the Director knows it: the venue's host and the show file's path on the venue machine.
std::string showKey(const std::string& venue, const std::string& showFile);

// A media hash as it names objects and files: 64 lowercase hex digits.
bool isMediaHash(const std::string& text);

// Which media the Director keeps on its own disk. Per clip a choice (keep or not) wins over the per-show one; a kept
// show keeps every media hash it was last seen to use. Kept as JSON in `file` (empty: in memory only). Not
// thread-safe.
class KeepLocal {
public:
    explicit KeepLocal(std::filesystem::path file = {});

    // Reads the file again when it changed on disk since the last read or write; true when it did.
    bool reload();

    // Nothing: the clip follows its shows.
    std::optional<bool> clip(const std::string& hash) const;
    void setClip(const std::string& hash, std::optional<bool> keep);

    bool show(const std::string& show) const;
    // Kept, the show keeps `hashes`; not kept, it is forgotten.
    void setShow(const std::string& show, bool keep, const std::set<std::string>& hashes = {});
    // The media a kept show uses now; a show not kept is left alone. True when that changed anything.
    bool updateShow(const std::string& show, const std::set<std::string>& hashes);

    bool chosen(const std::string& hash) const;
    std::set<std::string> chosen() const;
    // Every clip with a choice of its own.
    const std::map<std::string, bool>& clips() const { return clips_; }
    const std::map<std::string, std::set<std::string>>& shows() const { return shows_; }

private:
    void load();
    void save();

    std::filesystem::path file_;
    std::filesystem::file_time_type stamp_{};
    std::map<std::string, bool> clips_;
    std::map<std::string, std::set<std::string>> shows_;
};

// JPEG stills by media hash in one folder, so a still a venue sent once is never asked for again.
class StillCache {
public:
    explicit StillCache(std::filesystem::path folder);

    std::optional<std::vector<uint8_t>> find(const std::string& hash) const;
    void store(const std::string& hash, const std::vector<uint8_t>& jpeg);

private:
    std::filesystem::path folder_;
};

} // namespace azl
