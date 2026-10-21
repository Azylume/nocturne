#include "output/screen_displays.hpp"

#include "document/composition_file.hpp"

#include <cstdio>

namespace azl {

ScreenDisplays::ScreenDisplays(std::filesystem::path file) : file_(std::move(file))
{
    if (file_.empty())
        return;
    try {
        if (const std::optional<nlohmann::json> j = readJsonFile(file_))
            roles_ = j->at("roles").get<RoleDisplays>();
    } catch (const std::exception& e) {
        std::printf("screen displays: %s; starting a new map\n", e.what());
    }
}

RoleDisplays ScreenDisplays::roles() const
{
    std::lock_guard lock(mutex_);
    return roles_;
}

void ScreenDisplays::keep(const Composition& composition)
{
    std::lock_guard lock(mutex_);
    if (!recordRoles(roles_, composition) || file_.empty())
        return;
    try {
        writeFileAtomically(file_, nlohmann::json{{"roles", roles_}}.dump(2) + "\n");
    } catch (const std::exception& e) {
        std::printf("screen displays: %s\n", e.what());
    }
}

} // namespace azl
