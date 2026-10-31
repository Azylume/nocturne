#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace azl {

// Opus, mono, 48 kHz, 20 ms packets: the system's own codec (AudioToolbox) on macOS, libopus elsewhere. Both write
// plain Opus packets, so either side decodes the other's.
inline constexpr double kOpusRate = 48000.0;
inline constexpr size_t kOpusFrameSamples = 960;

class OpusStreamEncoder {
public:
    // Takes mono samples at `inputRate` and resamples them. Throws when the codec cannot be opened.
    OpusStreamEncoder(double inputRate, int bitrate);
    ~OpusStreamEncoder();
    OpusStreamEncoder(const OpusStreamEncoder&) = delete;
    OpusStreamEncoder& operator=(const OpusStreamEncoder&) = delete;

    double inputRate() const;
    // Adds samples; returns the packets they complete.
    std::vector<std::vector<uint8_t>> encode(const float* samples, size_t count);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class OpusStreamDecoder {
public:
    // Throws when the codec cannot be opened.
    OpusStreamDecoder();
    ~OpusStreamDecoder();
    OpusStreamDecoder(const OpusStreamDecoder&) = delete;
    OpusStreamDecoder& operator=(const OpusStreamDecoder&) = delete;

    // Appends the packet's samples, mono at kOpusRate, to `out`; false when it does not decode.
    bool decode(const uint8_t* packet, size_t size, std::vector<float>& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace azl
