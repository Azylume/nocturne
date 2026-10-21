#pragma once

#include "engine/engine.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace azl {

// Records the composition, or one screen through its mapping, to a QuickTime movie with VideoToolbox: HEVC (the
// hardware encoder where the machine has one) or ProRes 422.
//
// The render thread only fills a pooled IOSurface-backed pixel buffer from each frame on the frame's own command buffer:
// one compute pass writes the encoder's own Y'CbCr format where the recorder knows it (else a BGRA copy the encoder
// converts itself); a screen or another size is painted first. Once the GPU finished it, the buffer joins a bounded queue that the
// recorder's own thread encodes and writes. Nothing on the render thread ever waits: with no free buffer or a full
// queue, the recorded frame is dropped and counted.
//
// The movie is written to a hidden partial file next to the target and renamed into place once complete, so the
// target path only ever holds a whole movie. The partial file is fragmented: when the engine dies mid-recording, it
// still plays up to its last fragment, and `recover` names it like a recording.
//
// With an audio analysis set, the movie also gets an AAC track of its input at 48 kHz: the analysis hands each chunk it
// drains to the recorder (never on the audio thread), the writer thread resamples it, and each lands at its capture
// time on the frames' clock. When no track can be made, the recording starts anyway and its status says why.
//
// A thread of its own checks the free space on the recording's volume every few seconds, and the writer after every
// quarter second of movie: below twice the reserve the status warns, below the reserve the recording stops and finishes
// cleanly, and no recording starts there. A write that fails anyway (the disk filled up) ends it too, and the partial
// file is removed. With a maximum size or length, the recording goes on in the next numbered file ("name 001.mov",
// "name 002.mov", ...) without losing a frame: one VideoToolbox encoder runs for the whole recording, each file takes
// its output as it is, the next file is opened while the current one is written, and the full one finishes on another
// queue.
//
// A recording finishes by itself, cleanly, past its maximum total length across all its files, or when it started
// inside a Gig window, at the window's end plus a grace, whichever comes first.
class Recorder {
public:
    struct Settings {
        // "composition" or "screen:N".
        std::string target = "composition";
        // "hevc" or "prores".
        std::string codec = "hevc";
        // A .mov file, or a folder that gets a file named after the date and time. Empty: ~/Movies/Nocturne.
        std::filesystem::path path;
        // 0: the composition's size.
        uint32_t width = 0;
        uint32_t height = 0;
        // The reserve is the larger of the two: below it a recording stops, and none starts.
        uint64_t reserveBytes = uint64_t(20) << 30;
        double reservePercent = 10.0;
        // Past either (0: no limit), the recording goes on in the next file.
        uint64_t maxBytes = 0;
        double maxSeconds = 0.0;
        // Past this length in all its files (0: no limit), the recording finishes.
        double maxTotalSeconds = 8 * 3600.0;
        // A recording started inside a Gig window finishes this long after the window ends.
        double gigGraceSeconds = 15 * 60.0;
        // An audio track of the analysis's input, when there is one.
        bool audio = true;
        // Off: 8-bit input, HEVC Main from '420v' and ProRes from '2vuy', cheaper on the GPU. On: the encoders' own
        // precision, HEVC Main10 from 'x420' and ProRes from 'v216'.
        bool precise = false;
    };

    struct Space {
        uint64_t free = UINT64_MAX;
        uint64_t total = 0;
    };
    using SpaceProvider = std::function<Space(const std::filesystem::path& folder)>;
    // Low: less than twice the reserve free. Full: less than the reserve; the recording stopped.
    enum class SpaceLevel { Unknown, Ok, Low, Full };

    struct Status {
        bool recording = false;
        Settings settings;
        // The movie being written, or the last one.
        std::filesystem::path file;
        uint64_t frames = 0;
        uint64_t dropped = 0;
        // Whether the encoder runs in hardware, once known.
        std::optional<bool> hardware;
        // The pixel format handed to the encoder: its own ("420v", "2vuy", or with `precise` "x420", "v216"), or "BGRA", which it converts itself.
        std::string pixels;
        // "on", "off" (the setting) or "unavailable", with the reason.
        std::string audio = "off";
        std::string audioReason;
        // The input it records: the device and its channels.
        std::string audioSource;
        // Audio samples that did not fit the backlog or the encoder.
        uint64_t audioDropped = 0;
        // As of the last check.
        SpaceLevel space = SpaceLevel::Unknown;
        uint64_t freeBytes = 0;
        uint64_t reserve = 0;
        // When the recording finishes by itself (Unix seconds), and why: "max length" or "gig over".
        std::optional<double> stopsAt;
        std::string stopsFor;
    };

    struct Ended {
        std::filesystem::path file;
        uint64_t frames = 0;
        uint64_t dropped = 0;
        // "stopped", "disk full", "max length", "gig over" or "failed: ...".
        std::string reason;
        // The movie is complete at `file`; otherwise nothing was left there.
        bool kept = false;
    };
    // The input a recording would take its audio from now: a name, or empty with the reason there is none.
    struct AudioSource {
        std::string name;
        std::string reason;
    };
    using AudioSourceOf = std::function<AudioSource()>;
    struct AudioPlan {
        std::string state = "off";
        std::string reason;
        std::string source;
    };

    // Called once per recording, on the thread that ended it.
    using OnEnded = std::function<void(const Ended&)>;
    // Called on the checking thread when the space level of a running recording changes.
    using OnSpace = std::function<void()>;

    Recorder(Engine& engine, double fps, OnEnded onEnded, OnSpace onSpace = {});
    ~Recorder();

    // Any thread. Throws std::runtime_error when it cannot start (already recording, unknown target or codec, the file
    // exists, its folder is missing or not writable, less free space than the reserve). `now` is the wall clock (Unix
    // seconds), and `gigEnd` the end of the Gig window it reads inside, if any.
    void start(Settings settings, double now, std::optional<double> gigEnd = {});
    // Any thread; returns once the movie is written. Nothing happens when not recording.
    void stop();
    Status status() const;

    // Where the frame at `engineTime` lands while recording: the movie file and its time in that movie (0 for a time
    // before the first frame). `settled` once the writer has passed that time, so a later rollover cannot move it.
    struct Position {
        std::filesystem::path file;
        double mediaTime = 0.0;
        bool settled = false;
    };
    std::optional<Position> position(double engineTime) const;

    // For the harness: wait until every frame rendered so far is written or dropped.
    void waitUntilWritten();
    // For the harness: every frame takes this much longer to write, as with a slow encoder or disk.
    void setWriteDelay(std::chrono::microseconds delay);
    // For the harness: finishing a full file takes `finish` longer, and setting up a video encoder holds up every
    // encoder for `encoderSetup`, as a second hardware HEVC session does on the Mac mini.
    void setRolloverDelays(std::chrono::microseconds finish, std::chrono::microseconds encoderSetup);
    // Any thread: later recordings take their audio track from `audio`; it must outlive the recorder or be reset first.
    // `source` names the input; without it the analysis counts as running once it has a sample rate.
    void setAudio(AudioAnalysis* audio, AudioSourceOf source = {});
    // Any thread: the audio track a recording started now would get.
    AudioPlan audioPlan(bool wanted) const;
    // For the harness: later recordings ask `provider` (empty: the volume itself) for the free space, every `interval`.
    void setSpaceCheck(SpaceProvider provider, std::chrono::milliseconds interval);

    static uint64_t reserveOf(const Settings& settings, uint64_t total);

    // Renames each playable partial file a killed engine left in `folder` to the movie it was recording, and logs
    // every partial file it finds.
    static void recover(const std::filesystem::path& folder);
    // Why a recording to the settings' path cannot start: its folder is missing or not writable. Empty when it can,
    // and for the default folder, which a start creates.
    static std::string folderProblem(const std::filesystem::path& path);
    // The movie a settings' path names at this moment.
    static std::filesystem::path movieFile(const std::filesystem::path& path);

private:
    struct Session;

    void ended(Session& session, const Ended& ended);
    void spaceChanged();
    static void removeAudioTap(Session& session);

    Engine& engine_;
    double fps_;
    OnEnded onEnded_;
    OnSpace onSpace_;
    // Serialises start and stop.
    std::mutex control_;
    mutable std::mutex mutex_;
    std::shared_ptr<Session> session_;
    std::chrono::microseconds writeDelay_{0};
    std::chrono::microseconds finishDelay_{0};
    std::chrono::microseconds encoderSetup_{0};
    AudioAnalysis* audio_ = nullptr;
    AudioSourceOf audioSource_;
    SpaceProvider space_;
    std::chrono::milliseconds spaceInterval_{2000};
};

} // namespace azl
