#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>

namespace azl {

// An append-only log file that survives restarts and rotates by size: when it grows past `maxBytes`, `log` becomes
// `log.1`, `log.1` becomes `log.2` and so on, keeping `keep` old files. Thread-safe; every write is flushed.
class RotatingLog {
public:
    static constexpr uint64_t kDefaultMaxBytes = uint64_t(64) << 20;
    static constexpr int kDefaultKeep = 4;

    // Throws std::runtime_error when the file cannot be opened.
    explicit RotatingLog(std::filesystem::path path, uint64_t maxBytes = kDefaultMaxBytes, int keep = kDefaultKeep);
    ~RotatingLog();
    RotatingLog(const RotatingLog&) = delete;
    RotatingLog& operator=(const RotatingLog&) = delete;

    void write(const std::string& text);
    const std::filesystem::path& path() const { return path_; }

private:
    void open();
    void rotate();

    std::filesystem::path path_;
    uint64_t maxBytes_;
    int keep_;
    std::mutex mutex_;
    FILE* file_ = nullptr;
    uint64_t size_ = 0;
};

} // namespace azl
