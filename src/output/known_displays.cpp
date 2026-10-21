#include "output/known_displays.hpp"

#include "document/composition_file.hpp"

#include <cstdio>
#include <fstream>

namespace azl {

namespace {

nlohmann::json detailsJson(const DisplayInfo& d)
{
    return {{"name", d.name}, {"vendor", d.vendor}, {"model", d.model}, {"serial", d.serial},
            {"unit", d.unit},  {"x", d.x},           {"y", d.y}};
}

DisplayInfo fromJson(const std::string& id, const nlohmann::json& j)
{
    DisplayInfo d{.id = id};
    d.name = j.value("name", "");
    d.vendor = j.value("vendor", 0u);
    d.model = j.value("model", 0u);
    d.serial = j.value("serial", 0u);
    d.unit = j.value("unit", 0u);
    d.x = j.value("x", 0);
    d.y = j.value("y", 0);
    return d;
}

// A mirroring display reports no name of its own and its mirror's position, so those stay as last seen.
DisplayInfo merged(const DisplayInfo& known, const DisplayInfo& seen)
{
    DisplayInfo d = seen;
    d.mirrorOf.clear();
    if (seen.name.empty())
        d.name = known.name;
    if (!seen.mirrorOf.empty()) {
        d.x = known.x;
        d.y = known.y;
    }
    return d;
}

bool sameDetails(const DisplayInfo& a, const DisplayInfo& b)
{
    return detailsJson(a) == detailsJson(b);
}

} // namespace

KnownDisplays::KnownDisplays(std::filesystem::path file) : file_(std::move(file))
{
    if (file_.empty())
        return;
    try {
        const std::optional<nlohmann::json> j = readJsonFile(file_);
        if (!j)
            return;
        const nlohmann::json details = j->value("details", nlohmann::json::object());
        for (const std::string& id : j->value("displays", std::set<std::string>{}))
            displays_[id] = fromJson(id, details.value(id, nlohmann::json::object()));
        unmirror_ = j->value("unmirror", true);
    } catch (const std::exception& e) {
        std::printf("known displays: %s; starting a new list\n", e.what());
    }
}

void KnownDisplays::add(const std::vector<DisplayInfo>& displays)
{
    std::lock_guard lock(mutex_);
    bool changed = false;
    for (const DisplayInfo& seen : displays) {
        auto [it, added] = displays_.try_emplace(seen.id, DisplayInfo{.id = seen.id});
        const DisplayInfo next = merged(it->second, seen);
        if (added || !sameDetails(it->second, next)) {
            it->second = next;
            changed = true;
        }
    }
    if (changed)
        save();
}

std::set<std::string> KnownDisplays::ids() const
{
    std::lock_guard lock(mutex_);
    std::set<std::string> ids;
    for (const auto& [id, display] : displays_)
        ids.insert(id);
    return ids;
}

std::optional<DisplayInfo> KnownDisplays::find(const std::string& id) const
{
    std::lock_guard lock(mutex_);
    auto it = displays_.find(id);
    if (it == displays_.end())
        return std::nullopt;
    return it->second;
}

bool KnownDisplays::unmirror() const
{
    std::lock_guard lock(mutex_);
    return unmirror_;
}

void KnownDisplays::setUnmirror(bool on)
{
    std::lock_guard lock(mutex_);
    if (unmirror_ == on)
        return;
    unmirror_ = on;
    save();
}

void KnownDisplays::save() const
{
    if (file_.empty())
        return;
    nlohmann::json ids = nlohmann::json::array();
    nlohmann::json details = nlohmann::json::object();
    for (const auto& [id, display] : displays_) {
        ids.push_back(id);
        details[id] = detailsJson(display);
    }
    const std::filesystem::path temporary = file_.string() + ".tmp";
    std::ofstream(temporary) << nlohmann::json{{"displays", ids}, {"details", details}, {"unmirror", unmirror_}}.dump(2);
    std::error_code ec;
    std::filesystem::rename(temporary, file_, ec);
}

} // namespace azl
