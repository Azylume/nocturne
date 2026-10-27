#pragma once

#include "audio/audio_analysis.hpp"
#include "audio/audio_input.hpp"
#include "document/wall_clock.hpp"
#include "engine/bundle_update.hpp"
#include "engine/engine.hpp"
#include "engine/machine_actions.hpp"
#include "output/displays.hpp"
#include "output/known_displays.hpp"
#include "platform/thermal.hpp"
#include "protocol/document_server.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

namespace azl {

// Where the machine's health comes from besides the engine and the displays.
struct MachineSources {
    // The last second's stats as the stats printer collected them: "render" {"fps", "gpuMs"} and "outputs"
    // [{"display", "presents", "hitches"}]; null before the first.
    std::function<Message()> stats;
    ThermalReader thermal;
    // djlink::Service::summary with "following" while the DJ link is served; unset, "djlink" is null.
    std::function<Message()> djlink;
    // The venue's media readiness (MediaSync::status); unset, "media" is null.
    std::function<Message()> media;
    const AudioSource* input = nullptr;
    // Every output shows its Screen name and display large for `seconds`; on the thread that owns the displays.
    std::function<void(double seconds)> identify;
    // Ends mirroring between displays that carry different screens now: the mirrored sets ended. On the thread that
    // owns the displays.
    std::function<int()> unmirror;
    // Where the Show lock is kept; empty keeps it in memory only.
    std::filesystem::path showLock;
    // Unset while the engine runs without a supervisor.
    MachineActions::Restart restart;
    // Unset while the engine runs without a supervisor.
    MachineActions::Stop stop;
    UpdateSource update;
    // The venue clock (Unix seconds; unset: the system's) and its zone, for the Gig windows that refuse Update.
    std::function<double()> wall;
    UtcOffset zone = systemUtcOffset;
};

// Serves the venue machine's health as "machine" messages, for the UI's Machines tab and nocturne-ctl machine:
//
//   {"op": "status"} -> {"op": "status", "mode": "normal", "showLock", "testCard", "version", "render": {"fps",
//     "gpuMs", "guard"}, "thermal": {"cpuSpeedLimit", "temperatureC", "state"}, "nextCue": {"cue", "name", "beat", "bar", "seconds"} | null, "outputs": [{"screen",
//     "screenName", "display", "displayName", "hz", "presents", "hitches", "state"}], "audio": {"device", "state",
//     "channels", "gainDb", "level", "live"} | null, "clock": {"bpm", "beat", "bar", "beatInBar", "djlink":
//     {"enabled", "following", "onAir", "bpm", "players": [{"number", "onAir", "bpm", "pitch", "play", "beatInBar"}],
//     ...} | null}}, "update": {"refused": reason | null, "last": the installer's last result
//     {"result", "build", "previous", "reason", "at"} | null}, "media": MediaSync's status | null}
//   {"op": "subscribe", "rate": 2} -> status at that rate (kMinRate..kMaxRate Hz) until "unsubscribe" or disconnect
//   {"op": "unsubscribe"}
//   {"op": "testcard", "on": bool} -> {"op": "testcard", "on"}, after the test card is turned on or off on every screen
//   {"op": "identify"[, "seconds": 3]} -> {"op": "identify", "seconds"}, after every output shows its Screen name and
//     display large for that long
//   {"op": "unmirror"} -> {"op": "unmirror", "ended"}, after mirroring between displays that carry different screens
//     is ended now, even while the display keeper's setting is off
//   restart, safeMode and lock: MachineActions, guarded by the Show lock
//   update: BundleUpdate, refused under the Show lock and inside a Gig window of the composition
//
// One output per Screen bound to a display, in screen order. Its state is "missing" while the display is not attached
// and "mirrored" while it shows another display's picture (the rule of the UI's status bar), "low rate" while it
// presented fewer than kLowRate of its display's refreshes in the last second, else "ok". Render and present figures
// are the last second's stats and null without them; a field the machine does not report is null. The thermal reading
// is taken at most every kThermalEvery and only while a client subscribes or asks, so with no Machines tab open the
// service costs nothing. Every message has "type": "machine"; replies go to the sender, errors as {"type": "error"}.
class MachineService {
public:
    using Now = std::function<double()>;
    // Runs `work` on the thread that owns the displays.
    using Post = std::function<void(std::function<void()> work)>;

    static constexpr double kMinRate = 0.2;
    static constexpr double kMaxRate = 5.0;
    static constexpr double kLowRate = 0.75;
    static constexpr auto kThermalEvery = std::chrono::seconds(10);

    MachineService(DocumentServer& document, Engine& engine, Displays& displays, const KnownDisplays* known, Now now,
                   Post post, MachineSources sources);
    ~MachineService();

    // The analysis the audio section reports; null leaves "audio" null. It must outlive the service or be reset first.
    void setAudio(const AudioAnalysis* audio) { audio_ = audio; }
    size_t subscribers();
    const MachineActions& actions() const { return actions_; }

private:
    using Clock = std::chrono::steady_clock;

    void handle(const Message& message, const DocumentServer::Sink& reply);
    void act(const std::string& op, const Message& message, const DocumentServer::Sink& reply);
    void send(std::vector<ClientId> clients);
    void run();
    Thermal thermal();
    // On the thread that owns the displays.
    Message status(const Thermal& thermal) const;
    Message outputs(const Composition& composition, const Message& stats) const;
    Message nextCue(const Composition& composition, double beat, double tempo) const;
    Message audio() const;
    std::optional<std::string> updateRefusal() const;

    DocumentServer& document_;
    Engine& engine_;
    Displays& displays_;
    const KnownDisplays* known_;
    Now now_;
    Post post_;
    MachineSources sources_;
    MachineActions actions_;
    BundleUpdate update_;
    std::atomic<const AudioAnalysis*> audio_{nullptr};

    std::mutex thermalMutex_;
    std::optional<Clock::time_point> thermalAt_;
    Thermal thermal_;

    std::mutex mutex_;
    std::condition_variable wake_;
    bool running_ = true;
    // Subscribed clients and the interval between their status messages.
    std::map<ClientId, Clock::duration> subscribers_;
    std::thread thread_;
};

} // namespace azl
