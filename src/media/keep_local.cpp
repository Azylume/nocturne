#include "media/keep_local.hpp"

#include "media/media_library.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace azl {

namespace fs = std::filesystem;

namespace {

fs::path supportFolder()
{
    return MediaLibrary::defaultRoot().parent_path();
}

std::optional<fs::file_time_type> stampOf(const fs::path& file)
{
    std::error_code ec;
    const auto time = fs::last_write_time(file, ec);
    if (ec)
        return std::nullopt;
    return time;
}

} // namespace

fs::path defaultKeepLocalFile()
{
    return supportFolder() / "keep-local.json";
}

fs::path defaultStillsFolder()
{
    return supportFolder() / "Stills";
}

fs::path defaultFetchFolder()
{
    return supportFolder() / "Fetched";
}

std::string showKey(const std::string& venue, const std::string& showFile)
{
    return venue + ":" + showFile;
}

bool isMediaHash(const std::string& text)
{
    return text.size() == 64 &&
           std::ranges::all_of(text, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string mediaExtension(const std::string& name)
{
    const std::string extension = fs::path(name).extension().string();
    if (extension.size() < 2 || extension.size() > 9 ||
        !std::all_of(extension.begin() + 1, extension.end(), [](unsigned char c) { return std::isalnum(c); }))
        return {};
    return extension;
}

KeepLocal::KeepLocal(fs::path file) : file_(std::move(file))
{
    reload();
}

bool KeepLocal::reload()
{
    if (file_.empty())
        return false;
    const auto stamp = stampOf(file_);
    if (!stamp || *stamp == stamp_)
        return false;
    stamp_ = *stamp;
    load();
    return true;
}

std::optional<bool> KeepLocal::clip(const std::string& hash) const
{
    auto it = clips_.find(hash);
    if (it == clips_.end())
        return std::nullopt;
    return it->second;
}

void KeepLocal::setClip(const std::string& hash, std::optional<bool> keep)
{
    if (keep)
        clips_[hash] = *keep;
    else
        clips_.erase(hash);
    save();
}

bool KeepLocal::show(const std::string& show) const
{
    return shows_.contains(show);
}

void KeepLocal::setShow(const std::string& show, bool keep, const std::set<std::string>& hashes)
{
    if (keep)
        shows_[show] = hashes;
    else
        shows_.erase(show);
    save();
}

bool KeepLocal::updateShow(const std::string& show, const std::set<std::string>& hashes)
{
    auto it = shows_.find(show);
    if (it == shows_.end() || it->second == hashes)
        return false;
    it->second = hashes;
    save();
    return true;
}

bool KeepLocal::chosen(const std::string& hash) const
{
    if (const auto own = clip(hash))
        return *own;
    return std::ranges::any_of(shows_, [&](const auto& show) { return show.second.contains(hash); });
}

std::set<std::string> KeepLocal::chosen() const
{
    std::set<std::string> all;
    for (const auto& [show, hashes] : shows_)
        all.insert(hashes.begin(), hashes.end());
    for (const auto& [hash, keep] : clips_) {
        if (keep)
            all.insert(hash);
        else
            all.erase(hash);
    }
    return all;
}

void KeepLocal::load()
{
    clips_.clear();
    shows_.clear();
    std::ifstream in(file_);
    try {
        const nlohmann::json j = nlohmann::json::parse(in);
        const nlohmann::json clips = j.value("clips", nlohmann::json::object());
        const nlohmann::json shows = j.value("shows", nlohmann::json::object());
        for (const auto& [hash, keep] : clips.items())
            clips_[hash] = keep.get<bool>();
        for (const auto& [show, hashes] : shows.items())
            shows_[show] = hashes.get<std::set<std::string>>();
    } catch (const nlohmann::json::exception& e) {
        std::fprintf(stderr, "keep local: unreadable %s (%s)\n", file_.string().c_str(), e.what());
    }
}

void KeepLocal::save()
{
    if (file_.empty())
        return;
    const nlohmann::json j = {{"clips", clips_}, {"shows", shows_}};
    std::error_code ec;
    fs::create_directories(file_.parent_path(), ec);
    const fs::path temp = file_.string() + ".tmp";
    {
        std::ofstream out(temp);
        out << j.dump(2);
        if (!out) {
            std::fprintf(stderr, "keep local: cannot write %s\n", temp.string().c_str());
            return;
        }
    }
    fs::rename(temp, file_, ec);
    if (const auto stamp = stampOf(file_))
        stamp_ = *stamp;
}

StillCache::StillCache(fs::path folder) : folder_(std::move(folder)) {}

std::optional<std::vector<uint8_t>> StillCache::find(const std::string& hash) const
{
    if (folder_.empty() || !isMediaHash(hash))
        return std::nullopt;
    std::ifstream in(folder_ / (hash + ".jpg"), std::ios::binary);
    if (!in)
        return std::nullopt;
    return std::vector<uint8_t>{std::istreambuf_iterator<char>(in), {}};
}

void StillCache::store(const std::string& hash, const std::vector<uint8_t>& jpeg)
{
    if (folder_.empty() || !isMediaHash(hash) || jpeg.empty())
        return;
    std::error_code ec;
    fs::create_directories(folder_, ec);
    const fs::path temp = folder_ / (hash + ".jpg.tmp");
    std::ofstream(temp, std::ios::binary).write(reinterpret_cast<const char*>(jpeg.data()), std::streamsize(jpeg.size()));
    fs::rename(temp, folder_ / (hash + ".jpg"), ec);
}

} // namespace azl
