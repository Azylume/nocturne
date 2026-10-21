#pragma once

#include "audio/audio_analysis.hpp"
#include "audio/spsc_ring.hpp"
#include "protocol/document_server.hpp"
#include "protocol/return_stream.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace azl {

// Streams the analysis input, the audio the engine already reads, back to the Directors that listen: Opus, mono,
// 48 kHz, kBitrate, three 20 ms packets per message, so it takes at most kMaxWireBitrate on the wire with its framing.
// Encoding runs on its own thread and only while someone listens; the analysis thread only copies samples into a
// ring and drops them when the encoder is behind. A return stream (return_stream.hpp): a slow link drops packets.
//
//   {"type": "listen", "op": "start"}  -> {"op": "start", "codec": "opus", "sampleRate": 48000, "channels": 1,
//                                          "bitrate", "frameMs": 20, "listeners"}
//   {"type": "listen", "op": "stop"}   -> {"op": "stop", "listeners"}
//   {"type": "listen", "op": "status"} -> {"op": "status", "listeners", "bytes"}
//   to each listener, every 60 ms of audio:
//     {"type": "listen", "op": "audio", "seq": n, "sizes": [bytes of each packet], "payload": <the packets in order>}
//
// "seq" counts messages per listener stream, so a gap shows packets the link dropped. A listener that disconnects
// stops listening.
class ReturnAudio {
public:
    static constexpr int kBitrate = 48000;
    static constexpr size_t kPacketsPerMessage = 3;
    static constexpr int kMaxWireBitrate = 64000;

    ReturnAudio(DocumentServer& document, AudioAnalysis& analysis);
    ~ReturnAudio();
    ReturnAudio(const ReturnAudio&) = delete;
    ReturnAudio& operator=(const ReturnAudio&) = delete;

    size_t listeners() const { return listeners_.size(); }
    // Bytes given to the transport so far.
    uint64_t bytes() const { return listeners_.bytes(); }
    // Samples the encoder was too far behind to take.
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    struct Chunk {
        uint32_t count;
        double sampleRate;
    };

    void handle(const Message& message, const DocumentServer::Sink& reply);
    // Taps the analysis while anyone listens.
    void retap();
    void heard(const float* samples, size_t count, double sampleRate);
    void run();
    void send(const std::vector<std::vector<uint8_t>>& packets);

    DocumentServer& document_;
    AudioAnalysis& analysis_;
    ReturnListeners listeners_;
    // On the heap: a harness may live on a small thread stack.
    std::unique_ptr<SpscRing<float, 1 << 16>> samples_ = std::make_unique<SpscRing<float, 1 << 16>>();
    std::unique_ptr<SpscRing<Chunk, 1024>> chunks_ = std::make_unique<SpscRing<Chunk, 1024>>();
    std::atomic<uint64_t> dropped_{0};
    uint64_t seq_ = 0;

    std::mutex mutex_;
    std::condition_variable wake_;
    bool running_ = true;
    std::thread thread_;
};

} // namespace azl
