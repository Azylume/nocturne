#include "audio/audio_analyzer.hpp"

#include <Accelerate/Accelerate.h>

#include <algorithm>
#include <cmath>

namespace azl {

namespace {

constexpr std::array<const char*, kAudioFeatures> kNames = {"level", "sub",   "bass", "lowMid",  "mid",
                                                            "highMid", "high", "kick", "snare",  "hat",
                                                            "lowBand", "midBand", "highBand"};
constexpr std::array<std::pair<double, double>, kAudioBands> kBands = {
    {{20, 60}, {60, 150}, {150, 500}, {500, 2000}, {2000, 6000}, {6000, 16000}}};

constexpr double kHopsPerSecond = 375.0;
constexpr double kPeakReleaseSeconds = 4.0;
// The lowest peak auto gain normalises by: -54 dBFS RMS, so silence and room tone stay low.
constexpr double kAgcFloor = 0.002;
// Manual gain: an RMS of -12 dBFS reads 1 at 0 dB.
constexpr double kManualReference = 0.25;
constexpr double kSilenceDb = -70.0;

constexpr size_t kFftLog2 = 11;
constexpr size_t kFftSize = size_t(1) << kFftLog2;
constexpr size_t kFftHop = kFftSize / 4;
constexpr double kSpectrumLow = 30.0;
constexpr double kSpectrumHigh = 16000.0;
constexpr double kSpectrumRangeDb = 60.0;

enum class Pass { Low, High };

AudioAnalyzer::Biquad butterworth(Pass pass, double hz, double sampleRate)
{
    const double w = 2.0 * M_PI * std::min(hz, 0.45 * sampleRate) / sampleRate;
    const double alpha = std::sin(w) / (2.0 * M_SQRT1_2);
    const double c = std::cos(w);
    const double a0 = 1.0 + alpha;
    AudioAnalyzer::Biquad b;
    if (pass == Pass::Low) {
        b.b0 = b.b2 = (1.0 - c) / 2.0 / a0;
        b.b1 = (1.0 - c) / a0;
    } else {
        b.b0 = b.b2 = (1.0 + c) / 2.0 / a0;
        b.b1 = -(1.0 + c) / a0;
    }
    b.a1 = -2.0 * c / a0;
    b.a2 = (1.0 - alpha) / a0;
    return b;
}

double toDb(double power)
{
    return 10.0 * std::log10(power + 1e-12);
}

double coefficient(double ms, double seconds)
{
    return ms <= 0.0 ? 0.0 : std::exp(-seconds / (ms / 1000.0));
}

} // namespace

const char* audioFeatureName(AudioFeature feature)
{
    return kNames[size_t(feature)];
}

std::optional<AudioFeature> audioFeatureFromName(std::string_view name)
{
    for (size_t i = 0; i < kAudioFeatures; ++i)
        if (name == kNames[i])
            return AudioFeature(i);
    return std::nullopt;
}

struct AudioAnalyzer::Fft {
    explicit Fft(double sampleRate) : setup(vDSP_create_fftsetup(kFftLog2, kFFTRadix2))
    {
        vDSP_hann_window(window.data(), kFftSize, vDSP_HANN_NORM);
        const double binHz = sampleRate / double(kFftSize);
        const double high = std::min(kSpectrumHigh, 0.45 * sampleRate);
        size_t from = 1;
        for (size_t b = 0; b < kSpectrumBands; ++b) {
            const double edge = kSpectrumLow * std::pow(high / kSpectrumLow, double(b + 1) / kSpectrumBands);
            const size_t to = std::max(from + 1, size_t(std::ceil(edge / binHz)));
            bins[b] = {std::min(from, kFftSize / 2 - 1), std::min(to, kFftSize / 2)};
            from = to;
        }
    }
    ~Fft() { vDSP_destroy_fftsetup(setup); }

    FFTSetup setup;
    std::array<float, kFftSize> window{};
    std::array<float, kFftSize> input{};
    std::array<float, kFftSize> windowed{};
    std::array<float, kFftSize / 2> real{};
    std::array<float, kFftSize / 2> imag{};
    std::array<float, kFftSize / 2> power{};
    std::array<std::pair<size_t, size_t>, kSpectrumBands> bins{};
    size_t written = 0;
    size_t sinceTransform = 0;
};

AudioAnalyzer::AudioAnalyzer(double sampleRate, const AudioSettings& settings, BandEdges edges)
    : sampleRate_(sampleRate),
      hop_(std::max<size_t>(16, size_t(std::lround(sampleRate / kHopsPerSecond)))),
      hopSeconds_(double(hop_) / sampleRate),
      settings_(settings),
      peakRelease_(std::exp(-hopSeconds_ / kPeakReleaseSeconds)),
      edges_(edges),
      edgeTargets_(edges),
      fft_(std::make_unique<Fft>(sampleRate))
{
    for (size_t i = 0; i < kAudioBands; ++i)
        bands_[i].filter.stages = {butterworth(Pass::High, kBands[i].first, sampleRate),
                                   butterworth(Pass::Low, kBands[i].second, sampleRate)};
    split_[0].filter.stages.resize(3);
    split_[1].filter.stages.resize(4);
    split_[2].filter.stages.resize(3);
    tuneSplit();

    struct DrumSpec {
        std::vector<Biquad> stages;
        double smoothingMs;
        double lagMs;
        double riseDb;
        double gateDb;
        double refractoryMs;
    };
    const std::array<DrumSpec, kOnsetKinds> specs = {{
        {{butterworth(Pass::High, 35, sampleRate), butterworth(Pass::Low, 150, sampleRate),
          butterworth(Pass::Low, 150, sampleRate)},
         10, 12, 9, 10, 100},
        {{butterworth(Pass::High, 1000, sampleRate), butterworth(Pass::High, 1000, sampleRate),
          butterworth(Pass::Low, 4000, sampleRate)},
         4, 8, 6, 12, 100},
        {{butterworth(Pass::High, 7000, sampleRate), butterworth(Pass::High, 7000, sampleRate)}, 2, 6, 6, 15, 60},
    }};
    for (size_t i = 0; i < kOnsetKinds; ++i) {
        Drum& d = drums_[i];
        d.filter.stages = specs[i].stages;
        d.smoothing = std::exp(-1.0 / (specs[i].smoothingMs / 1000.0 * sampleRate));
        d.lag = std::clamp<size_t>(size_t(std::lround(specs[i].lagMs / 1000.0 / hopSeconds_)), 1, d.history.size() / 2 - 1);
        d.riseDb = specs[i].riseDb;
        d.gateDb = specs[i].gateDb;
        d.refractory = specs[i].refractoryMs / 1000.0;
    }
}

AudioAnalyzer::~AudioAnalyzer() = default;

bool AudioAnalyzer::sample(float in)
{
    const double x = in;
    full_.sum += x * x;
    for (Band& band : bands_) {
        const double y = band.filter.run(x);
        band.sum += y * y;
    }
    for (Band& band : split_) {
        const double y = band.filter.run(x);
        band.sum += y * y;
    }
    for (Drum& drum : drums_) {
        const double y = drum.filter.run(x);
        drum.power = y * y + (drum.power - y * y) * drum.smoothing;
    }
    Fft& f = *fft_;
    f.input[f.written++ % kFftSize] = in;
    ++f.sinceTransform;
    return ++inHop_ == hop_;
}

double AudioAnalyzer::reference(double peak) const
{
    return settings_.autoGain ? peak : kManualReference * std::pow(10.0, -settings_.gainDb / 20.0);
}

double AudioAnalyzer::smooth(double value, double target, const AudioEnvelope& envelope) const
{
    const double k = coefficient(target > value ? envelope.attackMs : envelope.releaseMs, hopSeconds_);
    return target + (value - target) * k;
}

void AudioAnalyzer::bandLevel(Band& band, AudioFeature feature)
{
    const double rms = std::sqrt(band.sum / double(hop_));
    band.sum = 0.0;
    band.peak = std::max(rms, std::max(band.peak * peakRelease_, kAgcFloor));
    const size_t f = size_t(feature);
    frame_.values[f] = float(smooth(frame_.values[f], std::min(1.0, rms / reference(band.peak)), settings_.envelopes[f]));
}

void AudioAnalyzer::glideEdges()
{
    if (edges_ == edgeTargets_)
        return;
    const double step = kEdgeGlide * hopSeconds_;
    for (size_t i = 0; i < 2; ++i) {
        const double octaves = std::log2(edgeTargets_[i] / edges_[i]);
        edges_[i] = std::abs(octaves) <= step ? edgeTargets_[i] : edges_[i] * std::exp2(std::copysign(step, octaves));
    }
    tuneSplit();
}

// Linkwitz-Riley crossovers (two Butterworth stages per edge), retuned in place so the filters keep their state.
void AudioAnalyzer::tuneSplit()
{
    auto tune = [this](Biquad& b, Pass pass, double hz) {
        const Biquad t = butterworth(pass, hz, sampleRate_);
        b.b0 = t.b0;
        b.b1 = t.b1;
        b.b2 = t.b2;
        b.a1 = t.a1;
        b.a2 = t.a2;
    };
    std::vector<Biquad>& low = split_[0].filter.stages;
    std::vector<Biquad>& mid = split_[1].filter.stages;
    std::vector<Biquad>& high = split_[2].filter.stages;
    tune(low[0], Pass::High, 20.0);
    tune(low[1], Pass::Low, edges_[0]);
    tune(low[2], Pass::Low, edges_[0]);
    tune(mid[0], Pass::High, edges_[0]);
    tune(mid[1], Pass::High, edges_[0]);
    tune(mid[2], Pass::Low, edges_[1]);
    tune(mid[3], Pass::Low, edges_[1]);
    tune(high[0], Pass::High, edges_[1]);
    tune(high[1], Pass::High, edges_[1]);
    tune(high[2], Pass::Low, 16000.0);
}

bool AudioAnalyzer::detect(Drum& d)
{
    const double db = toDb(d.power);
    const size_t n = d.history.size();
    if (d.written == 0)
        d.history.fill(db);
    // Against the loudest hop of the stretch before the lag, so noise flickering upward does not count as a rise.
    double baseline = d.history[(d.written + n - d.lag) % n];
    for (size_t k = d.lag + 1; k <= 2 * d.lag; ++k)
        baseline = std::max(baseline, d.history[(d.written + n - k) % n]);
    const double rise = db - baseline;
    d.history[d.written++ % n] = db;
    d.peak = std::max(d.power, std::max(d.peak * peakRelease_, kAgcFloor * kAgcFloor * 0.1));
    d.since += hopSeconds_;
    const bool gate = db > toDb(d.peak) - d.gateDb && db > kSilenceDb;
    if (rise < d.riseDb * 0.5)
        d.armed = true;
    if (!d.armed || rise <= d.riseDb || !gate || d.since < d.refractory)
        return false;
    d.armed = false;
    d.since = 0.0;
    return true;
}

void AudioAnalyzer::endHop(double time, bool live)
{
    inHop_ = 0;
    frame_.time = time;
    frame_.live = live;

    const double fullRms = std::sqrt(full_.sum / double(hop_));
    full_.sum = 0.0;
    full_.peak = std::max(fullRms, std::max(full_.peak * peakRelease_, kAgcFloor));
    const auto level = size_t(AudioFeature::Level);
    frame_.values[level] = float(smooth(frame_.values[level], std::min(1.0, fullRms / reference(full_.peak)),
                                        settings_.envelopes[level]));
    frame_.gainDb = float(settings_.autoGain ? -20.0 * std::log10(full_.peak) : settings_.gainDb);

    for (size_t i = 0; i < kAudioBands; ++i)
        bandLevel(bands_[i], AudioFeature(size_t(AudioFeature::Sub) + i));
    for (size_t i = 0; i < kSplitBands; ++i)
        bandLevel(split_[i], AudioFeature(size_t(AudioFeature::LowBand) + i));
    glideEdges();

    for (size_t i = 0; i < kOnsetKinds; ++i) {
        Drum& drum = drums_[i];
        const size_t f = size_t(AudioFeature::Kick) + i;
        const AudioEnvelope& envelope = settings_.envelopes[f];
        if (detect(drum)) {
            ++frame_.onsets[i];
            drum.pulse.rising = true;
        }
        Pulse& p = drum.pulse;
        if (p.rising) {
            p.value = envelope.attackMs <= 0.0 ? 1.0 : p.value + hopSeconds_ / (envelope.attackMs / 1000.0);
            if (p.value >= 1.0) {
                p.value = 1.0;
                p.rising = false;
            }
        } else {
            p.value *= coefficient(envelope.releaseMs, hopSeconds_);
        }
        frame_.values[f] = float(p.value);
    }

    if (fft_->sinceTransform >= kFftHop && fft_->written >= kFftSize)
        spectrum(frame_.gainDb);
}

void AudioAnalyzer::spectrum(float gainDb)
{
    Fft& f = *fft_;
    f.sinceTransform = 0;
    const size_t start = f.written % kFftSize;
    for (size_t i = 0; i < kFftSize; ++i)
        f.windowed[i] = f.input[(start + i) % kFftSize] * f.window[i];
    DSPSplitComplex split{f.real.data(), f.imag.data()};
    vDSP_ctoz(reinterpret_cast<const DSPComplex*>(f.windowed.data()), 2, &split, 1, kFftSize / 2);
    vDSP_fft_zrip(f.setup, &split, 1, kFftLog2, kFFTDirection_Forward);
    split.imagp[0] = 0.0f;
    vDSP_zvmags(&split, 1, f.power.data(), 1, kFftSize / 2);
    // Each bin's share of the windowed signal's mean square (vDSP's transform is scaled by 2; Hann's mean square is
    // 3/8).
    const double scale = 1.0 / (2.0 * double(kFftSize) * double(kFftSize) * 0.375);
    const double gain = settings_.autoGain ? gainDb : settings_.gainDb + 20.0 * std::log10(1.0 / kManualReference);
    for (size_t b = 0; b < kSpectrumBands; ++b) {
        double sum = 0.0;
        for (size_t k = f.bins[b].first; k < f.bins[b].second; ++k)
            sum += f.power[k];
        const double db = toDb(sum * scale) + gain;
        frame_.spectrum[b] = float(std::clamp((db + kSpectrumRangeDb) / kSpectrumRangeDb, 0.0, 1.0));
    }
}

} // namespace azl
