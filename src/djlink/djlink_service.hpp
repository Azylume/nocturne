#pragma once

#include "djlink/djlink_receiver.hpp"
#include "djlink/djlink_state.hpp"
#include "protocol/document_server.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace azl::djlink {

// The venue machine's DJ link setting: listening or not, and on which network interface. It belongs to the machine,
// so it lives in a file on it (`djlink.json` in the state folder), not in the composition.
struct Settings {
    bool enabled = false;
    std::string interface;
    bool operator==(const Settings&) const = default;
};

// Defaults when the file is missing or unreadable.
Settings loadSettings(const std::filesystem::path& file);
// Throws std::runtime_error.
void saveSettings(const std::filesystem::path& file, const Settings& settings);

// Serves the DJ link over the protocol as "djlink" messages, and is the beat source for the beat clock. Receive only
// (see Receiver).
//
//   {"op": "status"}  -> to the sender: {"op": "status", "enabled", "interface", "listening", "error", "datagrams",
//                        "unknown", "time" (engine time the status is for, as the clock state's "time"), "onAir" (the
//                        followed player, 0 when none), "bpm" (its BPM, null when none), "beat":
//                        {"inBar" 1..4, "phase" 0..1} or null, "devices": [{"number", "name", "kind" ("player",
//                        "mixer", "computer"), "ip", "mac", "seen"}], "players": [{"number", "name", "live", "grid",
//                        "onAir" (null when unknown), "master", "bpm", "trackBpm", "pitch" (percent), "beatInBar",
//                        "playing" (null when unknown), "playState", "play" ("playing", "paused", "cued", ...; empty
//                        when unknown), "track": {"player", "slot", "type", "id"} or
//                        null}], "mixer": {"number", "name", "bpm", "beatInBar", "master"} or null}
//   {"op": "set", "enabled": true|false, "interface": "en7"}  -> saved, applied, and every client gets the status; the
//                        interface may be left out to keep the saved one
//
// "seen" is seconds since the device last announced itself; "live" a player sending beats; "grid" one beating
// regularly, which alone can be followed. Track IDs, play state and the master flag come from status packets, which
// players send only to devices that announced themselves, so they are present only where the network delivers them
// anyway. Every message has "type": "djlink"; errors are
// {"type": "error"} to the sender.
class Service : public BeatSource {
public:
    using Now = std::function<double()>;

    // `receiver` gives the address and ports to bind (tests); the interface comes from the settings.
    Service(DocumentServer& document, Now now, std::filesystem::path settingsFile, Receiver::Options receiver = {});
    ~Service() override;

    std::optional<OnAirBeat> onAirBeat(double now) override;
    Settings settings() const;
    Message status();
    // The status in brief, for the machine status and the stats each second: {"enabled", "interface", "listening",
    // "error", "datagrams", "onAir", "bpm", "beatInBar", "players": [{"number", "onAir", "bpm", "pitch", "play",
    // "beatInBar"}]}.
    Message summary();

private:
    void handle(const Message& message, const DocumentServer::Sink& reply);
    void apply(const Settings& settings);
    void datagram(uint16_t port, const uint8_t* data, size_t size);

    DocumentServer& document_;
    const Now now_;
    const std::filesystem::path file_;
    const Receiver::Options options_;

    mutable std::mutex settingsMutex_;
    Settings settings_;
    std::unique_ptr<Receiver> receiver_;

    std::mutex stateMutex_;
    LinkState state_;
    uint64_t unknown_ = 0;
};

// The summary (with "following" when given) as one line of the stats printout: "djlink en7, listening, 812 datagrams,
// on air 2 at 128.00 BPM, following | player 2 128.00 BPM +0.00% playing beat 3 on air | ...", or "djlink off".
std::string statusLine(const Message& summary);

} // namespace azl::djlink
