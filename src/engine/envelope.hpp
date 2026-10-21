#pragma once

#include <cstdint>
#include <limits>
#include <optional>

namespace azl {

// A preset envelope's times in seconds. A rise reaches 90 % of the step within `attack`; a peak stays for `hold`, then
// halves (falls 6 dB) per `release`, linear in dB like a level meter's needle, until it meets the feature again.
struct EnvelopeTimes {
    double attack = 0.0;
    double hold = 0.0;
    double release = 0.0;
};

struct EnvelopeState {
    static constexpr uint64_t kNoOnsets = std::numeric_limits<uint64_t>::max();

    double value = 0.0;
    // Meter: hold left. Gate: time since the trigger.
    double held = 0.0;
    double since = 1e9;
    uint64_t onsets = kNoOnsets;
    bool armed = true;
};

// The meter's value after `dt` seconds with the feature at `input`.
double followMeter(EnvelopeState& state, double input, double dt, const EnvelopeTimes& times);

// Jumps to 1 on a trigger, holds, then falls 40 dB within `release` and ends at 0, whatever the feature does meanwhile.
// An onset feature (kick, snare, hat) passes its onset count and triggers on each new onset; any other triggers on
// rising past kGateOn, and again only after falling below kGateOff.
double followGate(EnvelopeState& state, double input, std::optional<uint64_t> onsets, double dt,
                  const EnvelopeTimes& times);

inline constexpr double kGateOn = 0.6;
inline constexpr double kGateOff = 0.3;

} // namespace azl
