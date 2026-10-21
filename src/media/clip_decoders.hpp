#pragma once

#include "media/video_decoder.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace azl {

class MediaLibrary;

// A clip on a layer: the layer's id and the media's content hash. Every clip has its own decoder and playhead.
using ClipKey = std::pair<uint64_t, std::string>;

// A clip a trigger may start next, decoded at its start before it plays: `position` (0..1 of the clip) in its play
// direction.
struct Preload {
    ClipKey clip;
    double position = 0.0;
    bool forward = true;

    bool operator==(const Preload&) const = default;
};

// The decoders of every clip that plays or may play next. One background thread resolves hashes through the Media
// Library and opens and closes decoders, so the render thread never waits for the library or for a file to open.
// Opens of preloads and closes go one at a time with a gap between them (none for a clip that plays, at start-up or
// while the harness waits), so a column trigger's changes spread over the bar.
// A clip whose file is missing (never there, or gone while playing, as with a removed drive) counts as failed until
// the file is back: it is looked for every few seconds, not each time the clip comes up, and opened again once found.
class ClipDecoders {
public:
    // Runs on a background thread when a clip cannot be opened or turns out broken while playing.
    using OnFailure = std::function<void(const ClipKey& clip, const std::string& reason)>;

    // A null library resolves nothing. Each clip's frame cache holds up to `cacheBytes`; parked preloads together hold
    // up to `preloadBytes`. `frameCost` slows every decoder down (harness).
    ClipDecoders(MTL::Device* device, MediaLibrary* library, size_t cacheBytes, size_t preloadBytes,
                 std::chrono::microseconds frameCost = {});
    ~ClipDecoders();

    // Any thread, never blocks: keeps `playing` and `preloads` open and closes the rest. Preloads are parked at their
    // start in the order given while they fit the preload budget; the others stay open but decode nothing. A clip
    // that failed to open is tried again when it starts playing, unless its file is missing.
    void want(std::set<ClipKey> playing, std::vector<Preload> preloads);
    // Null while the clip is opening or when it is unplayable. Never blocks unless `wait`, which waits until the clip
    // has opened (deterministic harness).
    std::optional<ClipLength> length(const ClipKey& clip, bool wait);
    // The clip could not be opened, or broke while playing.
    bool failed(const ClipKey& clip);
    // Clips of this media that could not be opened try again, e.g. once it arrived in the library.
    void retry(const std::string& hash);
    // The clip's frame `index` played in that direction, or null while it is opening, not decoded yet, or unplayable.
    // `targets` are prefetched. Never blocks unless `wait`, which waits until the clip has opened and that frame is
    // decoded.
    VideoFrameRef frame(const ClipKey& clip, int index, bool forward, const std::vector<FrameTarget>& targets, bool wait);
    // Deterministic harness: waits until every wanted clip has opened and decoded what its cache wants.
    void waitIdle();
    // Every wanted clip has opened, or failed to, and decoded what its cache wants. Never blocks.
    bool settled();
    // The same for the preloads only: a clip that plays fills its cache while it plays.
    bool preloaded();
    // For the harness: while held, no clip opens and nothing waits for one to open, as with a slow disk.
    void holdOpens(bool held);
    // While on, every decoder decodes at full speed (VideoDecoder::hurry).
    void hurry(bool on);
    // Fault injection for tests: the clip's decoder fails once when it reaches frame `index`, also when it opens later.
    void injectFault(const ClipKey& clip, int index);
    // Fault injection for tests: one read of the clip's decoder reaching frame `index` blocks for `duration`, also when
    // it opens later.
    void injectStall(const ClipKey& clip, int index, std::chrono::milliseconds duration);
    // Any thread; once it returns, the previous callback no longer runs.
    void onFailure(OnFailure callback);
    const DecodeStats& stats() const { return *stats_; }
    // New bounds, from now on: a lower preload budget drops the preloads past it at once; a lower frame cache bound
    // reopens the clips that do not play now, so they hold no more than it, and a playing clip takes it when it opens
    // next. Any thread, never blocks.
    void setLimits(size_t cacheBytes, size_t preloadBytes);
    // What the playing clips' frame caches and the preloads hold now.
    struct Use {
        int64_t playingBytes = 0;
        int64_t preloadBytes = 0;
    };
    Use use();
    // Container sizes, to see that nothing grows over a long show.
    std::map<std::string, size_t> sizes();

private:
    void run();
    bool round();
    bool roundBody();
    void retryMissing();
    void park();
    void retire();
    void report(const ClipKey& clip, const std::string& reason);
    std::shared_ptr<VideoDecoder> open(const ClipKey& clip);
    std::shared_ptr<VideoDecoder> waitOpen(const ClipKey& clip);
    bool allOpen() const;

    MTL::Device* device_;
    MediaLibrary* library_;
    size_t cacheBytes_;
    size_t preloadBytes_;
    std::chrono::microseconds frameCost_;
    std::shared_ptr<DecodeStats> stats_ = std::make_shared<DecodeStats>();
    std::mutex mutex_;
    std::condition_variable changed_;
    std::set<ClipKey> wanted_;
    std::set<ClipKey> playing_;
    std::vector<Preload> preloads_;
    // A null decoder is a clip that could not be opened.
    std::map<ClipKey, std::shared_ptr<VideoDecoder>> decoders_;
    // Decoders no longer wanted, idle, closing one by one, oldest first.
    std::vector<std::pair<ClipKey, std::shared_ptr<VideoDecoder>>> retired_;
    // Retired for a lower frame cache bound: closed, never taken back.
    std::set<const VideoDecoder*> stale_;
    std::chrono::steady_clock::time_point lastChange_{};
    // Harness waits in progress: nothing is paced while one waits.
    int waiters_ = 0;
    // Faults and stalls injected before their clip opened.
    std::map<ClipKey, int> faults_;
    std::map<ClipKey, std::pair<int, std::chrono::milliseconds>> stalls_;
    // Clips whose file was missing when they opened or while they played, until it is back.
    std::set<ClipKey> missing_;
    std::chrono::steady_clock::time_point retried_{};
    // Media found again (a relink): their broken decoders close at the next round.
    std::set<std::string> retrying_;
    bool held_ = false;
    bool hurried_ = false;
    bool running_ = true;
    std::mutex failureMutex_;
    OnFailure onFailure_;
    std::thread thread_;
};

} // namespace azl
