#pragma once

#include "document/composition.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace azl {

// The local clock of the venue machine: seconds east of UTC at a Unix time. Local times are what this clock reads.
using UtcOffset = std::function<int(int64_t unixSeconds)>;
int systemUtcOffset(int64_t unixSeconds);

// "03:00" -> 180; none unless HH:MM within 00:00..23:59.
std::optional<int> clockMinutes(std::string_view text);
std::string clockText(int minutes);
// "2026-10-16" -> days since 1970-01-01; none unless it is a real date.
std::optional<int64_t> dateDays(std::string_view text);
std::string dateText(int64_t days);

int64_t localDay(int64_t unixSeconds, const UtcOffset& zone = systemUtcOffset);
// The clock time the local clock reads, in minutes since midnight.
int localMinute(int64_t unixSeconds, const UtcOffset& zone = systemUtcOffset);
// The first Unix time at which the local clock reads `minute` of `day` or later: in a repeated hour (DST ends) the
// first of the two, in a skipped one (DST starts) the moment the clock jumps past it.
int64_t unixAt(int64_t day, int minute, const UtcOffset& zone = systemUtcOffset);
// The first time after `after` that the clock time comes round: once on its date, or daily; none once it has passed.
std::optional<int64_t> nextAt(const CueAt& at, int64_t after, const UtcOffset& zone = systemUtcOffset);

// Its start and end as Unix times.
std::pair<int64_t, int64_t> gigSpan(const GigWindow& window, const UtcOffset& zone = systemUtcOffset);
// Whether `now` (Unix seconds) falls in one of the venue's Gig windows, for what `channel` names ("update", "sync").
// No channel is let through a window yet; a named exception (e.g. guest clips during a gig) goes here.
// The end of the Gig window `now` falls in, the latest when windows overlap; none outside them all.
std::optional<int64_t> gigWindowEnd(const std::vector<GigWindow>& gigs, double now,
                                    const UtcOffset& zone = systemUtcOffset);
bool inGigWindow(const std::vector<GigWindow>& gigs, double now, std::string_view channel = {},
                 const UtcOffset& zone = systemUtcOffset);

} // namespace azl
