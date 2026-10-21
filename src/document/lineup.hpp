#pragma once

#include "document/composition.hpp"
#include "document/wall_clock.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace azl {

// One slot of a night's lineup, read from its Gig Cue.
struct LineupSlot {
    // Its place in the lineup, from 1, in start order.
    size_t slot = 0;
    std::string dj;
    // Unix seconds.
    int64_t start = 0;
    uint64_t deck = 0;
    uint64_t cue = 0;

    bool operator==(const LineupSlot&) const = default;
};

// A Gig window of the composition with its lineup.
struct Night {
    GigWindow window;
    // Unix seconds.
    int64_t start = 0;
    int64_t end = 0;
    std::vector<LineupSlot> lineup;
};

// The night whose window holds `now`; the latest starting when windows overlap.
std::optional<Night> nightAt(const Composition& composition, double now, const UtcOffset& zone = systemUtcOffset);
// The night whose window holds `now`, else the next one starting within `ahead` seconds.
std::optional<Night> tonight(const Composition& composition, double now, const UtcOffset& zone = systemUtcOffset,
                             double ahead = 12 * 3600.0);
// The slot a deck shown from `at` belongs to: of the slots with that deck, the one starting latest at or before `at`,
// else the earliest; none when no slot has that deck.
const LineupSlot* slotFor(const Night& night, uint64_t deck, double at);
// The date a night's files go under: the window's start date, else the local calendar date at `now`.
std::string nightDate(const Composition& composition, double now, const UtcOffset& zone = systemUtcOffset);
// ISO 8601 with milliseconds: "2026-10-17T01:00:00.000Z", or with the local offset "2026-10-17T03:00:00.000+02:00".
std::string isoTime(double unixSeconds, std::optional<int> offsetSeconds = std::nullopt);
// A switch log line (engine/switch_log.hpp) as one line of text: local time, deck, slot and DJ, cause, recording.
std::string switchText(const nlohmann::json& line);

} // namespace azl
