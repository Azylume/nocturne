#pragma once

#include "output/return_video.hpp"
#include "protocol/messages.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <vector>

// What every WebSocket implementation of the protocol (websocket.mm, websocket_sockets.cpp) decides the same way: the
// limits, the pairing check, and the errors a client is told.
namespace azl::ws {

// A client this far behind stops receiving changes; it notices the gap and re-requests a snapshot.
inline constexpr int kMaxPendingSends = 256;
// Preview pictures are dropped sooner: the next one replaces them anyway.
inline constexpr int kMaxPendingPreviews = 4;
// Room for the largest message a client sends, a 1 MiB bundle chunk, even as JSON.
inline constexpr size_t kMaxMessageBytes = size_t(16) << 20;
// Before pairing a client gets no further than hello, which is small.
inline constexpr size_t kMaxUnpairedMessageBytes = size_t(64) << 10;

Message undecodable();
Message unpaired();
Message failed(const std::exception& e);

// Empty when the message has no "type" or it is not a string.
std::string typeOf(const Message& message);
bool presentsToken(const std::optional<Message>& message, const std::string& token);

// Whether a message the document sends goes out to a client that has `pending` messages still unsent, or is dropped
// because the client is too far behind for it.
bool worthSending(const Message& message, int pending, return_video::Gate& video);

std::string webSocketUrl(const std::string& host, uint16_t port);
std::string joined(const std::vector<std::string>& addresses);

} // namespace azl::ws
