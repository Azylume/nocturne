#include "audio/audio_analysis.hpp"
#include "document/modulation.hpp"
#include "techno_loop.hpp"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

using azl::AudioFeature;
using azl::AudioFrame;

constexpr double kTolerance = 0.020;

struct Onsets {
    std::array<std::vector<double>, azl::kOnsetKinds> times;
    std::vector<AudioFrame> frames;
};

// Feeds the samples in chunks of random size, as audio callbacks deliver them.
Onsets analyse(const std::vector<float>& samples, azl::AudioSettings settings = {}, uint32_t seed = 1)
{
    azl::AudioAnalyzer analyzer(TechnoLoop::kRate, settings);
    Onsets result;
    std::array<uint64_t, azl::kOnsetKinds> seen{};
    std::mt19937 rng(seed);
    std::uniform_int_distribution<size_t> chunk(1, 1024);
    for (size_t i = 0; i < samples.size();) {
        const size_t n = std::min(chunk(rng), samples.size() - i);
        analyzer.process(samples.data() + i, n, double(i) / TechnoLoop::kRate, true, [&](const AudioFrame& frame) {
            for (size_t k = 0; k < azl::kOnsetKinds; ++k)
                if (frame.onsets[k] != seen[k]) {
                    seen[k] = frame.onsets[k];
                    result.times[k].push_back(frame.time);
                }
            result.frames.push_back(frame);
        });
        i += n;
    }
    return result;
}

// After the first bar, which lets auto gain settle, every expected time has exactly one detection within the
// tolerance, and nothing else was detected except near `ignore`.
void checkGrid(std::vector<double> detected, std::vector<double> expected, const std::string& what, double from,
               const std::vector<double>& ignore = {})
{
    CAPTURE(what);
    const auto near = [](const std::vector<double>& times, double t) {
        return std::any_of(times.begin(), times.end(), [t](double x) { return std::abs(x - t) <= kTolerance; });
    };
    std::erase_if(detected, [&](double t) { return t < from - kTolerance || near(ignore, t); });
    std::erase_if(expected, [&](double t) { return t < from; });
    CHECK(detected.size() == expected.size());
    double worst = 0.0;
    double sum = 0.0;
    for (double t : expected) {
        CAPTURE(t);
        CHECK(std::count_if(detected.begin(), detected.end(), [t](double d) { return std::abs(d - t) <= kTolerance; }) ==
              1);
        auto closest = std::min_element(detected.begin(), detected.end(),
                                        [t](double a, double b) { return std::abs(a - t) < std::abs(b - t); });
        if (closest != detected.end()) {
            worst = std::max(worst, std::abs(*closest - t));
            sum += *closest - t;
        }
    }
    MESSAGE(what << ": " << detected.size() << " onsets for " << expected.size() << ", mean offset "
                 << sum / double(expected.size()) * 1000.0 << " ms, worst " << worst * 1000.0 << " ms");
}

std::vector<double> times(int count, double (TechnoLoop::*at)(int) const, const TechnoLoop& loop, int every = 1,
                          int offset = 0)
{
    std::vector<double> t;
    for (int i = offset; i < count; i += every)
        t.push_back((loop.*at)(i));
    return t;
}

} // namespace

TEST_CASE("a techno loop yields kick, clap and hat onsets on their grid")
{
    TechnoLoop loop;
    for (double gain : {1.0, 0.03}) {
        CAPTURE(gain);
        loop.gain = gain;
        const Onsets onsets = analyse(loop.render());
        const double from = loop.kick(4);
        const std::vector<double> claps = times(loop.beats(), &TechnoLoop::kick, loop, 2, 1);
        checkGrid(onsets.times[0], times(loop.beats(), &TechnoLoop::kick, loop), "kick", from);
        checkGrid(onsets.times[1], claps, "clap", from);
        // A clap's top end is a transient above 7 kHz too.
        checkGrid(onsets.times[2], times(loop.beats(), &TechnoLoop::hat, loop), "hat", from, claps);
    }
}

TEST_CASE("the analysis does not depend on how the audio is chunked")
{
    const std::vector<float> samples = TechnoLoop{.bars = 2}.render();
    const Onsets a = analyse(samples, {}, 1);
    const Onsets b = analyse(samples, {}, 2);
    REQUIRE(a.frames.size() == b.frames.size());
    for (size_t i = 0; i < a.frames.size(); ++i) {
        CHECK(a.frames[i].time == doctest::Approx(b.frames[i].time).epsilon(1e-9));
        CHECK(a.frames[i].values == b.frames[i].values);
        CHECK(a.frames[i].spectrum == b.frames[i].spectrum);
    }
}

TEST_CASE("silence and steady noise give no onsets and low levels")
{
    std::vector<float> samples(size_t(TechnoLoop::kRate * 4), 0.0f);
    Onsets silent = analyse(samples);
    CHECK(silent.times[0].empty());
    CHECK(silent.times[1].empty());
    CHECK(silent.times[2].empty());
    CHECK(silent.frames.back().values[size_t(AudioFeature::Level)] == 0.0f);

    uint32_t seed = 7;
    for (float& s : samples) {
        seed = seed * 1664525u + 1013904223u;
        s = 0.1f * (float(seed >> 8) / float(1 << 23) - 1.0f);
    }
    Onsets noise = analyse(samples);
    for (size_t k = 0; k < azl::kOnsetKinds; ++k)
        CHECK(noise.times[k].size() <= 1);
}

TEST_CASE("band envelopes follow the kick with frame-rate independent attack and release")
{
    TechnoLoop loop{.bars = 4};
    const Onsets onsets = analyse(loop.render());
    const auto at = [&](double t) {
        return std::lower_bound(onsets.frames.begin(), onsets.frames.end(), t,
                                [](const AudioFrame& frame, double time) { return frame.time < time; });
    };
    const auto value = [&](double t, AudioFeature f) { return at(t)->values[size_t(f)]; };
    const auto peak = [&](double from, double to, AudioFeature f) {
        float most = 0.0f;
        for (auto it = at(from); it != at(to); ++it)
            most = std::max(most, it->values[size_t(f)]);
        return most;
    };
    for (int i = 4; i < loop.beats(); ++i) {
        CAPTURE(i);
        CHECK(peak(loop.kick(i), loop.kick(i) + 0.15, AudioFeature::Sub) > 0.7f);
        CHECK(peak(loop.kick(i), loop.kick(i) + 0.05, AudioFeature::Bass) > 0.7f);
        CHECK(value(loop.kick(i) - 0.005, AudioFeature::Bass) < 0.4f);
        CHECK(value(loop.kick(i) + 0.03, AudioFeature::Kick) > 0.7f);
        CHECK(value(loop.kick(i) - 0.005, AudioFeature::Kick) < 0.2f);
        CHECK(peak(loop.hat(i), loop.hat(i) + 0.03, AudioFeature::High) > 0.6f);
    }
    CHECK(onsets.frames.back().gainDb > 0.0f);
}

TEST_CASE("readers sample the published analysis at any frame rate, delayed by the latency offset")
{
    TechnoLoop loop{.bars = 2};
    const std::vector<float> samples = loop.render();
    const double t0 = 100.0;
    const size_t chunk = 256;
    const double chunkSeconds = chunk / TechnoLoop::kRate;
    for (double fps : {30.0, 60.0, 144.0}) {
        CAPTURE(fps);
        azl::AudioAnalysis analysis([] { return 0.0; }, {.thread = false});
        size_t pushed = 0;
        std::vector<double> seen;
        uint64_t last = 0;
        double t = t0;
        for (; t < t0 + loop.beats() * loop.beat(); t += 1.0 / fps) {
            for (; pushed + chunk <= samples.size() && double(pushed + chunk) / TechnoLoop::kRate <= t - t0; pushed += chunk)
                analysis.push(samples.data() + pushed, chunk, t0 + double(pushed) / TechnoLoop::kRate, TechnoLoop::kRate);
            analysis.pump();
            const AudioFrame frame = analysis.at(t);
            CHECK(frame.time <= t);
            if (frame.onsets[0] != last) {
                last = frame.onsets[0];
                seen.push_back(t - t0);
            }
        }
        REQUIRE(seen.size() == size_t(loop.beats()));
        for (size_t k = 0; k < seen.size(); ++k)
            CHECK(std::abs(seen[k] - loop.kick(int(k))) < kTolerance + chunkSeconds + 1.0 / fps);

        const double probe = t - 0.5;
        const AudioFrame now = analysis.at(probe);
        analysis.setSettings({.latencyMs = 100});
        CHECK(analysis.at(probe + 0.1).time == now.time);
    }
}

TEST_CASE("without input the analysis runs on silence and every feature releases to zero")
{
    TechnoLoop loop{.bars = 1};
    const std::vector<float> samples = loop.render();
    azl::AudioAnalysis analysis([] { return 0.0; }, {.thread = false});
    analysis.push(samples.data(), 4096, 10.0, TechnoLoop::kRate);
    analysis.pump();
    const AudioFrame loud = analysis.latest();
    CHECK(loud.live);
    CHECK(loud.values[size_t(AudioFeature::Level)] > 0.1f);
    analysis.pump(14.0);
    const AudioFrame quiet = analysis.latest();
    CHECK_FALSE(quiet.live);
    CHECK(quiet.time > 13.9);
    for (float v : quiet.values)
        CHECK(v < 0.01f);
}

TEST_CASE("exact zeros for two seconds count as digital silence; a full ring drops chunks instead of blocking")
{
    azl::AudioAnalysis analysis([] { return 0.0; }, {.thread = false});
    std::vector<float> zeros(4096, 0.0f);
    for (int i = 0; i < 30; ++i)
        analysis.push(zeros.data(), zeros.size(), i * 4096 / 48000.0, 48000.0);
    CHECK(analysis.digitalSilence());
    CHECK(analysis.overruns() > 0);
    analysis.pump();
    zeros[0] = 0.001f;
    analysis.push(zeros.data(), zeros.size(), 3.0, 48000.0);
    CHECK_FALSE(analysis.digitalSilence());
}

namespace {

constexpr double kRate = 48000.0;
constexpr size_t kLow = size_t(AudioFeature::LowBand);
constexpr size_t kMid = size_t(AudioFeature::MidBand);
constexpr size_t kHigh = size_t(AudioFeature::HighBand);

// A fixed gain that reads a sine of this amplitude (RMS -12 dBFS) as 1, so a band's level is its share of the tone.
constexpr double kFullScale = 0.25 * M_SQRT2;

azl::AudioSettings manualGain()
{
    azl::AudioSettings settings;
    settings.autoGain = false;
    return settings;
}

// A sine whose frequency moves from `from` to `to` Hz over the samples, exponentially.
std::vector<float> sweep(double from, double to, double seconds)
{
    std::vector<float> samples(size_t(seconds * kRate));
    double phase = 0.0;
    for (size_t i = 0; i < samples.size(); ++i) {
        samples[i] = float(kFullScale * std::sin(phase));
        phase += 2.0 * M_PI * from * std::pow(to / from, double(i) / double(samples.size())) / kRate;
    }
    return samples;
}

std::vector<AudioFrame> run(azl::AudioAnalyzer& analyzer, const std::vector<float>& samples, double start = 0.0)
{
    std::vector<AudioFrame> frames;
    for (size_t i = 0; i < samples.size(); i += 512)
        analyzer.process(samples.data() + i, std::min<size_t>(512, samples.size() - i), start + double(i) / kRate, true,
                         [&](const AudioFrame& frame) { frames.push_back(frame); });
    return frames;
}

} // namespace

TEST_CASE("audio bands: a tone in each band lights only that band; the defaults are fixed band edges")
{
    CHECK(azl::AudioAnalyzer::kDefaultEdges == azl::kDefaultBandEdges);
    CHECK(azl::audioFeatureFromName("midBand") == AudioFeature::MidBand);
    CHECK(std::string(azl::audioFeatureName(AudioFeature::HighBand)) == "highBand");
    struct Case {
        double hz;
        size_t lit;
    };
    for (const Case c : {Case{50.0, kLow}, Case{500.0, kMid}, Case{8000.0, kHigh}}) {
        CAPTURE(c.hz);
        azl::AudioAnalyzer analyzer(kRate, manualGain());
        const AudioFrame last = run(analyzer, sweep(c.hz, c.hz, 1.0)).back();
        for (const size_t band : {kLow, kMid, kHigh}) {
            CAPTURE(band);
            if (band == c.lit)
                CHECK(last.values[band] > 0.9f);
            else
                CHECK(last.values[band] < 0.05f);
        }
    }
}

TEST_CASE("audio bands: tones on either side of an edge fall in its two bands, and the edge moves with its setting")
{
    for (const double hz : {300.0, 1200.0}) {
        CAPTURE(hz);
        azl::AudioAnalyzer below(kRate, manualGain(), {150.0, 600.0});
        azl::AudioAnalyzer above(kRate, manualGain(), {150.0, 3000.0});
        const AudioFrame b = run(below, sweep(hz, hz, 0.5)).back();
        const AudioFrame a = run(above, sweep(hz, hz, 0.5)).back();
        if (hz < 600.0) {
            CHECK(b.values[kMid] > 0.8f);
            CHECK(b.values[kHigh] < 0.2f);
        } else {
            CHECK(b.values[kHigh] > 0.8f);
            CHECK(b.values[kMid] < 0.2f);
        }
        CHECK(a.values[kMid] > 0.8f);
        CHECK(a.values[kHigh] < 0.2f);
    }
}

TEST_CASE("audio bands: a sweep across an edge hands over from one band to the next without a gap")
{
    azl::AudioAnalyzer analyzer(kRate, manualGain());
    const std::vector<AudioFrame> frames = run(analyzer, sweep(500.0, 8000.0, 4.0));
    double handover = 0.0;
    for (size_t i = frames.size() / 10; i < frames.size(); ++i) {
        const float mid = frames[i].values[kMid];
        const float high = frames[i].values[kHigh];
        CAPTURE(frames[i].time);
        CHECK(mid + high > 0.85f);
        CHECK(mid + high < 1.2f);
        if (handover == 0.0 && high >= mid)
            handover = frames[i].time;
    }
    // The bands cross where the sweep passes 2 kHz.
    const double crossing = 4.0 * std::log(2000.0 / 500.0) / std::log(8000.0 / 500.0);
    CHECK(handover == doctest::Approx(crossing).epsilon(0.05));
}

TEST_CASE("audio bands: moving an edge across a tone glides its level from one band to the other without a jump")
{
    const auto steepest = [](const std::vector<AudioFrame>& frames, size_t band) {
        float step = 0.0f;
        for (size_t i = 1; i < frames.size(); ++i)
            step = std::max(step, std::abs(frames[i].values[band] - frames[i - 1].values[band]));
        return step;
    };
    azl::AudioAnalyzer analyzer(kRate, manualGain());
    run(analyzer, sweep(1000.0, 1000.0, 0.5));
    analyzer.setBandEdges({150.0, 500.0});
    const std::vector<AudioFrame> frames = run(analyzer, sweep(1000.0, 1000.0, 1.0), 0.5);
    CHECK(frames.front().values[kMid] > 0.9f);
    double crossed = 0.0;
    for (const AudioFrame& frame : frames)
        if (crossed == 0.0 && frame.values[kHigh] >= frame.values[kMid])
            crossed = frame.time - 0.5;
    // Over 2.7 ms hops: an edge set at once would move almost the whole level in one.
    CHECK(steepest(frames, kMid) < 0.04f);
    CHECK(steepest(frames, kHigh) < 0.04f);
    CHECK(crossed > 0.05);
    CHECK(frames.back().values[kHigh] > 0.9f);
    // A 24 dB/octave slope an octave past the edge.
    CHECK(frames.back().values[kMid] < 0.08f);
    CHECK(analyzer.bandEdges() == azl::AudioAnalyzer::BandEdges{150.0, 500.0});
}
