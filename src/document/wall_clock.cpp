#include "document/wall_clock.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace azl {

namespace {

constexpr int64_t kDay = 86400;

int64_t floorDiv(int64_t a, int64_t b)
{
    return a / b - (a % b != 0 && (a < 0) != (b < 0));
}

// Howard Hinnant's days_from_civil and civil_from_days.
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = floorDiv(y, 400);
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}

void civilFromDays(int64_t z, int64_t& y, unsigned& m, unsigned& d)
{
    z += 719468;
    const int64_t era = floorDiv(z, 146097);
    const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = int64_t(yoe) + era * 400 + (m <= 2);
}

bool digits(std::string_view text, size_t from, size_t count, int& value)
{
    value = 0;
    for (size_t i = from; i < from + count; ++i) {
        if (text[i] < '0' || text[i] > '9')
            return false;
        value = value * 10 + (text[i] - '0');
    }
    return true;
}

} // namespace

int systemUtcOffset(int64_t unixSeconds)
{
    const std::time_t t = std::time_t(unixSeconds);
    std::tm local{};
    localtime_r(&t, &local);
    return int(local.tm_gmtoff);
}

std::optional<int> clockMinutes(std::string_view text)
{
    int hours = 0, minutes = 0;
    if (text.size() != 5 || text[2] != ':' || !digits(text, 0, 2, hours) || !digits(text, 3, 2, minutes) ||
        hours > 23 || minutes > 59)
        return std::nullopt;
    return hours * 60 + minutes;
}

std::string clockText(int minutes)
{
    char text[8];
    std::snprintf(text, sizeof text, "%02d:%02d", (minutes / 60) % 24, minutes % 60);
    return text;
}

std::optional<int64_t> dateDays(std::string_view text)
{
    int y = 0, m = 0, d = 0;
    if (text.size() != 10 || text[4] != '-' || text[7] != '-' || !digits(text, 0, 4, y) || !digits(text, 5, 2, m) ||
        !digits(text, 8, 2, d) || m < 1 || m > 12 || d < 1)
        return std::nullopt;
    const int64_t days = daysFromCivil(y, unsigned(m), unsigned(d));
    int64_t yy = 0;
    unsigned mm = 0, dd = 0;
    civilFromDays(days, yy, mm, dd);
    if (int(mm) != m || int(dd) != d)
        return std::nullopt;
    return days;
}

std::string dateText(int64_t days)
{
    int64_t y = 0;
    unsigned m = 0, d = 0;
    civilFromDays(days, y, m, d);
    char text[16];
    std::snprintf(text, sizeof text, "%04lld-%02u-%02u", static_cast<long long>(y), m, d);
    return text;
}

int64_t localDay(int64_t unixSeconds, const UtcOffset& zone)
{
    return floorDiv(unixSeconds + zone(unixSeconds), kDay);
}

int localMinute(int64_t unixSeconds, const UtcOffset& zone)
{
    const int64_t local = unixSeconds + zone(unixSeconds);
    return int((local - floorDiv(local, kDay) * kDay) / 60);
}

int64_t unixAt(int64_t day, int minute, const UtcOffset& zone)
{
    const int64_t local = day * kDay + int64_t(minute) * 60;
    const auto reads = [&](int64_t t) { return t + zone(t); };
    const int64_t a = local - zone(local - kDay);
    const int64_t b = local - zone(local + kDay);
    const bool aFits = reads(a) == local, bFits = reads(b) == local;
    if (aFits || bFits)
        return aFits && bFits ? std::min(a, b) : aFits ? a : b;
    // A skipped hour: the clock reads before `local` at lo and after it at hi.
    int64_t lo = std::min(a, b), hi = std::max(a, b);
    while (hi - lo > 1) {
        const int64_t mid = lo + (hi - lo) / 2;
        (reads(mid) >= local ? hi : lo) = mid;
    }
    return hi;
}

std::optional<int64_t> nextAt(const CueAt& at, int64_t after, const UtcOffset& zone)
{
    if (at.date) {
        const std::optional<int64_t> day = dateDays(*at.date);
        if (!day)
            return std::nullopt;
        const int64_t t = unixAt(*day, at.minute, zone);
        return t > after ? std::optional(t) : std::nullopt;
    }
    for (int64_t day = localDay(after, zone) - 1;; ++day)
        if (const int64_t t = unixAt(day, at.minute, zone); t > after)
            return t;
}

std::pair<int64_t, int64_t> gigSpan(const GigWindow& window, const UtcOffset& zone)
{
    const std::optional<int64_t> day = dateDays(window.date);
    if (!day)
        return {0, 0};
    return {unixAt(*day, window.start, zone), unixAt(window.end > window.start ? *day : *day + 1, window.end, zone)};
}

std::optional<int64_t> gigWindowEnd(const std::vector<GigWindow>& gigs, double now, const UtcOffset& zone)
{
    std::optional<int64_t> found;
    for (const GigWindow& window : gigs)
        if (const auto [start, end] = gigSpan(window, zone); now >= double(start) && now < double(end))
            found = std::max(found.value_or(end), end);
    return found;
}

bool inGigWindow(const std::vector<GigWindow>& gigs, double now, std::string_view, const UtcOffset& zone)
{
    return gigWindowEnd(gigs, now, zone).has_value();
}

} // namespace azl
