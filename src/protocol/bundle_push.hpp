#pragma once

#include "protocol/messages.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace azl {

inline constexpr uint64_t kBundleChunk = uint64_t(1) << 20;
inline constexpr uint64_t kMaxBundleSize = uint64_t(1) << 30;

// The system's tar: bsdtar, on Windows the one without -s.
std::filesystem::path tarProgram();
// Runs a program with its arguments, its output discarded, and waits for it; its exit status, -1 when it did not run.
int runProgram(const std::vector<std::string>& argv);
// The build id an engine binary carries ("nocturne-build ID", protocol/build_info.cpp.in); empty without one.
std::string bundleBuildId(const std::filesystem::path& app);

// A client's side of Update (engine/bundle_update.hpp): Nocturne.app packed as a gzipped tar and sent in parts.
// Gzip's checksum and the installer's signature check catch a damaged transfer.
class BundlePush {
public:
    // Packs `app` as Nocturne.app; throws when it is not a Nocturne bundle or cannot be packed.
    explicit BundlePush(const std::filesystem::path& app);
    ~BundlePush();

    uint64_t size() const { return size_; }
    const std::string& build() const { return build_; }
    // {"type": "machine", "op": "update", "size", "build"}
    Message start() const;
    // The next part, none after the last.
    std::optional<Message> next();

private:
    std::filesystem::path leftover_;
    std::ifstream in_;
    uint64_t size_ = 0;
    uint64_t offset_ = 0;
    std::string build_;
};

} // namespace azl
