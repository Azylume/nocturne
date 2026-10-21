#pragma once

#include "audio/audio_input.hpp"
#include "document/wall_clock.hpp"
#include "output/recorder.hpp"
#include "protocol/document_server.hpp"

#include <filesystem>
#include <mutex>

namespace azl {

// Serves recording over the protocol as "record" messages: live, machine-local actions outside the document and its
// undo history.
//
//   {"type": "record", "op": "set", "target"?, "codec"?, "path"?, "width"?, "height"?, "reserveMB"?, "reservePercent"?,
//    "maxMB"?, "maxSeconds"?, "maxTotalSeconds"?, "gigGraceSeconds"?, "audio"?, "precise"?}
//       the settings the next recording starts with: target "composition" or "screen:N", codec "hevc" or "prores",
//       path a .mov file or a folder on the engine machine (default ~/Movies/Nocturne), size 0 for the composition's,
//       the reserve: the larger of reserveMB and reservePercent of the disk (default the engine's, 20480 MB and 10 %)
//       below which a recording stops and none starts, the size (MB) or length (s) past which the recording goes on in
//       the next numbered file (0: none), the length in all its files past which the recording finishes (0: none,
//       default 8 h), how long after the end of the Gig window it started in it finishes (default 15 min), whether the
//       movie gets an audio track of
//       the audio input (true or "on", the default, false or "off"; only when an input is running), and whether the encoders get their own
//       precision (HEVC Main10, ProRes from 16 bits) instead of 8-bit input (default false: cheaper on the GPU)
//   {"type": "record", "op": "start", same optional fields}   starts recording with the settings, these applied first
//   {"type": "record", "op": "stop"}                           returns once the movie is written
//   {"type": "record", "op": "state"}
//   {"type": "record", "op": "folders", "path"?}
//       the subfolders of `path` on the engine machine (default: the recording folder, else the home folder) and the
//       mounted volumes, for a folder picker on a UI that may run elsewhere; read-only, names and free space only:
//       {"type": "record", "op": "folders", "path", "parent" ("" at the root), "freeMB", "totalMB", "writable",
//        "folders": [{"name", "path"}], "volumes": [{"name", "path", "freeMB", "totalMB"}]}
// Each is answered to its sender with its own op and the state; set, start and stop also broadcast it as "state":
//   {"type": "record", "op": ..., "recording", "target", "codec", "path", "width", "height", "reserveMB", "reservePercent",
//    "maxMB", "maxSeconds", "maxTotalSeconds", "gigGraceSeconds", "audio": "on" | "off" | "unavailable", "audioReason", "audioSource", "precise", "file",
//    "frames", "dropped", "hardware": true | false | null, "audioTrack", "audioDropped",
//    "space": "unknown" | "ok" | "low" | "full", "freeMB", "stopsAtMB", "folder", "folderProblem",
//    "stopsAt": Unix seconds | null, "stopsAtClock": "04:15" | "", "stopsFor": "max length" | "gig over" | ""}
// "stopsAt" is when the running recording finishes by itself, on the engine machine's clock.
// "folder" is where the next recording goes, and "folderProblem" why it cannot ("" when it can).
// "audio" is the running recording's audio track, or the one a recording started now would get: "off" by the setting,
// "unavailable" with the reason when no track can be made (the recording runs without one), "on" with the input
// ("Scarlett Solo USB ch 2"). The broadcast after a start without the track carries a "notice".
// "file", "frames", "dropped", "hardware", "audioTrack" (the movie has an audio track), "audioDropped" (samples) and the
// space as of the last check ("low": less than twice the reserve free, "full": stopped below it) belong to the
// recording running or the last one; "state" is broadcast again whenever the space changes. When a recording ends,
// for whatever reason, every client gets once:
//   {"type": "record", "op": "stopped", "file", "frames", "dropped",
//    "reason": "stopped" | "disk full" | "max length" | "gig over" | "failed: ...",
//    "kept", "notice"?}
// with a "notice" for the UI to show when it ended by itself.
// The recorder's name for an audio input: the device and its channels, or why none runs.
Recorder::AudioSource audioSourceOf(const AudioInputStatus& input);

// With a `file`, every set and start keeps the settings there except the target and a .mov path; `load` reads them
// back on the next start.
class RecordService {
public:
    RecordService(DocumentServer& document, Engine& engine, double fps, Recorder::Settings settings = {},
                  std::filesystem::path file = {});
    ~RecordService();

    Recorder& recorder() { return recorder_; }
    // The settings the next recording starts with.
    Recorder::Settings settings();
    // For the harness: where the folder list finds the mounted volumes (default /Volumes).
    void setVolumes(std::filesystem::path volumes);
    // For the harness: the wall clock (Unix seconds) and its zone, for the Gig windows (default the system's).
    void setWallClock(std::function<double()> now, UtcOffset zone);

    // `defaults` with what `file` keeps; a file that cannot be read is logged and left out.
    static Recorder::Settings load(const std::filesystem::path& file, Recorder::Settings defaults);

private:
    void handle(const Message& message, const DocumentServer::Sink& reply);
    void apply(const Message& message);
    void save();
    Message state(const std::string& op);
    Message folders(const Message& message);
    void ended(const Recorder::Ended& ended);

    DocumentServer& document_;
    Engine& engine_;
    const std::filesystem::path file_;
    std::mutex mutex_;
    Recorder::Settings settings_;
    std::filesystem::path volumes_ = "/Volumes";
    std::function<double()> now_;
    UtcOffset zone_ = systemUtcOffset;
    Recorder recorder_;
};

// The "folders" reply's fields for `path`; throws std::runtime_error when it is no folder.
Message listFolders(const std::filesystem::path& path, const std::filesystem::path& volumes);

} // namespace azl
