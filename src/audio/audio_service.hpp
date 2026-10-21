#pragma once

#include "audio/audio_analysis.hpp"
#include "audio/audio_device_settings.hpp"
#include "audio/audio_input.hpp"
#include "protocol/document_server.hpp"
#include "supervisor/rotating_log.hpp"

#include <condition_variable>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace azl {

// Serves the audio input and its analysis over the protocol as "audio" messages. They are machine-local: the input
// device and the analysis belong to the machine, not to the composition.
//
//   {"op": "devices"}  -> {"op": "devices", "devices": [{"uid", "name", "channels", "sampleRate", "default"}]}; every
//                         client gets it again when a device is added or removed
//   {"op": "apps"}     -> {"op": "apps", "apps": [{"pid", "bundle", "name", "playing"}]}, what "app:" taps can capture
//   {"op": "input"}    -> {"op": "input", "requested", "channels", "uid", "name", "sampleRate", "state", "permission",
//                          "restarts"}
//   {"op": "select", "device": "<uid, name or part of a name>" | "" (default input) | "system" | "app:<part of a
//                          bundle ID or name, or a pid>" | "off", "channels": [1, 2]} (taps need macOS 14.2)
//                      -> every client gets "input" messages as the input changes, with an "event"
//   {"op": "state"}    -> {"op": "state", "time", "live", "silent", "overruns", "gainDb", "values": {"level", "sub",
//                          "bass", "lowMid", "mid", "highMid", "high", "kick", "snare", "hat", "lowBand", "midBand",
//                          "highBand"}, "onsets": {"kick", "snare", "hat"}, "spectrum": [24 x 0..1], "channels":
//                          [{"levelDb", "noise"}] for each of the device's input channels}, the analysis as the render
//                          thread sees it now; the composition's bandEdges split lowBand, midBand and highBand
//   {"op": "subscribe", "rate": 30}  -> "state" messages at that rate (1..60 Hz) until "unsubscribe" or disconnect
//   {"op": "settings"} with any of "autoGain", "gainDb", "latencyMs" (0..1000) and "envelopes": {"<feature>":
//                          {"attackMs", "releaseMs"}} -> every client gets the full {"op": "settings", ...}; with
//                          none of them only the sender gets it
//
// Gain and channels are remembered per input device in `deviceFile` and come back whenever that device runs again:
// selected, plugged back in, or after a restart. A select is kept there too, with the command line's input then, for
// the next start (startingInput).
//
// Every message has "type": "audio". Requests are answered to their sender; errors as {"type": "error"}. With a log
// file, the service appends the analysis as JSON lines at 10 Hz (state without the spectrum, plus the onsets since
// the previous line) and every input event and settings change, for a post-gig review. The log survives restarts and
// rotates by size.
class AudioService {
public:
    using Now = std::function<double()>;

    // `input` may be null (the harness): then only the analysis is served.
    AudioService(DocumentServer& document, AudioAnalysis& analysis, AudioSource* input, Now now,
                 const std::string& logFile = {}, const std::filesystem::path& deviceFile = {});
    ~AudioService();

    static constexpr double kMaxRate = 60.0;

    // The command line's input on this start, kept with each select.
    void setCommandLine(AudioChoice choice);

private:
    void handle(const Message& message, const DocumentServer::Sink& reply);
    void settings(const Message& message, const DocumentServer::Sink& reply);
    void applied(const AudioSettings& settings);
    void inputChanged(const AudioInputStatus& status, const std::string& event);
    Message state(double now) const;
    void run();
    void log(const Message& line);
    AudioChoice commandLine();

    DocumentServer& document_;
    AudioAnalysis& analysis_;
    AudioSource* input_;
    AudioChoice commandLine_;
    Now now_;
    std::unique_ptr<RotatingLog> log_;
    AudioDeviceSettings devices_;

    std::mutex mutex_;
    std::condition_variable wake_;
    bool running_ = true;
    // Subscribed clients and the interval between their state messages.
    std::map<ClientId, double> subscribers_;
    std::mutex logMutex_;
    std::thread thread_;
};

Message toJson(const AudioInputStatus& status);
Message toJson(const AudioSettings& settings);

} // namespace azl
