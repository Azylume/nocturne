#include "protocol/bundle_push.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <iterator>
#include <string_view>
#include <stdexcept>

extern char** environ;

namespace azl {

int runProgram(const std::vector<std::string>& argv)
{
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    for (int fd : {0, 1, 2})
        posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", fd ? O_WRONLY : O_RDONLY, 0);
    std::vector<char*> args;
    for (const std::string& a : argv)
        args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = 0;
    const int error = posix_spawn(&pid, args[0], &actions, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (error != 0)
        return -1;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
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
    std::string folder = (std::filesystem::temp_directory_path() / "nocturne-update-XXXXXX").string();
    if (!mkdtemp(folder.data()))
        throw std::runtime_error("cannot make a temporary folder");
    const std::filesystem::path archive = std::filesystem::path(folder) / "bundle.tgz";
    const std::string name = app.filename().string();
    if (runProgram({"/usr/bin/tar", "-czf", archive.string(), "-C", app.parent_path().string(), "-s",
                    ",^" + name + ",Nocturne.app,", name}) == 0)
        in_.open(archive, std::ios::binary | std::ios::ate);
    // The open stream keeps the archive readable.
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
    if (!in_)
        throw std::runtime_error("cannot pack " + app.string());
    size_ = uint64_t(in_.tellg());
    in_.seekg(0);
    if (size_ > kMaxBundleSize)
        throw std::runtime_error(app.string() + " packs to more than 1 GB");
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
