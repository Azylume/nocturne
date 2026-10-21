#pragma once

#include "djlink/djlink_packets.hpp"
#include "engine/beat_clock.hpp"

#include <map>
#include <optional>
#include <string>

namespace azl::djlink {

// The on-air player's latest beat, at engine time `time`.
struct OnAirBeat {
    int player = 0;
    double time = 0.0;
    int beatInBar = 1;
    // The BPM the beats come at: the track's BPM bent by the pitch.
    double bpm = 120.0;

    // 0 at this beat, up to the next.
    double beatsSince(double seconds) const { return (seconds - time) * bpm / 60.0; }
};

// A beat the beat clock can follow instead of its own: null when it has none (off, or lost).
class BeatSource {
public:
    virtual ~BeatSource() = default;
    virtual std::optional<OnAirBeat> onAirBeat(double now) = 0;
};

// What `beat` means for a beat clock at `engineBeat` at engine time `now`: the timeline whose beats land on the
// player's, numbered nearest to the clock's with the same place in the bar (the clock's beat 0 is a downbeat). An
// engine follows it without a jump while the two agree to within the bend BeatFollower allows.
BeatTimeline followTimeline(const OnAirBeat& beat, double engineBeat, double now);

// Everything heard on the link, by device number. Not thread-safe.
class LinkState {
public:
    // Devices announce themselves about every 1.5 s; players send beats only while playing, at least once a second
    // down to 60 BPM.
    static constexpr double kDeviceTimeout = 5.0;
    static constexpr double kBeatTimeout = 2.5;
    static constexpr double kStatusTimeout = 5.0;
    // A beat grid: this many beats in a row, each a beat after the last at the BPM it reports (within kBeatSlack of an
    // interval) and the next beat of the bar, or kLostBeats more (a lost packet). Scrubbing, cueing and jumps break it.
    static constexpr int kGridBeats = 2;
    static constexpr int kLostBeats = 1;
    static constexpr double kBeatSlack = 0.25;

    struct Device {
        Announcement announcement;
        double seen = 0.0;
    };
    struct Player {
        std::string name;
        std::optional<Beat> beat;
        double beatTime = 0.0;
        int regularBeats = 0;
        std::optional<bool> mixerOnAir;
        double mixerOnAirTime = 0.0;
        std::optional<PlayerStatus> status;
        double statusTime = 0.0;
    };

    void receive(const Packet& packet, double now);
    // The player the beat clock follows: on air (from the mixer, or from its own status) with a beat grid and, among
    // several, the one already followed, else the tempo master, else the lowest number. Without any on-air flags on the
    // link, the one player with a beat grid (or the one already followed while it keeps its grid). 0 when none.
    int onAir(double now);
    std::optional<OnAirBeat> onAirBeat(double now);

    bool live(const Player& player, double now) const;
    // Live, beating regularly at a sane BPM, and not paused by its own status.
    bool grid(const Player& player, double now) const;
    std::optional<bool> onAirFlag(const Player& player, double now) const;
    bool master(const Player& player, double now) const;
    double bpm(const Player& player) const;

    const std::map<int, Device>& devices() const { return devices_; }
    const std::map<int, Player>& players() const { return players_; }
    std::optional<MixerStatus> mixer(double now) const;
    void clear();

private:
    std::map<int, Device> devices_;
    std::map<int, Player> players_;
    std::optional<MixerStatus> mixer_;
    double mixerTime_ = 0.0;
    int followed_ = 0;
};

} // namespace azl::djlink
