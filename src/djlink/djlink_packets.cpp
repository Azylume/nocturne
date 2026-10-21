#include "djlink/djlink_packets.hpp"

#include <cstring>

namespace azl::djlink {

namespace {

constexpr uint8_t kMagic[] = {0x51, 0x73, 0x70, 0x74, 0x31, 0x57, 0x6d, 0x4a, 0x4f, 0x4c};
constexpr size_t kTypeAt = 0x0a;

constexpr uint8_t kKeepAlive = 0x06;
constexpr uint8_t kChannelsOnAir = 0x03;
constexpr uint8_t kBeat = 0x28;
constexpr uint8_t kPlayerStatus = 0x0a;
constexpr uint8_t kMixerStatus = 0x29;

constexpr uint8_t kFlagPlaying = 0x40;
constexpr uint8_t kFlagMaster = 0x20;
constexpr uint8_t kFlagSynced = 0x10;
constexpr uint8_t kFlagOnAir = 0x08;

uint16_t u16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

std::string name(const uint8_t* p)
{
    return std::string(reinterpret_cast<const char*>(p), strnlen(reinterpret_cast<const char*>(p), 20));
}

// Packets on 50001 and 50002: the name at 0x0b, the device number at 0x21, the rest from 0x24.
std::string statusName(const uint8_t* d) { return name(d + 0x0b); }

std::optional<Packet> announcement(const uint8_t* d, size_t size)
{
    if (d[kTypeAt] != kKeepAlive || size < 0x36)
        return std::nullopt;
    Announcement a;
    a.name = name(d + 0x0c);
    a.number = d[0x24];
    std::memcpy(a.mac.data(), d + 0x26, 6);
    std::memcpy(a.ip.data(), d + 0x2c, 4);
    a.peers = d[0x30];
    return a;
}

std::optional<Packet> beatPort(const uint8_t* d, size_t size)
{
    if (d[kTypeAt] == kBeat && size >= 0x60) {
        Beat b;
        b.number = d[0x21];
        b.name = statusName(d);
        b.nextBeatMs = u32(d + 0x24);
        b.nextBarMs = u32(d + 0x2c);
        b.pitch = u32(d + 0x54);
        b.trackBpm = u16(d + 0x5a) / 100.0;
        b.beatInBar = d[0x5c];
        if (b.beatInBar < 1 || b.beatInBar > 4)
            return std::nullopt;
        return b;
    }
    if (d[kTypeAt] == kChannelsOnAir && size >= 0x2d) {
        OnAir o;
        o.number = d[0x21];
        for (size_t i = 0x24; i < 0x28; ++i)
            o.channels.push_back(d[i] != 0);
        if (size >= 0x35 && u16(d + 0x22) >= 0x11) {
            o.channels.push_back(d[0x2d] != 0);
            o.channels.push_back(d[0x2e] != 0);
        }
        return o;
    }
    return std::nullopt;
}

std::optional<Packet> statusPort(const uint8_t* d, size_t size)
{
    if (d[kTypeAt] == kPlayerStatus && size >= 0xa7) {
        PlayerStatus s;
        s.number = d[0x21];
        s.name = statusName(d);
        s.trackPlayer = d[0x28];
        s.trackSlot = d[0x29];
        s.trackType = d[0x2a];
        s.trackId = u32(d + 0x2c);
        s.playState = d[0x7b];
        const uint8_t flags = d[0x89];
        s.playing = flags & kFlagPlaying;
        s.master = flags & kFlagMaster;
        s.synced = flags & kFlagSynced;
        s.onAir = flags & kFlagOnAir;
        s.pitch = u32(d + 0x8c);
        if (const uint16_t bpm = u16(d + 0x92); bpm != 0xffff)
            s.trackBpm = bpm / 100.0;
        if (const uint32_t beat = u32(d + 0xa0); beat != 0xffffffff)
            s.beat = beat;
        s.beatInBar = d[0xa6];
        return s;
    }
    if (d[kTypeAt] == kMixerStatus && size >= 0x38) {
        MixerStatus m;
        m.number = d[0x21];
        m.name = statusName(d);
        m.master = d[0x27] & kFlagMaster;
        m.bpm = u16(d + 0x2e) / 100.0;
        m.beatInBar = d[0x37];
        return m;
    }
    return std::nullopt;
}

} // namespace

std::optional<Packet> parse(uint16_t port, const uint8_t* data, size_t size)
{
    if (size < 0x24 || std::memcmp(data, kMagic, sizeof kMagic) != 0)
        return std::nullopt;
    switch (port) {
    case kAnnouncePort:
        return announcement(data, size);
    case kBeatPort:
        return beatPort(data, size);
    case kStatusPort:
        return statusPort(data, size);
    default:
        return std::nullopt;
    }
}

DeviceKind kindOf(int number)
{
    if (number >= 1 && number <= 6)
        return DeviceKind::Player;
    if (number >= 0x21 && number <= 0x28)
        return DeviceKind::Mixer;
    return DeviceKind::Computer;
}

double pitchFactor(uint32_t pitch) { return double(pitch) / kPitchNormal; }

double pitchPercent(uint32_t pitch) { return (pitchFactor(pitch) - 1.0) * 100.0; }

} // namespace azl::djlink
