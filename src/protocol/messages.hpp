#pragma once

#include "document/composition.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace azl {

using Message = nlohmann::json;

// Every protocol message is one object with a "type".
//   client -> engine: hello, snapshot (request), command
//   engine -> client: snapshot, change, error (also unsolicited, e.g. a chain that failed to compile)
// hello and snapshot carry the sender's "build" (buildId) and "effects" (effectRegistryVersion). A client on another
// machine than the engine's also puts the engine machine's pairing token in its hello as "token" (net/pairing.hpp).
namespace msg {

// With "token" when `token` is not empty.
Message hello(const std::string& token = {});
Message snapshotRequest();
Message command(const Edit& edit);
Message undo();
Message redo();
// Engine -> every client: an effect chain failed to compile. Layers with it keep showing what they showed.
Message chainFailed(const std::vector<std::string>& chain, const std::string& error);
// Engine -> every client: the clip on a layer cannot be played (missing, corrupt, unsupported or broken mid-stream).
// The layer keeps showing what it showed.
Message clipFailed(uint64_t layer, const std::string& hash, const std::string& reason);

} // namespace msg

// MessagePack travels in binary WebSocket messages, JSON (for debugging) in text messages.
enum class Encoding { MessagePack, Json };

std::vector<uint8_t> encode(const Message& message, Encoding encoding);
// Null when the bytes are not a valid message.
std::optional<Message> decode(const uint8_t* data, size_t size, Encoding encoding);

} // namespace azl
