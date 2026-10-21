#include "engine/envelope.hpp"

#include <algorithm>
#include <cmath>

namespace azl {

namespace {

// Below -60 dB the meter meets the feature at once.
constexpr double kFloor = 0.001;

bool triggered(EnvelopeState& state, double input, std::optional<uint64_t> onsets)
{
    if (onsets) {
        const bool fresh = state.onsets != EnvelopeState::kNoOnsets && *onsets != state.onsets;
        state.onsets = *onsets;
        return fresh;
    }
    if (input < kGateOff)
        state.armed = true;
    if (!state.armed || input < kGateOn)
        return false;
    state.armed = false;
    return true;
}

} // namespace

double followMeter(EnvelopeState& state, double input, double dt, const EnvelopeTimes& times)
{
    double& v = state.value;
    if (input >= v) {
        v = times.attack > 0.0 ? v + (input - v) * (1.0 - std::exp(-dt * std::log(10.0) / times.attack)) : input;
        state.held = times.hold;
        return v;
    }
    const double held = std::min(state.held, dt);
    state.held -= held;
    v = times.release > 0.0 ? std::max(input, v * std::exp2(-(dt - held) / times.release)) : input;
    if (v < kFloor)
        v = input;
    return v;
}

double followGate(EnvelopeState& state, double input, std::optional<uint64_t> onsets, double dt,
                  const EnvelopeTimes& times)
{
    state.since = triggered(state, input, onsets) ? 0.0 : state.since + dt;
    const double t = state.since - times.hold;
    if (t <= 0.0)
        state.value = 1.0;
    else
        state.value = t >= times.release ? 0.0 : std::pow(10.0, -2.0 * t / times.release);
    return state.value;
}

} // namespace azl
