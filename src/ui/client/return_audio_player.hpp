#pragma once

#include "audio/opus_codec.hpp"
#include "protocol/messages.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace azl::ui {

// Plays a venue's return audio (return_audio.hpp) on this machine's default output: decodes the Opus packets of its
// "listen" "audio" messages into a jitter buffer that starts playing once it holds kStartSeconds and is cut back to
// that when the link delivers a burst past kMaxSeconds, so the delay stays near half a second. Without a device it
// only decodes. Thread-safe.
class ReturnAudioPlayer {
public:
    static constexpr double kStartSeconds = 0.15;
    static constexpr double kMaxSeconds = 0.4;

    ReturnAudioPlayer() = default;
    ~ReturnAudioPlayer();
    ReturnAudioPlayer(const ReturnAudioPlayer&) = delete;
    ReturnAudioPlayer& operator=(const ReturnAudioPlayer&) = delete;

    // Off, nothing reaches the output device; tests turn it off.
    void useDevice(bool on) { device_ = on; }
    void receive(const Message& message);
    // Drops what is buffered and closes the output.
    void stop();
    // 0..1.
    void setVolume(double volume);
    double volume() const;

    uint64_t decodedSamples() const { return decoded_.load(std::memory_order_relaxed); }
    // Packets missing between the messages received.
    uint64_t lostPackets() const { return lost_.load(std::memory_order_relaxed); }

    // The device the player plays through: an AudioQueue on Apple, JUCE's default output elsewhere. It calls `pull`
    // on its own thread for the next mono samples at kOpusRate; destroying it stops those calls.
    class Output {
    public:
        virtual ~Output() = default;
        virtual void start() = 0;
    };
    // Null, with the reason on stderr, when the device does not open.
    static std::unique_ptr<Output> openOutput(std::function<void(float* out, size_t count)> pull);

private:
    // Fills `out` from the buffer, silence where it runs dry. The output's thread.
    void pull(float* out, size_t count);
    void open();

    std::atomic<bool> device_{true};
    mutable std::mutex mutex_;
    std::unique_ptr<OpusStreamDecoder> decoder_;
    std::deque<float> buffered_;
    bool playing_ = false;
    std::optional<uint64_t> next_;
    std::unique_ptr<Output> output_;
    std::atomic<double> volume_{1.0};
    std::atomic<uint64_t> decoded_{0};
    std::atomic<uint64_t> lost_{0};
};

} // namespace azl::ui
