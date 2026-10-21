#include "document/screen_roles.hpp"

#include <algorithm>
#include <set>

namespace azl {

std::string screenRole(const Screen& screen)
{
    return screen.name.empty() ? "Screen " + std::to_string(screen.id) : screen.name;
}

void placeByRole(Composition& composition, const RoleDisplays& roles, std::vector<std::string>& warnings)
{
    std::map<std::string, size_t> seen;
    std::set<std::string> taken;
    std::vector<Screen*> unmapped;
    for (Screen& screen : composition.screens) {
        const auto role = roles.find(screenRole(screen));
        const size_t index = seen[screenRole(screen)]++;
        if (role == roles.end() || index >= role->second.size() || taken.contains(role->second[index])) {
            unmapped.push_back(&screen);
            continue;
        }
        screen.display = role->second[index];
        if (!screen.display.empty())
            taken.insert(screen.display);
    }
    for (Screen* screen : unmapped) {
        if (screen->display.empty() || taken.insert(screen->display).second)
            continue;
        warnings.push_back("screen " + screen->name + ": display " + screen->display +
                           " carries another screen on this machine; no output");
        screen->display.clear();
    }
}

bool recordRoles(RoleDisplays& roles, const Composition& composition)
{
    RoleDisplays now;
    for (const Screen& screen : composition.screens)
        now[screenRole(screen)].push_back(screen.display);
    bool changed = false;
    for (auto& [role, displays] : now) {
        const auto kept = roles.find(role);
        if (kept == roles.end()) {
            if (std::all_of(displays.begin(), displays.end(), [](const std::string& d) { return d.empty(); }))
                continue;
            roles[role] = displays;
            changed = true;
            continue;
        }
        if (kept->second.size() > displays.size())
            displays.insert(displays.end(), kept->second.begin() + std::ptrdiff_t(displays.size()), kept->second.end());
        if (displays != kept->second) {
            kept->second = displays;
            changed = true;
        }
    }
    return changed;
}

} // namespace azl
