#pragma once

#include "protocol/document_server.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>

namespace azl {

// A return stream is media the venue sends back only while a Director asks for it: its audio input (return audio) or
// its outputs (return video). Each packet message carries its encoded bytes as binary "payload". A transport drops a
// packet for a client with more than kMaxPendingReturn messages still unsent instead of queueing it, so a slow link
// loses media and never holds the engine back. A return video frame dropped so takes its output's later frames with it
// until the next key frame (return_video::Gate), since the decoder needs one to go on.
inline constexpr const char* kReturnPayload = "payload";
inline constexpr int kMaxPendingReturn = 8;

bool isReturnPacket(const Message& message);

// The clients of one return stream and the bytes sent to them. Thread-safe.
class ReturnListeners {
public:
    // True when it is the first listener.
    bool add(ClientId client);
    // True when no listener is left.
    bool remove(ClientId client);
    bool empty() const;
    size_t size() const;
    // Sends to every listener and forgets those gone; true while any is left.
    bool send(DocumentServer& document, const Message& message);
    // The packet bytes, as MessagePack, given to the transport so far; it may still drop some for a slow client.
    uint64_t bytes() const { return bytes_.load(std::memory_order_relaxed); }

private:
    mutable std::mutex mutex_;
    std::set<ClientId> clients_;
    std::atomic<uint64_t> bytes_{0};
};

} // namespace azl
