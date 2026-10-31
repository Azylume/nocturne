#include "net/websocket_frames.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

// The first byte's low bit picks which side reads the frames; the rest of the input is fed in pieces as long as its
// other seven bits say. The whole input is also read as a handshake head, by the server and by the client.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0)
        return 0;
    const bool fromClient = data[0] & 1;
    const size_t piece = std::max<size_t>(1, data[0] >> 1);
    azl::ws::MessageReader reader(fromClient, 64 << 10);
    for (size_t at = 1; at < size; at += piece) {
        reader.feed(data + at, std::min(piece, size - at));
        while (reader.next()) {
        }
    }

    const std::string text(reinterpret_cast<const char*>(data), size);
    if (const auto head = azl::ws::parseHead(text)) {
        azl::ws::answerHandshake(*head);
        azl::ws::handshakeAccepted(*head, "dGhlIHNhbXBsZSBub25jZQ==");
    }
    return 0;
}
