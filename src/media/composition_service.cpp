#include "media/composition_service.hpp"

#include "media/composition_media.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>
#include <stdexcept>

namespace azl {

namespace fs = std::filesystem;

namespace {

constexpr size_t kRecent = 20;

fs::path pathField(const Message& message, const char* key)
{
    std::string path = message.value(key, "");
    if (path.starts_with("~/"))
        if (const char* home = std::getenv("HOME"))
            path = home + path.substr(1);
    if (!path.empty() && !fs::path(path).is_absolute())
        throw std::runtime_error(std::string("composition ") + key + " must be an absolute path on the engine machine");
    return path;
}

void print(const char* what, const fs::path& path, const std::vector<std::string>& warnings)
{
    std::printf("composition: %s %s\n", what, path.c_str());
    for (const std::string& w : warnings)
        std::printf("composition: warning: %s\n", w.c_str());
    std::fflush(stdout);
}

// Symbolic links and ".." are resolved first, so neither leads out of a root.
bool inside(const fs::path& path, const std::vector<fs::path>& roots)
{
    std::error_code ec;
    const fs::path real = fs::weakly_canonical(path, ec);
    if (ec)
        return false;
    return std::any_of(roots.begin(), roots.end(), [&](const fs::path& root) {
        std::error_code rootEc;
        const fs::path relative = real.lexically_relative(fs::weakly_canonical(root, rootEc));
        return !rootEc && !relative.empty() && *relative.begin() != "..";
    });
}

// Media present means found by the saved path or in the Media Library; nothing is hashed.
bool mediaPresent(nlohmann::json& composition, const fs::path& root, MediaLibrary& library)
{
    bool present = true;
    forEachCellJson(composition, [&](uint64_t, uint64_t, nlohmann::json& clip) {
        if (!present || !clip.is_object() || (clip.contains("text") && !clip["text"].is_null()))
            return;
        const fs::path path = clip.value("path", "");
        std::error_code ec;
        if (!path.empty() && fs::is_regular_file(path.is_absolute() ? path : root / path, ec))
            return;
        const std::optional<MediaEntry> entry = library.find(clip.value("hash", ""));
        present = entry && entry->status != MediaStatus::Missing;
    });
    return present;
}

// Whole Unix seconds, as stat's st_mtime.
double unixSeconds(fs::file_time_type time)
{
#ifdef _LIBCPP_VERSION
    const auto system = std::chrono::file_clock::to_sys(time);
#else
    const auto system = std::chrono::clock_cast<std::chrono::system_clock>(time);
#endif
    return double(std::chrono::floor<std::chrono::seconds>(system).time_since_epoch().count());
}

std::optional<Message> showEntry(const fs::path& file, MediaLibrary& library)
{
    std::error_code ec;
    if (!fs::is_regular_file(file, ec))
        return std::nullopt;
    const uint64_t size = fs::file_size(file, ec);
    const fs::file_time_type modified = fs::last_write_time(file, ec);
    if (ec)
        return std::nullopt;
    std::optional<nlohmann::json> j;
    try {
        j = readJsonFile(file);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!j || !j->is_object() || !j->contains("composition"))
        return std::nullopt;
    return Message{{"path", file.string()},
                   {"name", file.stem().string()},
                   {"modified", unixSeconds(modified)},
                   {"size", size},
                   {"mediaPresent", mediaPresent((*j)["composition"], mediaRootOf(file, {}), library)}};
}

void addJsonFiles(const fs::path& folder, std::vector<fs::path>& files)
{
    std::error_code ec;
    for (fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec))
        if (it->path().extension() == ".json")
            files.push_back(it->path());
}

} // namespace

CompositionService::CompositionService(DocumentServer& document, MediaLibrary& library, AttachedDisplays displays,
                                       ShowFolders folders, std::function<void()> beforeLoad)
    : document_(document), library_(library), displays_(std::move(displays)), folders_(std::move(folders)),
      beforeLoad_(std::move(beforeLoad)), queue_("nocturne.composition")
{
    std::error_code ec;
    fs::create_directories(folders_.shows, ec);
    document_.handle("composition", [this](const Message& message, DocumentServer::Sink reply) {
        queue_.async([this, message, reply = std::move(reply)] { handle(message, reply); });
    });
}

CompositionService::~CompositionService()
{
    document_.handle("composition", nullptr);
    queue_.flush();
}

void CompositionService::handle(const Message& message, const DocumentServer::Sink& reply)
{
    try {
        const std::string op = message.value("op", "");
        if (op == "shows")
            return reply(shows(message));
        const fs::path path = pathField(message, "path");
        const fs::path root = pathField(message, "mediaRoot");
        if (path.empty())
            throw std::runtime_error("composition " + op + " needs a path");
        if (op == "save") {
            const std::vector<std::string> warnings = saveComposition(path, document_.document(), library_, root);
            print("saved", path, warnings);
            document_.markSaved(path.string());
            remember(path);
            reply({{"type", "composition"}, {"op", "save"}, {"path", path.string()}, {"warnings", warnings}});
            return;
        }
        if (op != "load")
            throw std::runtime_error("unknown composition op " + op);
        DisplayPlacement displays = displays_();
        displays.remap = message.value("remapDisplays", false);
        LoadedComposition loaded = loadComposition(path, library_, displays, root);
        print("loaded", path, loaded.warnings);
        remember(path);
        Message done{{"type", "composition"}, {"op", "load"}, {"path", path.string()}, {"warnings", loaded.warnings}};
        if (!loaded.removed.empty())
            done["notice"] = removedEffectsNotice(loaded.removed);
        reply(done);
        if (beforeLoad_)
            beforeLoad_();
        document_.loadFile(message.at("client").get<ClientId>(), std::move(loaded.composition), path.string());
    } catch (const std::exception& e) {
        reply({{"type", "error"}, {"message", e.what()}});
    }
}

Message CompositionService::shows(const Message& message)
{
    const fs::path folder = pathField(message, "folder");
    if (!folder.empty() && !inside(folder, folders_.roots))
        throw std::runtime_error("composition folder " + folder.string() + " is outside the folders shows are listed in");
    std::vector<fs::path> recentShows = recent();
    const std::vector<fs::path> listable =
        message.value("loopback", true) ? folders_.roots : std::vector<fs::path>{folders_.shows};
    std::erase_if(recentShows, [&](const fs::path& show) { return !inside(show, listable); });
    std::vector<fs::path> files;
    if (!folder.empty()) {
        addJsonFiles(folder, files);
    } else {
        addJsonFiles(folders_.shows, files);
        std::set<fs::path> folders{folders_.shows.lexically_normal()};
        for (const fs::path& show : recentShows) {
            files.push_back(show);
            if (folders.insert(show.parent_path()).second)
                addJsonFiles(show.parent_path(), files);
        }
    }
    std::set<fs::path> seen;
    Message list = Message::array();
    for (const fs::path& file : files)
        if (seen.insert(file.lexically_normal()).second && inside(file, folders_.roots))
            if (std::optional<Message> entry = showEntry(file.lexically_normal(), library_))
                list.push_back(std::move(*entry));
    std::stable_sort(list.begin(), list.end(),
                     [](const Message& a, const Message& b) { return a["modified"] > b["modified"]; });
    Message recentList = Message::array();
    for (const fs::path& show : recentShows)
        recentList.push_back(show.string());
    return {{"type", "composition"}, {"op", "shows"},       {"folder", folder.string()},
            {"showsFolder", folders_.shows.string()},       {"recent", recentList}, {"shows", list}};
}

std::vector<fs::path> CompositionService::recent() const
{
    std::vector<fs::path> shows;
    try {
        if (std::optional<nlohmann::json> j = readJsonFile(library_.root() / "recent-shows.json"); j && j->is_array())
            for (const auto& show : *j)
                if (show.is_string())
                    shows.push_back(show.get<std::string>());
    } catch (const std::exception&) {
    }
    return shows;
}

void CompositionService::remember(const fs::path& show)
{
    std::vector<fs::path> shows = recent();
    const fs::path normal = fs::absolute(show).lexically_normal();
    std::erase(shows, normal);
    shows.insert(shows.begin(), normal);
    shows.resize(std::min(shows.size(), kRecent));
    nlohmann::json j = nlohmann::json::array();
    for (const fs::path& path : shows)
        j.push_back(path.string());
    try {
        writeFileAtomically(library_.root() / "recent-shows.json", j.dump(2) + "\n", false);
    } catch (const std::exception& e) {
        std::printf("composition: recent shows not kept: %s\n", e.what());
    }
}

} // namespace azl
