#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace azl {

constexpr size_t kMaxLogLines = 200;
constexpr size_t kMaxLogBytes = size_t(64) << 10;

struct LogTail {
    std::string text;
    // The file held more than `text`.
    bool truncated = false;
};

// The last `lines` lines (1 to kMaxLogLines) of `file` within its newest kMaxLogBytes, starting at a line start. Empty
// for a missing or unreadable file. Never throws.
LogTail tailLog(const std::filesystem::path& file, size_t lines);

} // namespace azl
