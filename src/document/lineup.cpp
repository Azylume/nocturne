#include "document/lineup.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace azl {

namespace {

Night nightOf(const Composition& composition, const GigWindow& window, const UtcOffset& zone)
{
    const auto [start, end] = gigSpan(window, zone);
    Night night{window, start, end, {}};
    for (const Cue& cue : composition.cues) {
        if (cue.gig != window.date || !cue.deck || !cue.at)
            continue;
        const std::optional<int64_t> day = dateDays(cue.at->date.value_or(window.date));
        if (!day)
            continue;
        night.lineup.push_back({0, cue.name, unixAt(*day, cue.at->minute, zone), *cue.deck, cue.id});
    }
    std::stable_sort(night.lineup.begin(), night.lineup.end(),
                     [](const LineupSlot& a, const LineupSlot& b) { return a.start < b.start; });
    for (size_t i = 0; i < night.lineup.size(); ++i)
        night.lineup[i].slot = i + 1;
    return night;
}

} // namespace

std::optional<Night> nightAt(const Composition& composition, double now, const UtcOffset& zone)
{
    std::optional<Night> found;
    for (const GigWindow& window : composition.gigs) {
        const auto [start, end] = gigSpan(window, zone);
        if (now < double(start) || now >= double(end) || (found && found->start > start))
            continue;
        found = nightOf(composition, window, zone);
    }
    return found;
}

std::optional<Night> tonight(const Composition& composition, double now, const UtcOffset& zone, double ahead)
{
    if (std::optional<Night> night = nightAt(composition, now, zone))
        return night;
    const GigWindow* next = nullptr;
    int64_t nextStart = 0;
    for (const GigWindow& window : composition.gigs) {
        const int64_t start = gigSpan(window, zone).first;
        if (double(start) <= now || double(start) > now + ahead || (next && nextStart <= start))
            continue;
        next = &window;
        nextStart = start;
    }
    if (!next)
        return std::nullopt;
    return nightOf(composition, *next, zone);
}

const LineupSlot* slotFor(const Night& night, uint64_t deck, double at)
{
    const LineupSlot* latest = nullptr;
    const LineupSlot* earliest = nullptr;
    for (const LineupSlot& slot : night.lineup) {
        if (slot.deck != deck)
            continue;
        if (!earliest)
            earliest = &slot;
        if (double(slot.start) <= at)
            latest = &slot;
    }
    return latest ? latest : earliest;
}

std::string nightDate(const Composition& composition, double now, const UtcOffset& zone)
{
    if (std::optional<Night> night = nightAt(composition, now, zone))
        return night->window.date;
    return dateText(localDay(int64_t(std::floor(now)), zone));
}

std::string isoTime(double unixSeconds, std::optional<int> offsetSeconds)
{
    const int64_t millis = int64_t(std::floor(unixSeconds * 1000.0 + 0.5)) + int64_t(offsetSeconds.value_or(0)) * 1000;
    const int64_t seconds = int64_t(std::floor(double(millis) / 1000.0));
    const int64_t days = int64_t(std::floor(double(seconds) / 86400.0));
    const int64_t inDay = seconds - days * 86400;
    char text[64];
    std::snprintf(text, sizeof text, "%sT%02d:%02d:%02d.%03d", dateText(days).c_str(), int(inDay / 3600),
                  int(inDay / 60 % 60), int(inDay % 60), int(millis - seconds * 1000));
    if (!offsetSeconds)
        return std::string(text) + "Z";
    const int offset = std::abs(*offsetSeconds) / 60;
    char zone[16];
    std::snprintf(zone, sizeof zone, "%c%02d:%02d", *offsetSeconds < 0 ? '-' : '+', offset / 60, offset % 60);
    return std::string(text) + zone;
}

std::string switchText(const nlohmann::json& line)
{
    const std::string local = line.value("local", "");
    std::string text = (local.size() >= 19 ? local.substr(11, 8) : local) + "  deck " +
                       std::to_string(line.value("deck", uint64_t(0)));
    if (const std::string name = line.value("deckName", ""); !name.empty())
        text += " " + name;
    if (line.contains("slot") && line["slot"].is_number())
        text += "  slot " + std::to_string(line["slot"].get<int>()) + " " + line.value("dj", "");
    text += "  " + line.value("cause", "");
    if (const auto& recording = line.value("recording", nlohmann::json()); recording.is_object()) {
        char at[64];
        std::snprintf(at, sizeof at, "  at %.3f s in ", recording.value("mediaTime", 0.0));
        text += at + recording.value("file", "");
    }
    return text;
}

} // namespace azl
