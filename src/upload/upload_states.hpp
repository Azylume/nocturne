#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace azl {

// ~/Library/Application Support/Nocturne/Uploads/states.json
std::filesystem::path defaultUploadStatesFile();

struct UploadState {
    enum class Kind { Uploading, Uploaded, Failed };

    Kind kind = Kind::Uploading;
    uint64_t bytes = 0;
    uint64_t size = 0;
    std::string source;
    std::string error;
    // The uploading process: once it is gone, the upload reads as failed.
    int pid = 0;

    bool operator==(const UploadState&) const = default;
};

const char* uploadStateName(UploadState::Kind kind);

// Each media hash's last upload from this machine, kept as JSON in `file` (empty: in memory only) so the uploader and
// the UI share it. Not thread-safe.
class UploadStates {
public:
    explicit UploadStates(std::filesystem::path file = {});

    // Reads the file again when it changed on disk since the last read or write; true when it did.
    bool reload();

    std::optional<UploadState> find(const std::string& hash) const;
    std::map<std::string, UploadState> all() const;
    // Merged into the file as it is on disk now, so other processes' uploads are kept.
    void set(const std::string& hash, const UploadState& state);

private:
    void load();
    void save();

    std::filesystem::path file_;
    std::filesystem::file_time_type stamp_{};
    std::map<std::string, UploadState> states_;
};

} // namespace azl
