#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace azl::djlink {

// Pro DJ Link, the CDJs' and mixers' own UDP protocol, as far as it can be heard without taking part in it. Layouts
// follow Deep Symmetry's protocol analysis (https://djl-analysis.deepsymmetry.org/), read as documentation only.
//
//   50000  device announcements (keep-alives), broadcast every ~1.5 s by every device
//   50001  beats and the mixer's channels-on-air flags, broadcast
//   50002  player and mixer status, sent only to devices that announced themselves, so a listener that never announces
//          receives it only where the network delivers it anyway (a mirrored port)
constexpr uint16_t kAnnouncePort = 50000;
constexpr uint16_t kBeatPort = 50001;
constexpr uint16_t kStatusPort = 50002;

// A pitch of 0x100000 is 0 %, 0 is -100 % and 0x200000 is +100 %.
constexpr uint32_t kPitchNormal = 0x100000;

enum class DeviceKind { Player, Mixer, Computer };

struct Announcement {
    int number = 0;
    std::string name;
    std::array<uint8_t, 6> mac{};
    std::array<uint8_t, 4> ip{};
    int peers = 0;
};

struct Beat {
    int number = 0;
    std::string name;
    // The track's BPM, before the pitch.
    double trackBpm = 0.0;
    uint32_t pitch = kPitchNormal;
    // 1..4
    int beatInBar = 1;
    // Milliseconds from this beat to the next beat and the next downbeat, at 0 % pitch.
    uint32_t nextBeatMs = 0;
    uint32_t nextBarMs = 0;
};

// The mixer's on-air flag per channel; channel N is player N.
struct OnAir {
    int number = 0;
    std::vector<bool> channels;
};

struct PlayerStatus {
    int number = 0;
    std::string name;
    // Where the loaded track comes from: the player holding the media, its slot (1 CD, 2 SD, 3 USB, 4 rekordbox),
    // the track type (1 rekordbox, 2 unanalysed, 5 CD audio) and its rekordbox ID; all 0 with no track.
    int trackPlayer = 0;
    int trackSlot = 0;
    int trackType = 0;
    uint32_t trackId = 0;
    // P1 in the analysis: 0 no track, 2 loading, 3 playing, 4 looping, 5 paused, 6 paused at the cue, 9 searching,
    // 0x11 ended.
    uint8_t playState = 0;
    bool playing = false;
    bool master = false;
    bool synced = false;
    bool onAir = false;
    // Null when no track is loaded.
    std::optional<double> trackBpm;
    uint32_t pitch = kPitchNormal;
    // Null when the player does not know it.
    std::optional<uint32_t> beat;
    int beatInBar = 0;
};

struct MixerStatus {
    int number = 0;
    std::string name;
    bool master = false;
    double bpm = 0.0;
    int beatInBar = 0;
};

using Packet = std::variant<Announcement, Beat, OnAir, PlayerStatus, MixerStatus>;

// Null for anything else: other packet types, a short packet, or bytes that are not Pro DJ Link.
std::optional<Packet> parse(uint16_t port, const uint8_t* data, size_t size);

DeviceKind kindOf(int number);
// The pitch as a factor: 1.0 is 0 %.
double pitchFactor(uint32_t pitch);
// The pitch in percent.
double pitchPercent(uint32_t pitch);

} // namespace azl::djlink
