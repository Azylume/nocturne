#pragma once

#include "audio/opus_codec.hpp"

#include <doctest.h>

#include <cmath>
#include <numbers>
#include <vector>

inline constexpr double kTone = 440.0;

inline std::vector<float> tone(double rate, double seconds, double amplitude = 0.5)
{
    std::vector<float> samples(size_t(rate * seconds));
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = float(amplitude * std::sin(2.0 * std::numbers::pi * kTone * double(i) / rate));
    return samples;
}

struct Heard {
    double rms = 0.0;
    double hz = 0.0;
};

// The level and the frequency, from the zero crossings, of 48 kHz samples after the first 0.1 s.
inline Heard hear(const std::vector<float>& samples)
{
    const size_t skip = size_t(0.1 * azl::kOpusRate);
    REQUIRE(samples.size() > skip + 4800);
    double power = 0.0;
    size_t crossings = 0;
    for (size_t i = skip; i < samples.size(); ++i) {
        power += double(samples[i]) * samples[i];
        crossings += (samples[i - 1] < 0.0f) != (samples[i] < 0.0f);
    }
    const double seconds = double(samples.size() - skip) / azl::kOpusRate;
    return {std::sqrt(power / double(samples.size() - skip)), double(crossings) / 2.0 / seconds};
}
