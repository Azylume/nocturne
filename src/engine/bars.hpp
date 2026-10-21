#pragma once

#include "document/composition.hpp"

#include <algorithm>
#include <cmath>

// Bar arithmetic the Autopilots share: steps start and end on bar lines of four beats.

namespace azl::bars {

inline constexpr double kBar = 4.0;

inline double floor(double beat)
{
    return std::floor(beat / kBar + kBeatEpsilon) * kBar;
}

inline double ceil(double beat)
{
    return std::ceil(beat / kBar - kBeatEpsilon) * kBar;
}

inline double paced(int bars, double pace)
{
    return kBar * std::max(1.0, std::round(bars * pace));
}

// Where a step that started at `start` and lasts `before` beats starts once it lasts `after`: the fraction still to play
// keeps its share of the new length, ending on the next bar line at or after that.
inline double rescaledStart(double start, double before, double after, double beat)
{
    const double end = start + before;
    return (end > beat ? ceil(beat + (end - beat) / before * after) : end) - after;
}

// The bars left of a step ending at `end`, this one included.
inline int left(double end, double beat)
{
    return std::max(1, int(std::ceil((end - beat) / kBar - kBeatEpsilon)));
}

} // namespace azl::bars
