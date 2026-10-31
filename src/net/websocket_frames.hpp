#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// The WebSocket wire format (RFC 6455) without any socket: the opening handshake on both sides, framing, and a reader
// that turns received bytes into whole messages. Pure functions of their input, so a fuzzer can drive them.
namespace azl::ws {

enum class Opcode : uint8_t { Continuation = 0x0, Text = 0x1, Binary = 0x2, Close = 0x8, Ping = 0x9, Pong = 0xa };

inline constexpr uint16_t kCloseNormal = 1000;
inline constexpr uint16_t kCloseProtocolError = 1002;
inline constexpr uint16_t kClosePolicy = 1008;
inline constexpr uint16_t kCloseTooBig = 1009;

// The longest handshake head either side reads; a longer one is refused.
inline constexpr size_t kMaxHeadBytes = 8192;

std::array<uint8_t, 20> sha1(const void* data, size_t size);
std::string base64(const uint8_t* data, size_t size);
// Sec-WebSocket-Accept for a Sec-WebSocket-Key.
std::string acceptKey(const std::string& key);

// One HTTP head up to its blank line: the first line, and the headers by lowercase name, repeated ones joined by ", ".
struct Head {
    std::string start;
    std::map<std::string, std::string> headers;
};
// Null when `text` does not end its head, or a line is not a header.
std::optional<Head> parseHead(const std::string& text);
// Where the head ends in `text`, just past its blank line; npos while it has not ended.
size_t headEnd(const std::string& text);

// What a server answers a client's opening handshake: 101 when it upgrades, else a refusal to send before closing.
// A request with an Origin header is refused, since every browser sends one and native clients never do.
struct HandshakeAnswer {
    bool upgraded = false;
    std::string response;
};
HandshakeAnswer answerHandshake(const Head& request);

// The client's opening handshake for `hostHeader` ("host:port") with a key of 16 random bytes in base64.
std::string handshakeRequest(const std::string& hostHeader, const std::string& key);
// The server's head upgrades the connection for `key`.
bool handshakeAccepted(const Head& response, const std::string& key);

// One whole frame. A client masks every frame with `mask`; a server sends them unmasked.
std::vector<uint8_t> frame(Opcode opcode, const uint8_t* payload, size_t size,
                           const std::optional<std::array<uint8_t, 4>>& mask = std::nullopt);
std::vector<uint8_t> closeFrame(uint16_t code, const std::optional<std::array<uint8_t, 4>>& mask = std::nullopt);

// What a reader found in the bytes so far.
struct Event {
    enum class Kind { Text, Binary, Ping, Pong, Close, TooBig, Invalid };
    Kind kind;
    // The message, the ping or pong data, or for Close its status code (two bytes) and reason.
    std::vector<uint8_t> payload;
};

// Reads the frames one side receives into messages. Fragments join into one message; pings, pongs and closes come out
// between them. A message whose frames announce more than the limit in all is TooBig as soon as the frame header says
// so, before its payload is read. After TooBig or Invalid it reads no further.
class MessageReader {
public:
    // `fromClient`: the frames come from a client and must be masked; from a server they must not be.
    MessageReader(bool fromClient, size_t limit) : fromClient_(fromClient), limit_(limit) {}

    void setLimit(size_t limit) { limit_ = limit; }
    void feed(const uint8_t* data, size_t size);
    // The next event, once its bytes are all in.
    std::optional<Event> next();

private:
    std::optional<Event> fail(Event::Kind kind);

    bool fromClient_;
    size_t limit_;
    std::vector<uint8_t> buffer_;
    size_t offset_ = 0;
    std::vector<uint8_t> message_;
    std::optional<Opcode> fragmented_;
    bool failed_ = false;
};

} // namespace azl::ws
