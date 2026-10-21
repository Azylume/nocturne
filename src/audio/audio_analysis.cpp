#include "audio/audio_analysis.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace azl {

namespace {

constexpr double kSilenceRate = 48000.0;
constexpr double kDigitalSilenceSeconds = 2.0;
constexpr double kMaxCatchUpSeconds = 1.0;

} // namespace

ChannelLevel ChannelMeter::take()
{
    ChannelLevel level;
    if (count > 0 && power > 0.0) {
        level.levelDb = float(std::max(-120.0, 10.0 * std::log10(power / double(count))));
        level.noise = float(std::min(1.0, change / (2.0 * power)));
    }
    *this = {.last = last};
    return level;
}

void AudioAnalysis::setChannels(const ChannelLevel* levels, size_t count)
{
    count = std::min(count, kMaxChannels);
    for (size_t i = 0; i < count; ++i) {
        channelDb_[i].store(levels[i].levelDb, std::memory_order_relaxed);
        channelNoise_[i].store(levels[i].noise, std::memory_order_relaxed);
    }
    channelCount_.store(count, std::memory_order_release);
}

std::vector<ChannelLevel> AudioAnalysis::channels() const
{
    std::vector<ChannelLevel> result(channelCount_.load(std::memory_order_acquire));
    for (size_t i = 0; i < result.size(); ++i)
        result[i] = {channelDb_[i].load(std::memory_order_relaxed), channelNoise_[i].load(std::memory_order_relaxed)};
    return result;
}

AudioAnalysis::AudioAnalysis(Now now, Options options) : now_(std::move(now)), options_(options)
{
    if (options_.thread)
        thread_ = std::thread([this] { run(); });
}

AudioAnalysis::~AudioAnalysis()
{
    running_ = false;
    if (thread_.joinable())
        thread_.join();
}

void AudioAnalysis::push(const float* samples, size_t count, double time, double sampleRate)
{
    sampleRate_.store(sampleRate, std::memory_order_relaxed);
    bool zero = true;
    for (size_t i = 0; i < count && zero; ++i)
        zero = samples[i] == 0.0f;
    zeros_ = zero ? zeros_ + count : 0;
    digitalSilence_.store(double(zeros_) > kDigitalSilenceSeconds * sampleRate, std::memory_order_relaxed);

    for (size_t done = 0; done < count;) {
        const size_t n = std::min(count - done, kMaxChunk);
        if (chunks_.writable() == 0 || samples_.writable() < n) {
            overruns_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        samples_.write(samples + done, n);
        const Chunk chunk{uint32_t(n), time + double(done) / sampleRate, sampleRate};
        chunks_.write(&chunk, 1);
        done += n;
    }
}

void AudioAnalysis::setSettings(const AudioSettings& settings)
{
    std::lock_guard lock(settingsMutex_);
    settings_ = settings;
    latencySeconds_.store(settings.latencyMs / 1000.0, std::memory_order_relaxed);
    settingsVersion_.fetch_add(1, std::memory_order_release);
}

AudioSettings AudioAnalysis::settings() const
{
    std::lock_guard lock(settingsMutex_);
    return settings_;
}

void AudioAnalysis::setBandEdges(AudioAnalyzer::BandEdges edges)
{
    std::lock_guard lock(settingsMutex_);
    if (edges == bandEdges_)
        return;
    bandEdges_ = edges;
    settingsVersion_.fetch_add(1, std::memory_order_release);
}

AudioAnalyzer::BandEdges AudioAnalysis::bandEdges() const
{
    std::lock_guard lock(settingsMutex_);
    return bandEdges_;
}

void AudioAnalysis::run()
{
    while (running_) {
        const double now = now_();
        if (drain())
            lastAudio_ = now;
        else if (now - lastAudio_ > options_.starveSeconds)
            silence(now - options_.starveSeconds);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void AudioAnalysis::pump(double until)
{
    drain();
    if (until > 0.0)
        silence(until);
}

bool AudioAnalysis::drain()
{
    bool any = false;
    Chunk chunk;
    while (chunks_.read(&chunk, 1)) {
        samples_.read(scratch_.data(), chunk.count);
        analyse(scratch_.data(), chunk.count, chunk.time, chunk.sampleRate, true);
        {
            std::lock_guard lock(tapMutex_);
            for (const auto& [owner, tap] : taps_)
                tap(scratch_.data(), chunk.count, chunk.time, chunk.sampleRate);
        }
        any = true;
    }
    return any;
}

void AudioAnalysis::setTap(const void* owner, Tap tap)
{
    std::lock_guard lock(tapMutex_);
    if (tap)
        taps_[owner] = std::move(tap);
    else
        taps_.erase(owner);
}

void AudioAnalysis::silence(double until)
{
    const double rate = analyzer_ ? analyzer_->sampleRate() : kSilenceRate;
    analysedUntil_ = std::max(analysedUntil_, until - kMaxCatchUpSeconds);
    std::array<float, kMaxChunk> zeros{};
    while (analysedUntil_ < until) {
        const size_t n = std::min(kMaxChunk, size_t((until - analysedUntil_) * rate) + 1);
        analyse(zeros.data(), n, analysedUntil_, rate, false);
    }
}

void AudioAnalysis::analyse(const float* samples, size_t count, double time, double sampleRate, bool live)
{
    if (settingsVersion_.load(std::memory_order_acquire) != appliedVersion_ || !analyzer_ ||
        analyzer_->sampleRate() != sampleRate) {
        std::lock_guard lock(settingsMutex_);
        appliedVersion_ = settingsVersion_.load(std::memory_order_relaxed);
        if (!analyzer_ || analyzer_->sampleRate() != sampleRate) {
            analyzer_ = std::make_unique<AudioAnalyzer>(sampleRate, settings_, bandEdges_);
        } else {
            analyzer_->setSettings(settings_);
            analyzer_->setBandEdges(bandEdges_);
        }
    }
    time = std::max(time, analysedUntil_);
    analyzer_->process(samples, count, time, live, [this](const AudioFrame& frame) { publish(frame); });
    analysedUntil_ = time + double(count) / sampleRate;
}

void AudioAnalysis::publish(const AudioFrame& frame)
{
    const uint64_t index = published_.load(std::memory_order_relaxed);
    Slot& slot = history_[index % kHistory];
    slot.sequence.store(2 * index + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    slot.frame = frame;
    slot.sequence.store(2 * index + 2, std::memory_order_release);
    published_.store(index + 1, std::memory_order_release);
}

bool AudioAnalysis::read(uint64_t index, AudioFrame& frame) const
{
    const Slot& slot = history_[index % kHistory];
    const uint64_t before = slot.sequence.load(std::memory_order_acquire);
    if (before != 2 * index + 2)
        return false;
    frame = slot.frame;
    std::atomic_thread_fence(std::memory_order_acquire);
    return slot.sequence.load(std::memory_order_relaxed) == before;
}

AudioFrame AudioAnalysis::latest() const
{
    AudioFrame frame;
    for (;;) {
        const uint64_t n = published_.load(std::memory_order_acquire);
        if (n == 0 || read(n - 1, frame))
            return frame;
    }
}

AudioFrame AudioAnalysis::at(double seconds) const
{
    const double target = seconds - latencySeconds_.load(std::memory_order_relaxed);
    for (;;) {
        const uint64_t n = published_.load(std::memory_order_acquire);
        if (n == 0)
            return {};
        // Keep clear of the slots the writer may reuse while this searches.
        uint64_t low = n > kHistory - 64 ? n - (kHistory - 64) : 0;
        uint64_t high = n - 1;
        AudioFrame frame;
        bool torn = false;
        while (low < high) {
            const uint64_t mid = (low + high + 1) / 2;
            if (!read(mid, frame)) {
                torn = true;
                break;
            }
            if (frame.time <= target)
                low = mid;
            else
                high = mid - 1;
        }
        if (!torn && read(low, frame))
            return frame;
    }
}

} // namespace azl
