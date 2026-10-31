#pragma once

#include <cstdint>
#include <cstring>
#include <optional>

// The clock ping and its answer on the wire (ADR 0005), for both UDP clock implementations.
namespace azl::udp_clock {

inline constexpr uint32_t kMagic = 0x434c5a41; // "AZLC"
inline constexpr size_t kMaxPeers = 64;
// A pong this many pings behind is stale.
inline constexpr uint32_t kMaxSeqLag = 20;

struct Packet {
    uint32_t magic = kMagic;
    uint32_t seq = 0;
    double sent = 0.0;
    double remoteReceived = 0.0;
    double remoteSent = 0.0;
};
static_assert(sizeof(Packet) == 32);

// None when the datagram is not a clock packet.
inline std::optional<Packet> readPacket(const void* bytes, size_t size)
{
    if (size != sizeof(Packet))
        return std::nullopt;
    Packet packet;
    std::memcpy(&packet, bytes, sizeof packet);
    if (packet.magic != kMagic)
        return std::nullopt;
    return packet;
}

} // namespace azl::udp_clock
