#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace azl {

enum class MediaStatus { Ok, Unsupported, Corrupt, Missing };

struct MediaEntry {
    std::string hash;
    std::filesystem::path path;
    std::string name;
    uint64_t size = 0;
    int64_t mtime = 0;
    MediaStatus status = MediaStatus::Ok;
    std::string reason;
};

struct ProbeResult {
    MediaStatus status = MediaStatus::Ok;
    std::string reason;
};

// How long probing waits for the first frame before the file counts as unplayable.
inline constexpr std::chrono::milliseconds kProbeDeadline{2000};

// Whether the file is a video the engine can play (AVFoundation; implemented in video_decoder.mm). The first frame is
// read on a worker thread, so a file that blocks AVFoundation for good costs kProbeDeadline, not the caller's thread.
// `stall` is fault injection for tests: the read blocks that long first.
ProbeResult probeMedia(const std::filesystem::path& file, std::chrono::milliseconds stall = {});
// Hex SHA-256 of the file's content. Throws std::runtime_error when it cannot be read.
std::string sha256File(const std::filesystem::path& file);

// The machine's store of media files, identified by content hash (ADR 0007). Its index (hash -> current path, display
// name, status) lives in `root`. The folders files were added from are the library's folders: a file renamed or moved
// anywhere inside them is found again by hash. Thread-safe; hashing and probing run on the caller's thread, outside
// the lock, so never call it from the render thread.
class MediaLibrary {
public:
    explicit MediaLibrary(std::filesystem::path root);

    // ~/Library/Application Support/Nocturne/MediaLibrary
    static std::filesystem::path defaultRoot();

    // Hashes and probes the file and records it (again, if already known). Unplayable files are recorded too, flagged
    // with a reason. Throws std::runtime_error when the path is not a readable regular file.
    MediaEntry add(const std::filesystem::path& file);
    // The entry with its current path; a file that moved since is searched for by hash in the library's folders. A
    // file that cannot be found comes back flagged Missing.
    std::optional<MediaEntry> find(const std::string& hash);
    std::vector<MediaEntry> entries() const;
    const std::filesystem::path& root() const { return root_; }
    void flag(const std::string& hash, MediaStatus status, const std::string& reason);
    // Forgets the media; its file stays where it is. Returns the entry it had.
    std::optional<MediaEntry> remove(const std::string& hash);

private:
    std::optional<MediaEntry> relocate(const MediaEntry& entry) const;
    MediaEntry store(MediaEntry entry);
    void load();
    void save() const;

    std::filesystem::path root_;
    mutable std::mutex mutex_;
    std::map<std::string, MediaEntry> media_;
    std::set<std::filesystem::path> folders_;
};

const char* statusName(MediaStatus status);
void to_json(nlohmann::json& j, const MediaEntry& entry);

} // namespace azl
