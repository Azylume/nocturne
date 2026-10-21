#pragma once

#include "audio/audio_analyzer.hpp"
#include "audio/spsc_ring.hpp"

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace azl {

// One input channel before mixing and gain. `noise` compares the power of the sample-to-sample change with the
// power itself: about 1 for white noise such as an idle preamp's hiss, near 0 for music, whose power sits low.
struct ChannelLevel {
    float levelDb = -120.0f;
    float noise = 0.0f;
};

// Measures one channel over a window. Audio thread, wait-free.
struct ChannelMeter {
    double power = 0.0;
    double change = 0.0;
    float last = 0.0f;
    size_t count = 0;

    void add(float x)
    {
        power += double(x) * x;
        change += double(x - last) * (x - last);
        last = x;
        ++count;
    }
    // The window so far; starts the next one.
    ChannelLevel take();
};

// The bridge from an audio callback to every reader. The callback pushes samples into a lock-free ring without
// blocking or allocating; an analysis thread drains it into an AudioAnalyzer and publishes each hop into a history
// that the render thread and the protocol read without locks. When no audio arrives, the analysis carries on over
// silence, so every feature releases to zero instead of freezing.
class AudioAnalysis {
public:
    using Now = std::function<double()>;
    // Sees the captured samples as the analysis drains them: mono, the first at engine time `time`.
    using Tap = std::function<void(const float* samples, size_t count, double time, double sampleRate)>;

    struct Options {
        // Off for tests, which call pump() instead.
        bool thread = true;
        // How long the input may stay quiet before the analysis runs on silence.
        double starveSeconds = 0.1;
    };

    AudioAnalysis(Now now, Options options);
    explicit AudioAnalysis(Now now) : AudioAnalysis(std::move(now), Options{}) {}
    ~AudioAnalysis();

    // Audio thread only, wait-free: mono samples at `sampleRate`, the first captured at engine time `time`. A chunk
    // that does not fit is dropped and counted.
    void push(const float* samples, size_t count, double time, double sampleRate);

    // Any thread, never blocks: the analysis as of engine time `seconds`, less the latency offset.
    AudioFrame at(double seconds) const;
    AudioFrame latest() const;

    // Any thread: from now on `tap` also gets every chunk the analysis drains, on the analysis thread (or pump's
    // caller), never on the audio thread; it must not block. Each owner has one tap; null removes it. Once it returns,
    // the owner's previous tap is no longer running.
    void setTap(const void* owner, Tap tap);

    void setSettings(const AudioSettings& settings);
    AudioSettings settings() const;
    // Any thread: where LowBand, MidBand and HighBand split; the composition's, as the engine sets them.
    void setBandEdges(AudioAnalyzer::BandEdges edges);
    AudioAnalyzer::BandEdges bandEdges() const;
    double now() const { return now_(); }

    // Analyses what was pushed, on the caller's thread; with `until`, then runs on silence up to that engine time.
    void pump(double until = 0.0);

    uint64_t overruns() const { return overruns_.load(std::memory_order_relaxed); }
    // The last pushed samples were exact zeros for over two seconds, as when microphone access is denied.
    bool digitalSilence() const { return digitalSilence_.load(std::memory_order_relaxed); }
    double sampleRate() const { return sampleRate_.load(std::memory_order_relaxed); }

    static constexpr size_t kMaxChannels = 64;
    // Audio thread, wait-free: each of the input device's channels; none while no device runs.
    void setChannels(const ChannelLevel* levels, size_t count);
    std::vector<ChannelLevel> channels() const;

private:
    struct Chunk {
        uint32_t count;
        double time;
        double sampleRate;
    };
    struct Slot {
        std::atomic<uint64_t> sequence{0};
        AudioFrame frame;
    };
    static constexpr size_t kMaxChunk = 4096;
    static constexpr size_t kHistory = 1024;

    void run();
    bool drain();
    void silence(double until);
    void analyse(const float* samples, size_t count, double time, double sampleRate, bool live);
    void publish(const AudioFrame& frame);
    bool read(uint64_t index, AudioFrame& frame) const;

    Now now_;
    Options options_;
    SpscRing<float, 1 << 16> samples_;
    SpscRing<Chunk, 1024> chunks_;
    std::atomic<uint64_t> overruns_{0};
    std::atomic<bool> digitalSilence_{false};
    std::atomic<double> sampleRate_{0.0};
    uint64_t zeros_ = 0;
    std::array<std::atomic<float>, kMaxChannels> channelDb_{};
    std::array<std::atomic<float>, kMaxChannels> channelNoise_{};
    std::atomic<size_t> channelCount_{0};

    std::mutex tapMutex_;
    std::map<const void*, Tap> taps_;

    mutable std::mutex settingsMutex_;
    AudioSettings settings_;
    AudioAnalyzer::BandEdges bandEdges_ = AudioAnalyzer::kDefaultEdges;
    std::atomic<uint64_t> settingsVersion_{0};
    std::atomic<double> latencySeconds_{0.0};

    // Analysis thread only.
    std::unique_ptr<AudioAnalyzer> analyzer_;
    uint64_t appliedVersion_ = 0;
    double analysedUntil_ = 0.0;
    double lastAudio_ = 0.0;
    std::array<float, kMaxChunk> scratch_{};

    std::array<Slot, kHistory> history_;
    std::atomic<uint64_t> published_{0};

    std::atomic<bool> running_{true};
    std::thread thread_;
};

} // namespace azl
