#include "media/media_library.hpp"

#include "platform/app_paths.hpp"
#include "platform/crypto.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace azl {

namespace fs = std::filesystem;

namespace {

constexpr const char* kIndexFile = "index.json";
constexpr int kIndexVersion = 1;

struct FileStamp {
    uint64_t size = 0;
    int64_t mtime = 0;
};

std::optional<FileStamp> stamp(const fs::path& path)
{
    std::error_code ec;
    if (!fs::is_regular_file(path, ec))
        return std::nullopt;
    const uint64_t size = fs::file_size(path, ec);
    if (ec)
        return std::nullopt;
    const auto time = fs::last_write_time(path, ec);
    if (ec)
        return std::nullopt;
    return FileStamp{size, int64_t(time.time_since_epoch().count())};
}

bool unchanged(const MediaEntry& entry)
{
    const auto now = stamp(entry.path);
    return now && now->size == entry.size && now->mtime == entry.mtime;
}

bool hasContent(const fs::path& path, const std::string& hash)
{
    try {
        return sha256File(path) == hash;
    } catch (const std::runtime_error&) {
        return false;
    }
}

MediaStatus statusFromName(const std::string& name)
{
    for (MediaStatus s : {MediaStatus::Ok, MediaStatus::Unsupported, MediaStatus::Corrupt, MediaStatus::Missing})
        if (name == statusName(s))
            return s;
    return MediaStatus::Corrupt;
}

MediaEntry entryFromJson(const nlohmann::json& j)
{
    return {j.at("hash").get<std::string>(),
            j.at("path").get<std::string>(),
            j.at("name").get<std::string>(),
            j.at("size").get<uint64_t>(),
            j.at("mtime").get<int64_t>(),
            statusFromName(j.at("status").get<std::string>()),
            j.at("reason").get<std::string>()};
}

} // namespace

const char* statusName(MediaStatus status)
{
    switch (status) {
    case MediaStatus::Ok: return "ok";
    case MediaStatus::Unsupported: return "unsupported";
    case MediaStatus::Corrupt: return "corrupt";
    case MediaStatus::Missing: return "missing";
    }
    return "corrupt";
}

void to_json(nlohmann::json& j, const MediaEntry& entry)
{
    j = {{"hash", entry.hash},   {"path", entry.path.string()}, {"name", entry.name},     {"size", entry.size},
         {"mtime", entry.mtime}, {"status", statusName(entry.status)}, {"reason", entry.reason}};
}

std::string sha256File(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read " + file.string());
    Sha256 hash;
    std::vector<char> buffer(1 << 20);
    while (in) {
        in.read(buffer.data(), std::streamsize(buffer.size()));
        hash.update(buffer.data(), size_t(in.gcount()));
    }
    if (in.bad())
        throw std::runtime_error("cannot read " + file.string());
    const Sha256Digest digest = hash.finish();

    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    for (unsigned char byte : digest) {
        hex += kHex[byte >> 4];
        hex += kHex[byte & 15];
    }
    return hex;
}

MediaLibrary::MediaLibrary(fs::path root) : root_(std::move(root))
{
    fs::create_directories(root_);
    load();
}

fs::path MediaLibrary::defaultRoot()
{
    return appDataFolder() / "MediaLibrary";
}

MediaEntry MediaLibrary::add(const fs::path& file)
{
    const fs::path path = fs::absolute(file).lexically_normal();
    const auto fileStamp = stamp(path);
    if (!fileStamp)
        throw std::runtime_error("not a readable file: " + path.string());
    const std::string hash = sha256File(path);
    const ProbeResult probe = probeMedia(path);

    std::lock_guard lock(mutex_);
    folders_.insert(path.parent_path());
    return store({hash, path, path.filename().string(), fileStamp->size, fileStamp->mtime, probe.status, probe.reason});
}

std::optional<MediaEntry> MediaLibrary::find(const std::string& hash)
{
    std::optional<MediaEntry> entry;
    {
        std::lock_guard lock(mutex_);
        auto it = media_.find(hash);
        if (it == media_.end())
            return std::nullopt;
        entry = it->second;
    }
    if (entry->status != MediaStatus::Missing && unchanged(*entry))
        return entry;

    std::optional<MediaEntry> found = relocate(*entry);
    if (!found) {
        found = std::move(entry);
        found->status = MediaStatus::Missing;
        found->reason = "not found in the library's folders";
    }
    std::lock_guard lock(mutex_);
    return store(std::move(*found));
}

std::vector<MediaEntry> MediaLibrary::entries() const
{
    std::lock_guard lock(mutex_);
    std::vector<MediaEntry> all;
    for (const auto& [hash, entry] : media_)
        all.push_back(entry);
    return all;
}

void MediaLibrary::flag(const std::string& hash, MediaStatus status, const std::string& reason)
{
    std::lock_guard lock(mutex_);
    auto it = media_.find(hash);
    if (it == media_.end())
        return;
    it->second.status = status;
    it->second.reason = reason;
    save();
}

std::optional<MediaEntry> MediaLibrary::remove(const std::string& hash)
{
    std::lock_guard lock(mutex_);
    auto it = media_.find(hash);
    if (it == media_.end())
        return std::nullopt;
    MediaEntry entry = std::move(it->second);
    media_.erase(it);
    save();
    return entry;
}

// Only files of the same size are hashed, so a rescan of large folders stays cheap.
std::optional<MediaEntry> MediaLibrary::relocate(const MediaEntry& entry) const
{
    std::set<fs::path> folders;
    {
        std::lock_guard lock(mutex_);
        folders = folders_;
    }
    auto relocated = [&](const fs::path& path) -> std::optional<MediaEntry> {
        const auto fileStamp = stamp(path);
        if (!fileStamp || fileStamp->size != entry.size || !hasContent(path, entry.hash))
            return std::nullopt;
        const ProbeResult probe = probeMedia(path);
        return MediaEntry{entry.hash,   path, path.filename().string(), fileStamp->size, fileStamp->mtime,
                          probe.status, probe.reason};
    };

    if (auto same = relocated(entry.path))
        return same;
    for (const fs::path& folder : folders) {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (it->path() == entry.path)
                continue;
            if (auto moved = relocated(it->path()))
                return moved;
        }
    }
    return std::nullopt;
}

MediaEntry MediaLibrary::store(MediaEntry entry)
{
    media_[entry.hash] = entry;
    save();
    return entry;
}

void MediaLibrary::load()
{
    const fs::path index = root_ / kIndexFile;
    std::ifstream in(index);
    if (!in)
        return;
    try {
        const nlohmann::json j = nlohmann::json::parse(in);
        for (const auto& folder : j.at("folders"))
            folders_.insert(folder.get<std::string>());
        for (const auto& e : j.at("media")) {
            MediaEntry entry = entryFromJson(e);
            media_[entry.hash] = std::move(entry);
        }
    } catch (const nlohmann::json::exception& e) {
        std::fprintf(stderr, "media library: unreadable index %s (%s), starting empty\n", index.string().c_str(),
                     e.what());
        media_.clear();
        folders_.clear();
        std::error_code ec;
        fs::rename(index, root_ / "index.json.unreadable", ec);
    }
}

void MediaLibrary::save() const
{
    nlohmann::json media = nlohmann::json::array();
    for (const auto& [hash, entry] : media_)
        media.push_back(entry);
    nlohmann::json folders = nlohmann::json::array();
    for (const fs::path& folder : folders_)
        folders.push_back(folder.string());
    const nlohmann::json j = {{"version", kIndexVersion}, {"folders", folders}, {"media", media}};

    const fs::path index = root_ / kIndexFile;
    const fs::path temp = root_ / "index.json.tmp";
    {
        std::ofstream out(temp);
        out << j.dump(2);
        if (!out) {
            std::fprintf(stderr, "media library: cannot write %s\n", temp.string().c_str());
            return;
        }
    }
    std::error_code ec;
    fs::rename(temp, index, ec);
    if (ec)
        std::fprintf(stderr, "media library: cannot replace %s: %s\n", index.string().c_str(), ec.message().c_str());
}

} // namespace azl
