#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// The platform-independent half of recording: settings, file naming, the disk reserve, rollover and the audio track's
// timing. Recorder (output/recorder.hpp) feeds it the engine's frames; a MovieEncoder per platform turns them into
// movie files (VideoToolbox and AVFoundation on Apple, FFmpeg elsewhere).
namespace azl::recording {

struct Settings {
    // "composition" or "screen:N".
    std::string target = "composition";
    // "hevc" or "prores".
    std::string codec = "hevc";
    // A .mov file, or a folder that gets a file named after the date and time. Empty: ~/Movies/Nocturne, on Windows
    // Nocturne in the user's Videos folder.
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

struct Ended {
    std::filesystem::path file;
    uint64_t frames = 0;
    uint64_t dropped = 0;
    // "stopped", "disk full", "max length", "gig over" or "failed: ...".
    std::string reason;
    // The movie is complete at `file`; otherwise nothing was left there.
    bool kept = false;
};

struct AudioPlan {
    std::string state = "off";
    std::string reason;
    std::string source;
};

// Where the frame at an engine time lands: the movie file and its time in that movie (0 for a time before the first
// frame). `settled` once the writer has passed that time, so a later rollover cannot move it.
struct Position {
    std::filesystem::path file;
    double mediaTime = 0.0;
    bool settled = false;
};

// AAC takes it from any input, and it matches video work.
inline constexpr double kTrackRate = 48000.0;

// A picture for the encoder, owned by whoever made it: a CVPixelBuffer on Apple, a YCbCrPicture elsewhere.
using Picture = std::shared_ptr<void>;

// One movie file of a recording, opened by its MovieEncoder.
class MovieFile {
public:
    virtual ~MovieFile() = default;
    // Whether it got the AAC track it was asked for.
    virtual bool hasAudio() const = 0;
    // Writer thread: whether the audio track takes more now; when it does not, samples are dropped and counted.
    virtual bool audioReady() const = 0;
    // Writer thread: `count` samples at kTrackRate, the first at sample `at` of the movie. False when the write failed.
    virtual bool appendAudio(const float* samples, size_t count, int64_t at) = 0;
    // Any thread, once no more frames are handed out for it: waits for the encoder's frames for this file, then writes
    // the movie out. True when it is complete.
    virtual bool finish() = 0;
    // Gives the file up unfinished and closes it.
    virtual void cancel() = 0;
    // Neither finished, cancelled nor failed.
    virtual bool writing() const = 0;
    // The last write failed because the disk is full.
    virtual bool diskFull() const = 0;
    virtual std::string error() const = 0;
    // The bytes written so far, where the file's size on disk lags behind them.
    virtual std::optional<uint64_t> bytes() const { return std::nullopt; }
};

// The one video encoder of a recording. Every file the recording rolls over to takes its output as it is, so a rollover
// loses no frame.
class MovieEncoder {
public:
    // What the encoder did with the frames it was handed; it may say so on a thread of its own.
    struct Counts {
        std::atomic<uint64_t> frames{0};
        std::atomic<uint64_t> dropped{0};
        // An encoded frame could not be written.
        std::atomic<bool> failed{false};
    };

    virtual ~MovieEncoder() = default;
    // Any thread, also while another file is written: a movie at `partial` with a video track and, when `audioRate` is
    // set, an AAC track. Throws std::runtime_error when it cannot.
    virtual std::unique_ptr<MovieFile> open(const std::filesystem::path& partial, double audioRate) = 0;
    // Writer thread: hands the picture to the encoder for `file`, at `seconds - origin` in its movie, a key frame when
    // `key`. False when the encoder refused it.
    virtual bool encode(const Picture& picture, double seconds, double origin, bool key, MovieFile& file) = 0;
    // Writer thread: returns once every frame handed out is in its file.
    virtual void complete() = 0;
    // Whether it runs in hardware, once known.
    virtual std::optional<bool> hardware() const = 0;
};

// Makes the recording's encoder; the encoder reports into `counts`, which outlives it.
using EncoderFactory = std::function<std::unique_ptr<MovieEncoder>(MovieEncoder::Counts& counts)>;

// Writes one recording: the pictures queued for it, which the writer thread hands to the encoder, the audio track at
// its capture times on the frames' clock, rollover to the next numbered file, the free space checked by a thread of its
// own and by the writer, and the end by itself (maximum length, Gig window). Pictures and audio come in from any thread
// and never wait for the writer; the owner bounds the pictures (its pool) and drops what does not fit.
//
// The movie is written to a hidden partial file next to the target and renamed into place once complete, so the target
// path only ever holds a whole movie.
class Writer {
public:
    using OnEnded = std::function<void(const Ended&)>;
    using OnSpace = std::function<void()>;

    // Throws std::runtime_error when it cannot start: the path is no .mov file, the file exists, its folder is missing
    // or not writable, less free space than the reserve. `onEnded` runs once on the writer thread, `onSpace` on the
    // checking thread when the space level changes.
    Writer(Settings settings, AudioPlan audio, SpaceProvider space, std::chrono::milliseconds spaceInterval,
           OnEnded onEnded, OnSpace onSpace);
    ~Writer();

    // Makes the encoder and opens the first file; throws when it cannot. With `encoderSetup` (the harness's stand-in for
    // a second hardware HEVC session on the Mac mini), setting up the encoder holds the encoder hardware that long.
    void open(const EncoderFactory& make, std::chrono::microseconds encoderSetup);
    // Starts the writer and the space check. `now` is the wall clock (Unix seconds), and `gigEnd` the end of the Gig
    // window it records inside, if any.
    void start(double now, std::optional<double> gigEnd, std::chrono::microseconds writeDelay,
               std::chrono::microseconds finishDelay);

    // Any thread, never waits for the writer: a picture is on its way (filled on the GPU).
    void begin();
    // Any thread: the picture begun is ready, or failed (`ok` false) and is dropped.
    void queued(Picture picture, double seconds, bool ok);
    // A frame that never got a picture.
    void drop() { counts.dropped++; }
    // Any thread, never waits for the writer: audio of the analysis's input, at its capture time.
    void heard(const float* samples, size_t count, double time, double sampleRate);
    // Any thread: the writer finishes the movie and ends.
    void close();
    // Until no picture is on its way, and unless `inFlightOnly` the writer has caught up.
    void waitIdle(bool inFlightOnly);
    void join();

    std::filesystem::path currentFile() const;
    std::optional<Position> position(double engineTime) const;

    // Set before start() and not changed after, except where atomic.
    Settings settings;
    std::optional<bool> hardware;
    // kTrackRate, or 0 without an audio track.
    double audioRate = 0.0;
    AudioPlan audioPlan;
    // When and why the recording finishes by itself.
    std::optional<double> stopsAt;
    std::string stopsFor;
    MovieEncoder::Counts counts;
    std::atomic<uint64_t> audioDropped{0};
    // As of the last space check.
    std::atomic<SpaceLevel> spaceLevel{SpaceLevel::Unknown};
    std::atomic<uint64_t> freeBytes{0};
    std::atomic<uint64_t> reserve{0};
    // For the harness: every frame takes this much longer to write.
    std::atomic<int64_t> delayMicros{0};
    // Set by the owner when it learns of the end, before it tells anyone.
    std::atomic<bool> done{false};

private:
    struct Item {
        Picture picture;
        double seconds;
    };
    struct Heard {
        std::vector<float> samples;
        double time;
        double rate;
    };
    // One movie file of the recording.
    struct Segment {
        std::unique_ptr<MovieFile> movie;
        std::filesystem::path file;
        std::filesystem::path partial;
        // The engine time of its first frame, which is time 0 of its movie.
        std::optional<double> origin;
        // The next sample its audio track takes.
        int64_t audioNext = -1;
        uint64_t frames = 0;
        // The recording's dropped count when it took over.
        uint64_t droppedBefore = 0;
    };
    class Resampler;

    void run();
    void watch();
    bool rollDue(double seconds) const;
    bool stopDue(double seconds) const;
    bool spaceCheckDue(double seconds);
    bool spaceFull();
    Segment openSegment(const std::filesystem::path& movie);
    void prepare();
    bool roll(std::string& reason);
    void retire(Segment old);
    void discardPrepared();
    bool write(const Item& item, bool keyFrame);
    bool writeAudio(const Heard& heard);
    bool placeAudio(const std::vector<float>& samples, double time);
    void finish(std::string reason);

    OnEnded onEnded_;
    OnSpace onSpace_;
    // The movie the settings name; numbered per segment when the recording rolls over.
    std::filesystem::path base_;
    std::filesystem::path folder_;
    int number_ = 1;
    // Writer thread only: the engine time of its last space check.
    std::optional<double> spaceCheckedAt_;
    // The wall clock at the start.
    double startedAt_ = 0.0;
    // Writer thread only, but `file_`, which status reads, changes under `mutex_`.
    Segment segment_;
    std::filesystem::path file_;
    std::unique_ptr<MovieEncoder> encoder_;
    // The next numbered file, opened while the current one is written. Writer thread only.
    std::future<Segment> prepared_;
    // Segments rolled over and still being finished.
    std::vector<std::future<void>> retiring_;
    SpaceProvider space_;
    std::chrono::milliseconds spaceInterval_;
    // Writer thread only.
    std::unique_ptr<Resampler> resampler_;
    std::vector<float> resampled_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::deque<Item> queue_;
    std::deque<Heard> audio_;
    size_t audioQueued_ = 0;
    // The engine time of the first recorded frame, which is time 0 of the movie.
    std::optional<double> origin_;
    // Each file with the engine time of its first frame, and the engine time of the last frame written.
    std::vector<std::pair<std::filesystem::path, double>> starts_;
    std::optional<double> written_;
    int inFlight_ = 0;
    bool writing_ = false;
    bool closing_ = false;
    // Why the recording ends when something other than close() closed it.
    std::string closedFor_;
    std::atomic<int64_t> finishDelayMicros_{0};
    std::atomic<int64_t> encoderSetupMicros_{0};
    // With the harness's encoder setup stand-in: the hardware every encoder shares.
    std::mutex encoderHardware_;
    std::thread thread_;
    std::thread watcher_;
};

uint64_t reserveOf(const Settings& settings, uint64_t total);
// The free and total bytes of the volume holding `folder`; unknown (all free) when it cannot tell.
Space volumeSpace(const std::filesystem::path& folder);
// Renames each partial file a killed engine left in `folder` that `plays` (has video frames) to the movie it was
// recording, and logs every partial file it finds.
void recover(const std::filesystem::path& folder, const std::function<bool(const std::filesystem::path&)>& plays);
// Why a recording to the settings' path cannot start: its folder is missing or not writable. Empty when it can, and for
// the default folder, which a start creates.
std::string folderProblem(const std::filesystem::path& path);
// Whether this process may create files in `folder`.
bool writable(const std::filesystem::path& folder);
// The movie a settings' path names at this moment.
std::filesystem::path movieFile(const std::filesystem::path& path);
// Throws std::runtime_error for a codec other than "hevc" or "prores".
void checkCodec(const std::string& codec);

} // namespace azl::recording
