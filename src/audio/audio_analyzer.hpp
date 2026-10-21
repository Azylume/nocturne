#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace azl {

// Bands in Hz: sub 20-60, bass 60-150, lowMid 150-500, mid 500-2k, highMid 2k-6k, high 6k-16k. Kick, snare and hat
// are onset pulses: 1 at the onset (after the attack ramp), then an exponential release. LowBand, MidBand and HighBand
// split 20 Hz-16 kHz at two adjustable edges, with 24 dB/octave slopes that cross at -6 dB.
enum class AudioFeature : uint8_t {
    Level,
    Sub,
    Bass,
    LowMid,
    Mid,
    HighMid,
    High,
    Kick,
    Snare,
    Hat,
    LowBand,
    MidBand,
    HighBand
};
constexpr size_t kAudioFeatures = 13;
constexpr size_t kAudioBands = 6;
constexpr size_t kSplitBands = 3;
constexpr size_t kOnsetKinds = 3;
constexpr size_t kSpectrumBands = 24;

const char* audioFeatureName(AudioFeature feature);
std::optional<AudioFeature> audioFeatureFromName(std::string_view name);

struct AudioEnvelope {
    double attackMs = 0.0;
    double releaseMs = 0.0;
};

struct AudioSettings {
    // Auto gain normalises each feature by its own recent peak (a peak tracker with a few seconds of release), so
    // quiet and loud inputs both fill 0..1. Off, `gainDb` scales a fixed reference instead.
    bool autoGain = true;
    double gainDb = 0.0;
    // Readers see the analysis this much later, so reactions line up with what the room hears.
    double latencyMs = 0.0;
    std::array<AudioEnvelope, kAudioFeatures> envelopes = {{{5, 150},
                                                            {5, 250},
                                                            {5, 200},
                                                            {5, 150},
                                                            {5, 120},
                                                            {3, 100},
                                                            {2, 80},
                                                            {0, 150},
                                                            {0, 150},
                                                            {0, 70},
                                                            {5, 200},
                                                            {5, 150},
                                                            {3, 100}}};

    static constexpr double kMaxLatencyMs = 1000.0;
    static constexpr double kMaxEnvelopeMs = 10000.0;
};

// The analysis after one hop of audio.
struct AudioFrame {
    // Engine time of the hop's last sample.
    double time = 0.0;
    // 0..1, indexed by AudioFeature.
    std::array<float, kAudioFeatures> values{};
    // Onsets so far: kick, snare, hat.
    std::array<uint64_t, kOnsetKinds> onsets{};
    // Log-spaced bands from 30 Hz to 16 kHz, 0..1 over 60 dB, after gain.
    std::array<float, kSpectrumBands> spectrum{};
    // The gain auto gain applies to the full-band level, or the manual gain.
    float gainDb = 0.0f;
    // False while the analysis runs on silence because no audio arrives.
    bool live = false;
};

// Analyses mono audio in hops of about 2.7 ms, independent of how it is chunked: band envelopes and onsets come from
// IIR filters defined in Hz, and the spectrum from a 2048-point FFT with 75 % overlap. Single-threaded.
class AudioAnalyzer {
public:
    // The Low/Mid and Mid/High edges in Hz of LowBand, MidBand and HighBand.
    using BandEdges = std::array<double, 2>;
    static constexpr BandEdges kDefaultEdges{150.0, 2000.0};
    // How fast the edges glide to new ones, in octaves per second.
    static constexpr double kEdgeGlide = 8.0;

    explicit AudioAnalyzer(double sampleRate, const AudioSettings& settings = {}, BandEdges edges = kDefaultEdges);
    ~AudioAnalyzer();
    AudioAnalyzer(const AudioAnalyzer&) = delete;
    AudioAnalyzer& operator=(const AudioAnalyzer&) = delete;

    void setSettings(const AudioSettings& settings) { settings_ = settings; }
    // The edges glide there, so the bands' levels move without a jump.
    void setBandEdges(BandEdges edges) { edgeTargets_ = edges; }
    BandEdges bandEdges() const { return edges_; }
    double sampleRate() const { return sampleRate_; }
    size_t hop() const { return hop_; }

    // `time` is the engine time of samples[0]. Calls onFrame(const AudioFrame&) after each hop.
    template <typename OnFrame>
    void process(const float* samples, size_t count, double time, bool live, OnFrame&& onFrame)
    {
        for (size_t i = 0; i < count; ++i)
            if (sample(samples[i])) {
                endHop(time + double(i) / sampleRate_, live);
                onFrame(static_cast<const AudioFrame&>(frame_));
            }
    }

    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1 = 0, z2 = 0;
        double run(double x)
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
private:
    struct Filter {
        std::vector<Biquad> stages;
        double run(double x)
        {
            for (Biquad& b : stages)
                x = b.run(x);
            return x;
        }
    };
    struct Band {
        Filter filter;
        double sum = 0.0;
        double peak = 0.0;
    };
    struct Pulse {
        double value = 0.0;
        bool rising = false;
    };
    struct Drum {
        Filter filter;
        double smoothing = 0.0;
        double power = 0.0;
        double peak = 0.0;
        std::array<double, 32> history{};
        size_t written = 0;
        size_t lag = 0;
        double riseDb = 0.0;
        double gateDb = 0.0;
        double refractory = 0.0;
        double since = 1e9;
        bool armed = true;
        Pulse pulse;
    };
    struct Fft;

    bool sample(float x);
    void endHop(double time, bool live);
    bool detect(Drum& drum);
    void spectrum(float gainDb);
    double smooth(double value, double target, const AudioEnvelope& envelope) const;
    double reference(double peak) const;
    void bandLevel(Band& band, AudioFeature feature);
    void glideEdges();
    void tuneSplit();

    double sampleRate_;
    size_t hop_;
    double hopSeconds_;
    AudioSettings settings_;
    double peakRelease_;
    std::array<Band, kAudioBands> bands_;
    std::array<Band, kSplitBands> split_;
    BandEdges edges_;
    BandEdges edgeTargets_;
    Band full_;
    std::array<Drum, kOnsetKinds> drums_;
    size_t inHop_ = 0;
    std::unique_ptr<Fft> fft_;
    AudioFrame frame_;
};

} // namespace azl
