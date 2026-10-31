#include "protocol/bundle_push.hpp"

#include "platform/crypto.hpp"
#include "platform/process.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string_view>
#include <stdexcept>

namespace azl {

namespace fs = std::filesystem;

namespace {

// A new, empty folder of its own in the temporary folder.
fs::path makeTempFolder(const std::string& prefix)
{
    for (int attempt = 0; attempt < 16; ++attempt) {
        uint8_t random[6];
        randomBytes(random, sizeof random);
        char suffix[16];
        std::snprintf(suffix, sizeof suffix, "%02x%02x%02x%02x%02x%02x", random[0], random[1], random[2], random[3],
                      random[4], random[5]);
        const fs::path folder = fs::temp_directory_path() / (prefix + suffix);
        std::error_code ec;
        if (fs::create_directory(folder, ec))
            return folder;
    }
    throw std::runtime_error("cannot make a temporary folder");
}

// Packs `app` into `archive` as Nocturne.app; false when tar fails. Windows' tar cannot rename entries (-s), so there
// it packs a copy named Nocturne.app made in `scratch`.
bool pack(const fs::path& app, const fs::path& archive, const fs::path& scratch)
{
#ifdef _WIN32
    const fs::path copy = scratch / "Nocturne.app";
    std::error_code ec;
    fs::copy(app, copy, fs::copy_options::recursive, ec);
    const bool packed =
        !ec && runProgram({tarProgram().string(), "-czf", archive.string(), "-C", scratch.string(), "Nocturne.app"}) == 0;
    fs::remove_all(copy, ec);
    return packed;
#else
    const std::string name = app.filename().string();
    return runProgram({tarProgram().string(), "-czf", archive.string(), "-C", app.parent_path().string(), "-s",
                       ",^" + name + ",Nocturne.app,", name}) == 0;
#endif
}

} // namespace

fs::path tarProgram()
{
#ifdef _WIN32
    const char* root = std::getenv("SystemRoot");
    return fs::path(root ? root : "C:\\Windows") / "System32" / "tar.exe";
#else
    return "/usr/bin/tar";
#endif
}

int runProgram(const std::vector<std::string>& argv)
{
    std::optional<Process> program = spawnProcess({.program = argv[0],
                                                   .args = {argv.begin() + 1, argv.end()},
                                                   .input = Stdio::null(),
                                                   .output = Stdio::null(),
                                                   .error = Stdio::null()});
    return program ? program->wait() : -1;
}

std::string bundleBuildId(const std::filesystem::path& app)
{
    std::ifstream in(app / "Contents/MacOS/nocturne", std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    constexpr std::string_view kPrefix = "nocturne-build ";
    for (size_t at = bytes.find(kPrefix); at != std::string::npos; at = bytes.find(kPrefix, at + 1)) {
        const size_t start = at + kPrefix.size();
        const size_t dash = bytes.find_first_not_of("0123456789", start);
        if (dash == start || dash == std::string::npos || bytes[dash] != '-')
            continue;
        const size_t end = std::min(bytes.find_first_not_of("0123456789abcdef", dash + 1), bytes.size());
        if (end > dash + 1)
            return bytes.substr(start, end - start);
    }
    return {};
}

BundlePush::BundlePush(const std::filesystem::path& app) : build_(bundleBuildId(app))
{
    if (build_.empty())
        throw std::runtime_error(app.string() + " is not a Nocturne bundle with a build id");
    const fs::path folder = makeTempFolder("nocturne-update-");
    const fs::path archive = folder / "bundle.tgz";
    if (pack(app, archive, folder))
        in_.open(archive, std::ios::binary | std::ios::ate);
    // The open stream keeps the archive readable. Windows keeps an open file instead: it goes with this push.
    std::error_code ec;
    fs::remove_all(folder, ec);
    if (fs::exists(folder, ec))
        leftover_ = folder;
    if (!in_.is_open() || !in_)
        throw std::runtime_error("cannot pack " + app.string());
    size_ = uint64_t(in_.tellg());
    in_.seekg(0);
    if (size_ > kMaxBundleSize)
        throw std::runtime_error(app.string() + " packs to more than 1 GB");
}

BundlePush::~BundlePush()
{
    if (leftover_.empty())
        return;
    in_.close();
    std::error_code ec;
    fs::remove_all(leftover_, ec);
}

Message BundlePush::start() const
{
    return {{"type", "machine"}, {"op", "update"}, {"size", size_}, {"build", build_}};
}

std::optional<Message> BundlePush::next()
{
    if (offset_ >= size_)
        return std::nullopt;
    std::vector<uint8_t> data(size_t(std::min(kBundleChunk, size_ - offset_)));
    if (!in_.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size())))
        throw std::runtime_error("cannot read the packed bundle");
    Message part{{"type", "machine"}, {"op", "update"}, {"offset", offset_}, {"data", Message::binary(std::move(data))}};
    offset_ += part["data"].get_binary().size();
    return part;
}

} // namespace azl
