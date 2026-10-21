#include "output/display_keeper.hpp"

#include <algorithm>
#include <map>
#include <optional>

namespace azl {

namespace {

std::set<std::string> boundDisplays(const Composition& composition)
{
    std::set<std::string> bound;
    for (const Screen& screen : composition.screens)
        if (!screen.display.empty())
            bound.insert(screen.display);
    return bound;
}

bool reportsModel(const DisplayInfo& d)
{
    return d.vendor || d.model;
}

bool sameProjector(const DisplayInfo& a, const DisplayInfo& b)
{
    if (a.serial && b.serial && a.serial != b.serial)
        return false;
    if (!a.name.empty() && !b.name.empty() && a.name != b.name)
        return false;
    if (reportsModel(a) && reportsModel(b))
        return a.vendor == b.vendor && a.model == b.model;
    return !a.name.empty() && a.name == b.name;
}

// Keeps those `same` holds for, unless none does.
template <typename Same>
std::vector<DisplayInfo> narrowed(std::vector<DisplayInfo> candidates, Same same)
{
    std::vector<DisplayInfo> kept;
    std::copy_if(candidates.begin(), candidates.end(), std::back_inserter(kept), same);
    return kept.empty() ? candidates : kept;
}

std::optional<DisplayInfo> uniqueMatch(const DisplayInfo& of, const std::vector<DisplayInfo>& among)
{
    std::vector<DisplayInfo> candidates;
    std::copy_if(among.begin(), among.end(), std::back_inserter(candidates),
                 [&](const DisplayInfo& d) { return sameProjector(of, d); });
    candidates = narrowed(candidates, [&](const DisplayInfo& d) { return d.unit == of.unit; });
    candidates = narrowed(candidates, [&](const DisplayInfo& d) { return d.x == of.x && d.y == of.y; });
    if (candidates.size() != 1)
        return std::nullopt;
    return candidates.front();
}

std::vector<DisplayInfo> missingDisplays(const Composition& composition, const std::set<std::string>& attached,
                                         const KnownDisplays& known)
{
    std::vector<DisplayInfo> missing;
    for (const std::string& id : boundDisplays(composition))
        if (!attached.contains(id))
            if (std::optional<DisplayInfo> info = known.find(id))
                missing.push_back(*info);
    return missing;
}

} // namespace

std::vector<std::vector<std::string>> mirrorsAcrossScreens(const Composition& composition,
                                                           const std::vector<DisplayInfo>& attached)
{
    std::map<std::string, std::set<std::string>> sets;
    for (const DisplayInfo& d : attached)
        if (!d.mirrorOf.empty())
            sets[d.mirrorOf].insert({d.mirrorOf, d.id});
    const std::set<std::string> bound = boundDisplays(composition);
    std::vector<std::vector<std::string>> across;
    for (const auto& [primary, members] : sets)
        if (std::count_if(members.begin(), members.end(), [&](const std::string& id) { return bound.contains(id); }) >= 2)
            across.emplace_back(members.begin(), members.end());
    return across;
}

std::vector<DisplayRebind> returnedDisplays(const Composition& composition, const std::vector<DisplayInfo>& attached,
                                            const std::set<std::string>& before, const KnownDisplays& known)
{
    std::set<std::string> attachedIds;
    for (const DisplayInfo& d : attached)
        attachedIds.insert(d.id);
    const std::set<std::string> bound = boundDisplays(composition);
    std::vector<DisplayInfo> fresh;
    std::copy_if(attached.begin(), attached.end(), std::back_inserter(fresh),
                 [&](const DisplayInfo& d) { return !before.contains(d.id) && !bound.contains(d.id); });
    const std::vector<DisplayInfo> missing = missingDisplays(composition, attachedIds, known);

    std::vector<DisplayRebind> rebinds;
    for (const DisplayInfo& gone : missing) {
        const std::optional<DisplayInfo> back = uniqueMatch(gone, fresh);
        if (!back)
            continue;
        const std::optional<DisplayInfo> claimed = uniqueMatch(*back, missing);
        if (!claimed || claimed->id != gone.id)
            continue;
        for (const Screen& screen : composition.screens)
            if (screen.display == gone.id)
                rebinds.push_back({screen.id, gone.id, back->id});
    }
    return rebinds;
}

} // namespace azl
