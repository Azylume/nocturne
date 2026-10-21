#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// A deterministic techno loop at 48 kHz: four-on-the-floor kick, claps on 2 and 4, offbeat open hats, a rolling 55 Hz
// bassline on the three sixteenths after each kick, and a noise floor at -46 dBFS.
struct TechnoLoop {
    static constexpr double kRate = 48000.0;

    double bpm = 128.0;
    int bars = 8;
    // Time of the first kick.
    double start = 0.25;
    double gain = 1.0;

    double beat() const { return 60.0 / bpm; }
    int beats() const { return bars * 4; }
    double kick(int i) const { return start + i * beat(); }
    double hat(int i) const { return kick(i) + beat() / 2; }

    std::vector<float> render() const
    {
        const size_t length = size_t((start + beats() * beat() + 0.5) * kRate);
        std::vector<double> out(length, 0.0);
        uint32_t seed = 12345;
        auto noise = [&seed] {
            seed = seed * 1664525u + 1013904223u;
            return double(seed >> 8) / double(1 << 23) - 1.0;
        };
        for (double& s : out)
            s += 0.005 * noise();
        for (int i = 0; i < beats(); ++i) {
            addKick(out, kick(i));
            addHat(out, hat(i), noise);
            if (i % 2 == 1)
                addClap(out, kick(i), noise);
            for (int n = 1; n < 4; ++n)
                addBass(out, kick(i) + n * beat() / 4);
        }
        std::vector<float> samples(length);
        for (size_t i = 0; i < length; ++i)
            samples[i] = float(std::tanh(out[i]) * gain);
        return samples;
    }

private:
    template <typename F>
    static void add(std::vector<double>& out, double at, double seconds, F&& voice)
    {
        const size_t from = size_t(at * kRate);
        const size_t to = std::min(out.size(), from + size_t(seconds * kRate));
        for (size_t i = from; i < to; ++i)
            out[i] += voice(double(i - from) / kRate);
    }

    static void addKick(std::vector<double>& out, double at)
    {
        double phase = 0.0;
        add(out, at, 0.4, [&phase](double t) {
            phase += 2.0 * M_PI * (48.0 + 110.0 * std::exp(-t / 0.025)) / kRate;
            return 0.8 * std::exp(-t / 0.22) * std::min(1.0, t / 0.002) * std::sin(phase);
        });
    }

    template <typename Noise>
    static void addHat(std::vector<double>& out, double at, Noise& noise)
    {
        double last = 0.0;
        add(out, at, 0.12, [&](double t) {
            const double n = noise();
            const double high = n - last;
            last = n;
            return 0.18 * std::exp(-t / 0.035) * high;
        });
    }

    template <typename Noise>
    static void addClap(std::vector<double>& out, double at, Noise& noise)
    {
        double first = 0.0;
        double low = 0.0;
        double band = 0.0;
        add(out, at, 0.2, [&](double t) {
            first += 0.4 * (noise() - first);
            low += 0.4 * (first - low);
            band += 0.12 * (low - band);
            return 2.0 * std::exp(-t / 0.06) * (low - band);
        });
    }

    static void addBass(std::vector<double>& out, double at)
    {
        double low = 0.0;
        add(out, at, 0.1, [&low](double t) {
            const double saw = 2.0 * std::fmod(55.0 * t, 1.0) - 1.0;
            low += 0.03 * (saw - low);
            return 0.35 * std::min(1.0, t / 0.004) * std::exp(-t / 0.05) * low;
        });
    }
};
