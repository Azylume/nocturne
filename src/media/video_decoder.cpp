#include "media/video_decoder.hpp"

#include "media/hap.hpp"
#include "media/media_library.hpp"
#include "media/video_source.hpp"
#include "platform/profile.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace azl {

namespace {

constexpr auto kWaitTimeout = std::chrono::seconds(5);
constexpr int kMinUnit = 4;
// Frames kept just behind the playhead, and just above it playing backward.
constexpr int kBehind = 8;
// Beyond these frames ahead of the playhead, the playhead's own frames are decoded at these multiples of the clip's
// frame rate, not as fast as the decoder goes: on the Mac mini a burst of decoding when a column starts (five clips
// filling their second ahead at once) or when the next column parks delays the GPU's frames past their refresh. With a
// column trigger every bar the fills of one column and the parking of the next overlap; at these rates five 1080p30
// layers peak near 230 decoded frames/s on the Mac mini instead of 285.
constexpr int kUrgentPlaying = 4;
constexpr double kFillRate = 1.1;
constexpr int kUrgentParked = 1;
constexpr double kParkRate = 0.25;
// A reader this many frames behind the keyframe a seek would decode from reads on instead.
constexpr int kReadOnSlack = 16;
// How far the decode plan skips spans that failed to decode, looking for frames to restart at.
constexpr size_t kMaxPlanRanges = 64;

// One read on a worker thread. The worker touches only this and the reader it captured, never the decoder, so a read
// that returns after its decoder is gone is harmless. A reader is never cancelled while it reads: that frees what the
// read is waiting on, and the read crashes or waits for good.
struct PendingRead {
    std::mutex mutex;
    std::condition_variable done;
    SourceRead sample;
    bool finished = false;
    // The decode thread stopped waiting: the worker drops what it reads and cancels the reader.
    bool abandoned = false;
};

// Reads the next sample on a worker thread, after blocking for `stalled` (fault injection). A read its caller has
// abandoned by the time it returns drops the sample and cancels the reader.
void readOnWorker(std::shared_ptr<SourceReader> reader, std::shared_ptr<PendingRead> pending,
                  std::chrono::milliseconds stalled)
{
    runRead([reader = std::move(reader), pending = std::move(pending), stalled] {
        std::this_thread::sleep_for(stalled);
        SourceRead sample = reader->read();
        std::unique_lock lock(pending->mutex);
        if (pending->abandoned) {
            lock.unlock();
            sample.reset();
            reader->cancel();
            return;
        }
        pending->sample = std::move(sample);
        pending->finished = true;
        pending->done.notify_all();
    });
}

// The read's sample (none at the end of the stream); nothing when it outlasted `deadline` or was abandoned, which
// leaves the reader to the read.
std::optional<SourceRead> awaitRead(PendingRead& pending, std::chrono::milliseconds deadline)
{
    std::unique_lock lock(pending.mutex);
    pending.done.wait_for(lock, deadline, [&] { return pending.finished || pending.abandoned; });
    pending.abandoned = pending.abandoned || !pending.finished;
    if (pending.abandoned)
        return std::nullopt;
    return std::exchange(pending.sample, std::nullopt);
}

PictureFormat formatOf(const std::vector<HapImage>& images)
{
    if (images.size() == 2)
        return PictureFormat::YCoCgAlpha;
    switch (images.front().texture) {
    case HapTexture::BC1: return PictureFormat::Rgb;
    case HapTexture::BC3: return PictureFormat::Rgba;
    case HapTexture::YCoCgBC3: return PictureFormat::YCoCg;
    case HapTexture::BC4: break;
    }
    return PictureFormat::YCbCr;
}

int64_t hapFrameBytes(HapCodec codec, int width, int height)
{
    switch (codec) {
    case HapCodec::Hap: return int64_t(hapTextureBytes(HapTexture::BC1, width, height));
    case HapCodec::HapAlpha:
    case HapCodec::HapQ: return int64_t(hapTextureBytes(HapTexture::BC3, width, height));
    case HapCodec::HapQAlpha:
        return int64_t(hapTextureBytes(HapTexture::BC3, width, height) + hapTextureBytes(HapTexture::BC4, width, height));
    case HapCodec::None: break;
    }
    return 0;
}

// What is wrong with a HAP sample, or an empty string when it decodes into `images`.
std::string decodeHapSample(const SourceSample& sample, const SourceTrack& t, std::vector<HapImage>& images)
{
    if (!sample.bytes)
        return "a HAP sample without data";
    std::string error = decodeHapFrame(sample.bytes, sample.size, t.width, t.height, images);
    if (error.empty() && images.size() == 1 && images.front().texture == HapTexture::BC4)
        error = "alpha-only HAP is not supported";
    return error.empty() ? "" : "HAP frame: " + error;
}

// Where the units reverse play decodes in one pass start: GOPs, short ones merged up to `merge` frames, those longer
// than `longest` split into equal parts.
std::vector<int> unitStartsOf(const std::vector<int>& keyframes, int frames, int merge, int longest)
{
    std::vector<int> starts{0};
    for (size_t g = 0; g < keyframes.size(); ++g) {
        const int begin = keyframes[g];
        const int end = g + 1 < keyframes.size() ? keyframes[g + 1] : frames;
        if (end - starts.back() <= merge)
            continue;
        if (starts.back() != begin)
            starts.push_back(begin);
        const int parts = (end - begin + longest - 1) / longest;
        for (int i = 1; i < parts; ++i)
            starts.push_back(begin + (end - begin) * i / parts);
    }
    return starts;
}

struct Range {
    int begin = 0;
    int end = 0;

    bool contains(int index) const { return index >= begin && index < end; }
};

} // namespace

std::array<float, 12> yuvToRgbRows(YuvMatrix matrix, bool fullRange)
{
    double kr = 0.2126, kb = 0.0722;
    if (matrix == YuvMatrix::Rec601)
        kr = 0.299, kb = 0.114;
    if (matrix == YuvMatrix::Rec2020)
        kr = 0.2627, kb = 0.0593;
    const double kg = 1.0 - kr - kb;
    const double ys = fullRange ? 1.0 : 255.0 / 219.0;
    const double yo = fullRange ? 0.0 : 16.0 / 255.0;
    const double cs = fullRange ? 1.0 : 255.0 / 224.0;
    const double co = 128.0 / 255.0;

    const double rCr = 2.0 * (1.0 - kr) * cs;
    const double gCb = -2.0 * kb * (1.0 - kb) / kg * cs;
    const double gCr = -2.0 * kr * (1.0 - kr) / kg * cs;
    const double bCb = 2.0 * (1.0 - kb) * cs;
    const double y0 = -ys * yo;
    return {float(ys), 0.0f, float(rCr), float(y0 - rCr * co),
            float(ys), float(gCb), float(gCr), float(y0 - (gCb + gCr) * co),
            float(ys), float(bCb), 0.0f, float(y0 - bCb * co)};
}

ProbeResult probeMedia(const std::filesystem::path& file, std::chrono::milliseconds stall)
{
    FrameworkScope scope;
    try {
        const std::unique_ptr<VideoSource> source = openVideoSource(nullptr, file.string());
        const SourceTrack& track = source->track();
        std::shared_ptr<SourceReader> reader;
        try {
            reader = source->startReading(0);
        } catch (const MediaError& e) {
            return {e.status, std::string("cannot decode: ") + e.what()};
        }
        auto pending = std::make_shared<PendingRead>();
        readOnWorker(reader, pending, stall);
        const std::optional<SourceRead> read = awaitRead(*pending, kProbeDeadline);
        if (!read)
            return {MediaStatus::Corrupt, "no frame within " + std::to_string(kProbeDeadline.count()) +
                                              " ms: reading the file stalls the decoder"};
        const SourceRead& sample = *read;
        std::string hapError;
        if (sample && track.hap != HapCodec::None) {
            std::vector<HapImage> images;
            hapError = decodeHapSample(*sample, track, images);
        }
        if (!hapError.empty()) {
            reader->cancel();
            return {MediaStatus::Corrupt, hapError};
        }
        if (!sample || (track.hap == HapCodec::None && !sample->picture)) {
            const std::optional<MediaError> failure = reader->failure();
            return {failure ? failure->status : MediaStatus::Corrupt,
                    std::string("no decodable frame: ") + (failure ? failure->what() : "unknown error")};
        }
        reader->cancel();
        return {};
    } catch (const MediaError& e) {
        return {e.status, e.what()};
    }
}

struct VideoDecoder::Impl {
    struct Failure {
        Range frames;
        // The playhead has been in it; it is retried once the playhead is out again.
        bool visited = false;
    };

    struct Reader {
        std::shared_ptr<SourceReader> reading;
        int start = 0;
        int next = 0;
        int last = -1;
        // The last picture read and the frames it fills, `first` to `last` (more than one across a hole in a variable
        // frame rate stream). Frames the cache did not want yet are taken from it later, without a seek to read them
        // again. A HAP picture is kept as its frame.
        int first = 0;
        std::shared_ptr<const void> pixels;
        VideoFrameRef picture;

        bool holds(int index) const { return (pixels || picture) && index >= first && index <= last; }
    };

    // What to decode, most urgent first, and what to keep. The first `playheadRanges` of `decode` are the playhead's,
    // the rest prefetch targets.
    struct Plan {
        std::vector<Range> decode;
        std::vector<Range> keep;
        size_t playheadRanges = 0;
    };

    struct Next {
        int index = 0;
        bool prefetch = false;
        // The frame a forward playhead needs next once its own frames are all decoded.
        std::optional<int> playheadNext;
    };

    std::unique_ptr<VideoSource> source;
    SourceTrack track;
    // Decode thread only: a HAP frame's decompressed textures.
    std::vector<HapImage> hapImages;
    Options options;
    std::filesystem::path path;
    uintmax_t fileSize = 0;
    std::vector<int> unitStarts;
    // Frames decoded ahead of the playhead and after each target: one second, more than a cold seek into a 5 s GOP
    // takes on the Mac mini (~350 ms).
    int ahead = 1;
    int64_t frameBytes = 0;

    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable decoded;
    enum class Mode { Idle, Parked, Playing };

    std::map<int, VideoFrameRef> frames;
    int frameCount = 0;
    int playhead = 0;
    bool forward = true;
    Mode mode = Mode::Idle;
    // Set when the mode changes, so the decode thread evicts what the new mode no longer keeps.
    bool modeChanged = false;
    std::vector<FrameTarget> targets;
    std::vector<Failure> failures;
    int failuresInARow = 0;
    bool broken = false;
    bool running = true;
    // The decode thread has left run().
    bool finished = false;
    std::condition_variable stopped;
    std::optional<int> fault;
    std::optional<std::pair<int, std::chrono::milliseconds>> stall;
    // The read the decode thread waits for, so closing can stop the wait.
    std::shared_ptr<PendingRead> inFlight;
    // Held while onFailure runs; closing takes it, after which the callback never runs.
    std::timed_mutex callbackMutex;
    bool callbacks = true;

    // Decode thread only: the playhead's reader and the prefetch reader.
    std::array<std::optional<Reader>, 2> readers;
    int stallsInARow = 0;
    // Paced decoding: the earliest time for the next paced frame; none while the harness waits for frames.
    std::chrono::steady_clock::time_point nextPaced;
    int waiting = 0;
    bool hurried = false;

    std::thread thread;

    const std::vector<int>& keyframes() const { return track.keyframes; }

    Range unit(int index) const
    {
        auto it = std::upper_bound(unitStarts.begin(), unitStarts.end(), index);
        const int end = it == unitStarts.end() ? frameCount : std::min(*it, frameCount);
        return {*(it - 1), end};
    }

    Range unitBefore(const Range& r) const { return unit((r.begin == 0 ? frameCount : r.begin) - 1); }

    int keyframeBefore(int index) const
    {
        return *(std::upper_bound(keyframes().begin(), keyframes().end(), index) - 1);
    }

    int keyframeAfter(int index) const
    {
        auto it = std::upper_bound(keyframes().begin(), keyframes().end(), index);
        return it == keyframes().end() ? frameCount : std::min(*it, frameCount);
    }

    bool isFailed(int index) const
    {
        return std::any_of(failures.begin(), failures.end(), [&](const Failure& f) { return f.frames.contains(index); });
    }

    bool fullyFailed(const Range& r) const
    {
        for (int index = r.begin; index < r.end; ++index)
            if (!isFailed(index))
                return false;
        return true;
    }

    Range window(const FrameTarget& t) const
    {
        const int index = std::clamp(t.index, 0, frameCount - 1);
        if (t.forward)
            return {index, std::min(index + ahead, frameCount)};
        return {std::max(0, index - ahead + 1), index + 1};
    }

    // Forward: the next `ahead` frames that have not failed, wrapping at the end, so decoding restarts at the next
    // keyframe after a failure.
    void planForward(Plan& plan) const
    {
        int steps = 0;
        for (int left = ahead; left > 0 && steps < frameCount; ++steps)
            left -= !isFailed((playhead + steps) % frameCount);
        plan.decode.push_back({playhead, std::min(playhead + steps, frameCount)});
        if (playhead + steps > frameCount)
            plan.decode.push_back({0, playhead + steps - frameCount});
        plan.keep.push_back({std::max(0, playhead - kBehind), playhead});
    }

    // Backward: the playhead's unit up to the playhead, then the unit below it whole (or the first below that has not
    // failed), each in one forward pass, so every frame is decoded once.
    void planBackward(Plan& plan) const
    {
        const Range current = unit(playhead);
        plan.decode.push_back({current.begin, playhead + 1});
        for (Range r = current; plan.decode.size() < kMaxPlanRanges;) {
            r = unitBefore(r);
            plan.decode.push_back(r);
            if (!fullyFailed(r))
                break;
        }
        plan.keep.push_back({playhead + 1, std::min(playhead + 1 + kBehind, frameCount)});
    }

    // A forward start parks the whole second the clip plays ahead, decoded at the parked pace while it waits, so clips
    // starting together (a column, Autopilot on the same bar) need no burst of decoding when they start.
    Range parked(int index, bool forwards) const
    {
        if (forwards)
            return {index, std::min(index + ahead, frameCount)};
        return {unit(index).begin, index + 1};
    }

    // Targets come after the playhead's needs but are never evicted for them.
    Plan plan() const
    {
        Plan plan;
        if (mode == Mode::Idle)
            return plan;
        if (mode == Mode::Parked) {
            plan.decode.push_back(parked(playhead, forward));
            plan.keep = plan.decode;
            plan.playheadRanges = 1;
            return plan;
        }
        if (forward)
            planForward(plan);
        else
            planBackward(plan);
        plan.playheadRanges = plan.decode.size();
        for (const FrameTarget& t : targets)
            plan.decode.push_back(window(t));
        plan.keep.insert(plan.keep.end(), plan.decode.begin(), plan.decode.end());
        return plan;
    }

    // Each range is read forward from its first missing frame, the only way a reader goes.
    std::optional<Next> nextToDecode() const
    {
        if (broken)
            return std::nullopt;
        const Plan p = plan();
        std::optional<int> playheadNext;
        if (mode == Mode::Playing && forward)
            playheadNext = p.decode[p.playheadRanges - 1].end % frameCount;
        for (size_t i = 0; i < p.decode.size(); ++i)
            for (int index = p.decode[i].begin; index < p.decode[i].end; ++index)
                if (!frames.count(index) && !isFailed(index))
                    return Next{index, i >= p.playheadRanges, playheadNext};
        return std::nullopt;
    }

    bool wanted(int index) const
    {
        const Plan p = plan();
        return std::any_of(p.keep.begin(), p.keep.end(), [&](const Range& r) { return r.contains(index); });
    }

    void evict()
    {
        const Plan p = plan();
        const size_t before = frames.size();
        std::erase_if(frames, [&](const auto& f) {
            return std::none_of(p.keep.begin(), p.keep.end(), [&](const Range& r) { return r.contains(f.first); });
        });
        options.stats->cachedBytes -= int64_t(before - frames.size()) * frameBytes;
    }

    // Under the lock: moves the playhead and sets the targets, waking the decoder. Returns the clamped index.
    int seek(int index, bool forwards, const std::vector<FrameTarget>& wantedTargets, Mode wantedMode = Mode::Playing)
    {
        index = std::clamp(index, 0, frameCount - 1);
        if (index == playhead && forwards == forward && wantedTargets == targets && wantedMode == mode)
            return index;
        playhead = index;
        forward = forwards;
        targets = wantedTargets;
        modeChanged = modeChanged || wantedMode != mode;
        mode = wantedMode;
        for (Failure& f : failures)
            f.visited = f.visited || f.frames.contains(playhead);
        std::erase_if(failures, [&](const Failure& f) { return f.visited && !f.frames.contains(playhead); });
        wake.notify_one();
        return index;
    }

    // Under the lock, on the decode thread: the rate, in multiples of the frame rate, `target` is decoded at; 0 for as
    // fast as possible. With prefetch targets only a reader reading on to the very frame is paced: frames read on the
    // way to a frame deep in a GOP go at full speed, and while the playhead's next frame waits for its pace the decoder
    // works on the targets (nextPrefetch).
    double paceRate(const Next& target) const
    {
        if (waiting || hurried || target.prefetch || !forward || mode == Mode::Idle)
            return 0.0;
        const std::optional<Reader>& reader = readers[0];
        if (mode == Mode::Playing && !targets.empty() && (!reader || reader->next != target.index))
            return 0.0;
        const int ahead = (target.index - playhead + frameCount) % frameCount;
        if (mode == Mode::Parked)
            return ahead >= kUrgentParked ? kParkRate : 0.0;
        return ahead >= kUrgentPlaying ? kFillRate : 0.0;
    }

    // Under the lock: the first prefetch frame to decode that the playhead's own frames do not cover (a BPM-synced
    // clip's first-frame target overlaps a fresh start, and those frames keep the playhead's pace).
    std::optional<Next> nextPrefetch() const
    {
        if (broken)
            return std::nullopt;
        const Plan p = plan();
        auto playheads = [&](int index) {
            for (size_t i = 0; i < p.playheadRanges; ++i)
                if (p.decode[i].contains(index))
                    return true;
            return false;
        };
        std::optional<int> playheadNext;
        if (mode == Mode::Playing && forward && p.playheadRanges > 0)
            playheadNext = p.decode[p.playheadRanges - 1].end % frameCount;
        for (size_t i = p.playheadRanges; i < p.decode.size(); ++i)
            for (int index = p.decode[i].begin; index < p.decode[i].end; ++index)
                if (!frames.count(index) && !isFailed(index) && !playheads(index))
                    return Next{index, true, playheadNext};
        return std::nullopt;
    }

    VideoFrameRef find(int index) const
    {
        auto it = frames.find(index);
        return it == frames.end() ? nullptr : it->second;
    }

    void run()
    {
        while (true) {
            Next target;
            {
                std::unique_lock lock(mutex);
                std::optional<Next> next;
                wake.wait(lock, [&] { return !running || modeChanged || (next = nextToDecode()); });
                if (!running)
                    break;
                modeChanged = false;
                evict();
                next = nextToDecode();
                if (!next) {
                    decoded.notify_all();
                    continue;
                }
                target = *next;
                if (const double rate = paceRate(target); rate > 0.0) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now < nextPaced) {
                        if (const std::optional<Next> prefetch = nextPrefetch()) {
                            target = *prefetch;
                        } else {
                            const int at = playhead;
                            wake.wait_until(lock, nextPaced,
                                            [&] { return !running || modeChanged || playhead != at || waiting; });
                            continue;
                        }
                    } else {
                        nextPaced = now + std::chrono::microseconds(int64_t(1e6 / (track.fps * rate)));
                    }
                }
            }
            const auto started = std::chrono::steady_clock::now();
            {
                FrameworkScope scope;
                decode(target);
            }
            const auto busy = std::chrono::steady_clock::now() - started;
            std::lock_guard lock(mutex);
            if (running)
                options.stats->busyMicros += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(busy).count());
            decoded.notify_all();
        }
        for (std::optional<Reader>& r : readers)
            if (r)
                r->reading->cancel();
        std::lock_guard lock(mutex);
        finished = true;
        stopped.notify_all();
    }

    // The decode thread counts into the shared stats only while the decoder is open, so a thread left running at close
    // changes nothing outside its own state. Every stats write checks `running` under the lock.
    void count(std::atomic<uint64_t> DecodeStats::*counter)
    {
        std::lock_guard lock(mutex);
        if (running)
            ++(*options.stats.*counter);
    }

    // Any thread but the decode thread: stops decoding and the callback, and waits up to `timeout` for the decode
    // thread to leave run(). False when it did not.
    bool stop(std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        {
            std::lock_guard lock(mutex);
            running = false;
            wake.notify_all();
            if (inFlight) {
                std::lock_guard read(inFlight->mutex);
                inFlight->abandoned = true;
                inFlight->done.notify_all();
            }
            options.stats->cachedBytes -= int64_t(frames.size()) * frameBytes;
            frames.clear();
        }
        std::unique_lock callback(callbackMutex, deadline);
        if (callback.owns_lock())
            callbacks = false;
        std::unique_lock lock(mutex);
        return stopped.wait_until(lock, deadline, [&] { return finished; }) && callback.owns_lock();
    }

    // Reads one frame on the way to `next`.
    void decode(const Next& next)
    {
        AZL_ZONE("decode");
        Reader* reader = readerFor(next);
        if (!reader)
            return;
        if (reader->holds(next.index)) {
            for (int index = next.index; index <= reader->last; ++index)
                keepHeld(*reader, index);
            return;
        }
        const std::optional<SourceRead> read = this->read(*reader);
        if (!read)
            return giveUp(*reader, next.index);
        std::this_thread::sleep_for(options.frameCost);
        if (!*read)
            return finish(*reader);
        const SourceSample& sample = **read;
        const int index = sample.index;
        if (const std::optional<int> faulty = takeFault(*reader, index)) {
            drop(*reader);
            return failAt(*faulty, "injected decoder fault");
        }
        if (track.hap != HapCodec::None && index >= 0)
            return readHap(*reader, sample, index);
        if (sample.picture && index >= 0) {
            // A hole in the stream (where the reader started, or between the timestamps of a variable frame rate
            // file) shows the next frame there, instead of seeking for a frame that does not exist forever.
            reader->first = reader->last < 0 ? reader->start : std::min(reader->last + 1, index);
            reader->next = index + 1;
            reader->last = index;
            reader->pixels = sample.picture;
            for (int hole = reader->first; hole <= index; ++hole)
                keepHeld(*reader, hole);
            count(&DecodeStats::frames);
            stallsInARow = 0;
        }
    }

    // A HAP frame: decompressed and made into textures only when the cache wants one of the frames it fills. Each HAP
    // frame is a keyframe, so a bad one fails alone and the reader reads on.
    void readHap(Reader& reader, const SourceSample& sample, int index)
    {
        const int first = reader.last < 0 ? reader.start : std::min(reader.last + 1, index);
        reader.next = index + 1;
        reader.last = index;
        reader.first = first;
        reader.pixels.reset();
        reader.picture.reset();
        if (!anyWanted(first, index))
            return;
        const std::string error = decodeHapSample(sample, track, hapImages);
        if (!error.empty())
            return failAt(index, error);
        reader.picture = source->hapFrame(hapImages, formatOf(hapImages), index);
        if (!reader.picture)
            return failAt(index, "cannot make GPU textures for a HAP frame");
        for (int hole = first; hole <= index; ++hole)
            keepHeld(reader, hole);
        count(&DecodeStats::frames);
        stallsInARow = 0;
    }

    bool anyWanted(int first, int last)
    {
        std::lock_guard lock(mutex);
        for (int index = first; index <= last; ++index)
            if (!frames.count(index) && !isFailed(index) && wanted(index))
                return true;
        return false;
    }

    // The reader's next sample (none at the end of the stream), read on a worker thread; nothing when the read
    // outlasted its deadline or the decoder is closing.
    std::optional<SourceRead> read(const Reader& reader)
    {
        auto pending = std::make_shared<PendingRead>();
        std::chrono::milliseconds stalled{0};
        {
            std::lock_guard lock(mutex);
            if (!running)
                return std::nullopt;
            if (stall && reader.start <= stall->first && reader.next >= stall->first)
                stalled = std::exchange(stall, std::nullopt)->second;
            inFlight = pending;
        }
        AZL_ZONE("read sample");
        const auto started = std::chrono::steady_clock::now();
        readOnWorker(reader.reading, pending, stalled);
        std::optional<SourceRead> sample = awaitRead(*pending, deadline(reader));
        const auto took = uint64_t(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
        std::lock_guard lock(mutex);
        inFlight.reset();
        if (running)
            for (uint64_t max = options.stats->maxReadMicros; took > max;)
                options.stats->maxReadMicros.compare_exchange_weak(max, took);
        return sample;
    }

    // A new reader's first read also sets up its decoder, which takes longer while many clips start at once.
    std::chrono::milliseconds deadline(const Reader& reader) const
    {
        return reader.last < 0 ? 4 * options.readDeadline : options.readDeadline;
    }

    // A read outlasted its deadline (or the decoder is closing): the reader is left to the read, which cancels it if it
    // ever returns, and the frame is read again with a new one.
    void giveUp(Reader& reader, int index)
    {
        const std::chrono::milliseconds waited = deadline(reader);
        for (std::optional<Reader>& r : readers)
            if (r && &*r == &reader)
                r.reset();
        {
            std::lock_guard lock(mutex);
            if (!running)
                return;
            options.stats->stalls++;
        }
        std::fprintf(stderr, "clip %s: a read toward frame %d took over %lld ms; the reader is replaced\n",
                     options.name.c_str(), index, (long long)waited.count());
        if (++stallsInARow >= VideoDecoder::kMaxStalls)
            breakDown("reads stalled " + std::to_string(stallsInARow) + " times in a row");
    }

    // The playhead and the prefetch targets each read with their own reader, so prefetching never takes the
    // playhead's reader away. The playhead always holds the reader nearest below the frame it needs next, as after a
    // jump onto a prefetched target: a prefetch reader reading on through those frames would drop them and leave the
    // playhead to seek from the keyframe. A reader reads on when that decodes no more than a seek would; a new one
    // starts at the keyframe and hands over every frame, so reaching a frame deep in a long GOP is many short reads,
    // between which the playhead's frames come first.
    Reader* readerFor(const Next& next)
    {
        const int target = next.index;
        const int key = keyframeBefore(target);
        auto reaches = [&](const std::optional<Reader>& r, int index) {
            return r && (r->holds(index) ||
                         (r->next <= index && index - r->next <= index - keyframeBefore(index) + kReadOnSlack));
        };
        const std::optional<int> playheadNeeds = next.prefetch ? next.playheadNext : target;
        if (playheadNeeds && reaches(readers[1], *playheadNeeds) &&
            (!reaches(readers[0], *playheadNeeds) || readers[1]->next > readers[0]->next))
            std::swap(readers[0], readers[1]);
        std::optional<Reader>& slot = readers[next.prefetch];
        if (reaches(slot, target))
            return &*slot;
        if (slot)
            slot->reading->cancel();
        slot.reset();
        // The source may still read a deleted or unplugged file it has open; a seek is where that shows.
        if (fileChanged()) {
            breakDown("the file changed or vanished while playing");
            return nullptr;
        }
        count(&DecodeStats::seeks);
        std::shared_ptr<SourceReader> reading;
        try {
            reading = source->startReading(key);
        } catch (const MediaError& e) {
            failAt(target, std::string("cannot decode: ") + e.what());
            return nullptr;
        }
        slot = Reader{std::move(reading), key, key, -1, key, nullptr, nullptr};
        return &*slot;
    }

    void drop(Reader& reader)
    {
        for (std::optional<Reader>& r : readers)
            if (r && &*r == &reader) {
                r->reading->cancel();
                r.reset();
            }
    }

    // At the end of a stream, the frames it delivered define the clip's length.
    void finish(Reader& reader)
    {
        const std::optional<MediaError> failure = reader.reading->failure();
        const int end = reader.last >= 0 ? reader.last + 1 : reader.start;
        const int at = reader.next;
        drop(reader);
        if (fileChanged())
            return breakDown("the file changed or vanished while playing");
        if (failure)
            return failAt(at, std::string("decoding failed: ") + failure->what());
        if (end == 0)
            return breakDown("no decodable frames");
        std::lock_guard lock(mutex);
        if (end >= frameCount)
            return;
        frameCount = end;
        playhead = std::min(playhead, frameCount - 1);
        evict();
    }

    std::optional<int> takeFault(const Reader& reader, int index)
    {
        std::lock_guard lock(mutex);
        if (!fault || reader.start > *fault || index < *fault)
            return std::nullopt;
        return std::exchange(fault, std::nullopt);
    }

    // A truncated, replaced or removed file is broken at once, however many reads happened to fail first.
    bool fileChanged() const
    {
        std::error_code error;
        const uintmax_t size = std::filesystem::file_size(path, error);
        return error || size != fileSize;
    }

    // The frames from `index` to the next keyframe count as missing; decoding goes on from that keyframe.
    void failAt(int index, const std::string& reason)
    {
        if (fileChanged())
            return breakDown("the file changed or vanished while playing");
        int restart = 0;
        int inARow = 0;
        {
            std::lock_guard lock(mutex);
            if (running)
                options.stats->errors++;
            restart = keyframeAfter(index);
            failures.push_back({{index, restart}, Range{index, restart}.contains(playhead)});
            inARow = ++failuresInARow;
        }
        std::fprintf(stderr, "clip %s: decoder error at frame %d, restarting at keyframe %d: %s\n",
                     options.name.c_str(), index, restart, reason.c_str());
        if (inARow >= kMaxFailures)
            breakDown(reason);
    }

    void breakDown(const std::string& reason)
    {
        {
            std::lock_guard lock(mutex);
            if (std::exchange(broken, true))
                return;
        }
        std::lock_guard callback(callbackMutex);
        if (callbacks && options.onFailure)
            options.onFailure(reason);
    }

    // Frame `index` from the reader's last picture.
    void keepHeld(const Reader& reader, int index)
    {
        {
            std::lock_guard lock(mutex);
            failuresInARow = 0;
            if (frames.count(index) || isFailed(index) || !wanted(index))
                return;
        }
        VideoFrameRef frame;
        if (reader.picture) {
            auto copy = std::make_shared<VideoFrame>(*reader.picture);
            copy->index = index;
            frame = std::move(copy);
        } else {
            frame = source->frame(reader.pixels, index);
        }
        if (!frame)
            return failAt(index, "cannot make GPU textures for a decoded frame");
        std::lock_guard lock(mutex);
        if (!running)
            return;
        frames[index] = std::move(frame);
        options.stats->cachedBytes += frameBytes;
    }
};

// `cacheBytes` bounds the two units reverse play holds, so GOPs longer than half of it are split.
// Decoders open and close on long-lived threads: what the platform's frameworks hand out to be freed later must be
// freed here, or it lives as long as the thread, the whole show.
VideoDecoder::VideoDecoder(const MediaDevice* device, const std::string& path, Options options)
    : impl_(std::make_shared<Impl>())
{
    FrameworkScope scope;
    open(device, path, std::move(options));
}

void VideoDecoder::open(const MediaDevice* device, const std::string& path, Options options)
{
    Impl& d = *impl_;
    d.source = openVideoSource(device, path);
    d.track = d.source->track();
    d.path = path;
    d.fileSize = std::filesystem::file_size(d.path);
    d.frameCount = d.track.frameCount;
    d.frameBytes = std::max<int64_t>(1, d.track.hap != HapCodec::None
                                            ? hapFrameBytes(d.track.hap, d.track.width, d.track.height)
                                            : d.track.pictureBytes);
    const int64_t cacheFrames = std::min<int64_t>(int64_t(options.cacheBytes) / d.frameBytes, d.frameCount);
    d.ahead = std::max(2, int(std::ceil(d.track.fps)));
    d.unitStarts = unitStartsOf(d.track.keyframes, d.frameCount, d.ahead, std::max(kMinUnit, int(cacheFrames / 2)));
    d.options = std::move(options);
    // The thread shares the decoder's state, so a thread left running at close never outlives it.
    d.thread = std::thread([impl = impl_] {
        becomeDecodeThread();
        AZL_THREAD_NAME("decode " + impl->options.name);
        impl->run();
    });
}

VideoDecoder::~VideoDecoder()
{
    FrameworkScope scope;
    if (impl_->stop(kCloseTimeout)) {
        impl_->thread.join();
    } else {
        std::fprintf(stderr, "clip %s: the decode thread did not stop within %lld ms; it is left to finish alone\n",
                     impl_->options.name.c_str(), (long long)kCloseTimeout.count());
        impl_->thread.detach();
    }
    impl_.reset();
}

ClipLength VideoDecoder::length() const
{
    std::lock_guard lock(impl_->mutex);
    return {impl_->frameCount, impl_->track.fps};
}

VideoFrameRef VideoDecoder::frame(int index, bool forward, const std::vector<FrameTarget>& targets)
{
    std::lock_guard lock(impl_->mutex);
    const int at = impl_->seek(index, forward, targets);
    return impl_->broken ? nullptr : impl_->find(at);
}

VideoFrameRef VideoDecoder::waitFrame(int index, bool forward, const std::vector<FrameTarget>& targets)
{
    std::unique_lock lock(impl_->mutex);
    impl_->waiting++;
    impl_->wake.notify_one();
    impl_->decoded.wait_for(lock, kWaitTimeout, [&] {
        const int at = impl_->seek(index, forward, targets);
        return impl_->broken || impl_->frames.count(at) || impl_->isFailed(at);
    });
    impl_->waiting--;
    const int at = impl_->seek(index, forward, targets);
    return impl_->broken ? nullptr : impl_->find(at);
}

void VideoDecoder::park(int index, bool forward)
{
    std::lock_guard lock(impl_->mutex);
    impl_->seek(index, forward, {}, Impl::Mode::Parked);
}

int64_t VideoDecoder::parkBytes(int index, bool forward) const
{
    std::lock_guard lock(impl_->mutex);
    const Range r = impl_->parked(std::clamp(index, 0, impl_->frameCount - 1), forward);
    return int64_t(r.end - r.begin) * impl_->frameBytes;
}

void VideoDecoder::idle()
{
    std::lock_guard lock(impl_->mutex);
    impl_->seek(impl_->playhead, impl_->forward, {}, Impl::Mode::Idle);
}

void VideoDecoder::waitIdle()
{
    std::unique_lock lock(impl_->mutex);
    impl_->waiting++;
    impl_->wake.notify_one();
    impl_->decoded.wait_for(lock, kWaitTimeout, [&] { return !impl_->nextToDecode(); });
    impl_->waiting--;
}

bool VideoDecoder::settled() const
{
    std::lock_guard lock(impl_->mutex);
    return !impl_->nextToDecode();
}

int64_t VideoDecoder::planBytes() const
{
    std::lock_guard lock(impl_->mutex);
    const Impl::Plan plan = impl_->plan();
    int64_t frames = 0;
    for (int index = 0; index < impl_->frameCount; ++index)
        frames += std::any_of(plan.keep.begin(), plan.keep.end(), [&](const Range& r) { return r.contains(index); });
    return (frames + 1) * impl_->frameBytes;
}

int64_t VideoDecoder::heldBytes() const
{
    std::lock_guard lock(impl_->mutex);
    return int64_t(impl_->frames.size()) * impl_->frameBytes;
}

std::pair<size_t, size_t> VideoDecoder::sizes() const
{
    std::lock_guard lock(impl_->mutex);
    return {impl_->frames.size(), impl_->failures.size()};
}

bool VideoDecoder::broken() const
{
    std::lock_guard lock(impl_->mutex);
    return impl_->broken;
}

void VideoDecoder::injectFault(int index)
{
    std::lock_guard lock(impl_->mutex);
    impl_->fault = index;
    impl_->wake.notify_one();
}

void VideoDecoder::hurry(bool on)
{
    std::lock_guard lock(impl_->mutex);
    impl_->hurried = on;
    impl_->wake.notify_all();
}

void VideoDecoder::injectStall(int index, std::chrono::milliseconds duration)
{
    std::lock_guard lock(impl_->mutex);
    impl_->stall = std::pair{index, duration};
}

} // namespace azl
