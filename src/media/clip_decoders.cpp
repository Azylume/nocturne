#include "media/clip_decoders.hpp"

#include "engine/thread_priority.hpp"
#include "media/media_library.hpp"
#include "platform/profile.hpp"
#include "supervisor/crash_guard.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <vector>

namespace azl {

namespace {

constexpr auto kOpenTimeout = std::chrono::seconds(10);
constexpr auto kMissingRetry = std::chrono::seconds(3);
// Decoders open and close at most once per gap unless a playing clip waits for one, so the opens and closes of a
// column trigger spread over the bar instead of hitting the hardware decoder at once.
constexpr auto kChangeGap = std::chrono::milliseconds(150);
// Retired decoders beyond this close without waiting for the gap.
constexpr size_t kMaxRetired = 8;

void raiseMax(std::atomic<uint64_t>& max, std::chrono::steady_clock::time_point since)
{
    const auto took = uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - since).count());
    for (uint64_t m = max; took > m;)
        max.compare_exchange_weak(m, took);
}

// The frame at `position` (0..1) of the clip: forward frame i covers [i, i + 1), backward (i, i + 1].
int startFrame(int frames, double position, bool forward)
{
    const double x = position * frames;
    return std::clamp(forward ? int(x) : int(std::ceil(x)) - 1, 0, frames - 1);
}

} // namespace

ClipDecoders::ClipDecoders(const gpu::Device& device, MediaLibrary* library, size_t cacheBytes, size_t preloadBytes,
                           std::chrono::microseconds frameCost)
    : device_(device), library_(library), cacheBytes_(cacheBytes), preloadBytes_(preloadBytes), frameCost_(frameCost),
      thread_([this] {
          setThreadRole(ThreadRole::Background);
          AZL_THREAD_NAME("clip decoders");
          run();
      })
{
}

ClipDecoders::~ClipDecoders()
{
    {
        std::lock_guard lock(mutex_);
        running_ = false;
        changed_.notify_all();
    }
    thread_.join();
}

void ClipDecoders::want(std::set<ClipKey> playing, std::vector<Preload> preloads)
{
    std::lock_guard lock(mutex_);
    for (const ClipKey& clip : playing)
        if (!playing_.count(clip))
            if (auto it = decoders_.find(clip); it != decoders_.end() && !it->second)
                decoders_.erase(it);
    std::erase_if(preloads, [&](const Preload& p) { return playing.count(p.clip); });
    wanted_ = playing;
    for (const Preload& p : preloads)
        wanted_.insert(p.clip);
    playing_ = std::move(playing);
    preloads_ = std::move(preloads);
    park();
    changed_.notify_all();
}

// Under the lock. Parking an open decoder only moves its playhead; the decode thread does the work.
void ClipDecoders::park()
{
    int64_t left = int64_t(preloadBytes_);
    for (const Preload& p : preloads_) {
        auto it = decoders_.find(p.clip);
        if (it == decoders_.end() || !it->second)
            continue;
        VideoDecoder& decoder = *it->second;
        const int index = startFrame(decoder.length().frames, p.position, p.forward);
        const int64_t bytes = decoder.parkBytes(index, p.forward);
        if (bytes > left) {
            decoder.idle();
            continue;
        }
        left -= bytes;
        decoder.park(index, p.forward);
    }
}

// The live paths call the decoder under the lock, so the render thread never holds the last reference to one: dropping
// it would join its decode thread.
std::optional<ClipLength> ClipDecoders::length(const ClipKey& clip, bool wait)
{
    if (wait) {
        const std::shared_ptr<VideoDecoder> decoder = waitOpen(clip);
        return decoder ? std::optional(decoder->length()) : std::nullopt;
    }
    std::lock_guard lock(mutex_);
    auto it = decoders_.find(clip);
    return it == decoders_.end() || !it->second ? std::nullopt : std::optional(it->second->length());
}

bool ClipDecoders::failed(const ClipKey& clip)
{
    std::lock_guard lock(mutex_);
    if (missing_.count(clip))
        return true;
    auto it = decoders_.find(clip);
    return it != decoders_.end() && (!it->second || it->second->broken());
}

// A clip that broke because its file went away keeps a broken decoder until the decoders' thread closes it, so the
// file is opened again at once wherever the clip is wanted.
void ClipDecoders::retry(const std::string& hash)
{
    std::lock_guard lock(mutex_);
    std::erase_if(decoders_, [&](const auto& entry) { return entry.first.second == hash && !entry.second; });
    std::erase_if(missing_, [&](const ClipKey& clip) { return clip.second == hash; });
    retrying_.insert(hash);
    changed_.notify_all();
}

VideoFrameRef ClipDecoders::frame(const ClipKey& clip, int index, bool forward, const std::vector<FrameTarget>& targets,
                                  bool wait)
{
    if (wait) {
        const std::shared_ptr<VideoDecoder> decoder = waitOpen(clip);
        return decoder ? decoder->waitFrame(index, forward, targets) : nullptr;
    }
    std::lock_guard lock(mutex_);
    auto it = decoders_.find(clip);
    return it == decoders_.end() || !it->second ? nullptr : it->second->frame(index, forward, targets);
}

bool ClipDecoders::allOpen() const
{
    return std::all_of(wanted_.begin(), wanted_.end(),
                       [&](const ClipKey& clip) { return decoders_.count(clip) || missing_.count(clip); });
}

void ClipDecoders::waitIdle()
{
    std::vector<std::shared_ptr<VideoDecoder>> open;
    {
        std::unique_lock lock(mutex_);
        ++waiters_;
        changed_.notify_all();
        changed_.wait_for(lock, kOpenTimeout, [&] { return held_ || allOpen(); });
        --waiters_;
        for (const auto& [clip, decoder] : decoders_)
            if (decoder)
                open.push_back(decoder);
    }
    for (const auto& decoder : open)
        decoder->waitIdle();
}

bool ClipDecoders::settled()
{
    std::lock_guard lock(mutex_);
    return !held_ && allOpen() &&
           std::all_of(decoders_.begin(), decoders_.end(), [](const auto& d) { return !d.second || d.second->settled(); });
}

bool ClipDecoders::preloaded()
{
    std::lock_guard lock(mutex_);
    return !held_ && allOpen() && std::all_of(decoders_.begin(), decoders_.end(), [&](const auto& d) {
               return !d.second || playing_.count(d.first) || d.second->settled();
           });
}

void ClipDecoders::setLimits(size_t cacheBytes, size_t preloadBytes)
{
    std::lock_guard lock(mutex_);
    const bool lower = cacheBytes < cacheBytes_;
    cacheBytes_ = cacheBytes;
    preloadBytes_ = preloadBytes;
    if (lower)
        std::erase_if(decoders_, [&](auto& entry) {
            if (!entry.second || playing_.count(entry.first))
                return false;
            entry.second->idle();
            stale_.insert(entry.second.get());
            retired_.emplace_back(entry.first, std::move(entry.second));
            return true;
        });
    park();
    changed_.notify_all();
}

ClipDecoders::Use ClipDecoders::use()
{
    std::vector<std::pair<bool, std::shared_ptr<VideoDecoder>>> open;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [clip, decoder] : decoders_)
            if (decoder)
                open.emplace_back(playing_.count(clip) > 0, decoder);
    }
    Use use;
    for (const auto& [playing, decoder] : open)
        (playing ? use.playingBytes : use.preloadBytes) += decoder->heldBytes();
    {
        std::lock_guard lock(mutex_);
        open.clear();
    }
    return use;
}

std::map<std::string, size_t> ClipDecoders::sizes()
{
    std::vector<std::shared_ptr<VideoDecoder>> open;
    std::map<std::string, size_t> sizes;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [clip, decoder] : decoders_)
            if (decoder)
                open.push_back(decoder);
        // The hard bound: the clip cache bound for each clip that plays, the preload budget for the rest.
        const size_t cap = (playing_.size() * cacheBytes_ + preloadBytes_) >> 20;
        sizes = {{"cacheCapMB", cap},           {"decoders", decoders_.size()}, {"wanted", wanted_.size()},
                 {"retired", retired_.size()},
                 {"preloads", preloads_.size()}, {"missing", missing_.size()},   {"faults", faults_.size()}};
    }
    for (const auto& decoder : open) {
        const auto [frames, failures] = decoder->sizes();
        sizes["cachedFrames"] += frames;
        sizes["cachePlanMB"] += size_t(decoder->planBytes() >> 20);
        sizes["failedSpans"] += failures;
    }
    // Keep the last reference off this thread's hands only briefly: decoders are destroyed on the decoders' thread.
    {
        std::lock_guard lock(mutex_);
        open.clear();
    }
    return sizes;
}

void ClipDecoders::holdOpens(bool held)
{
    std::lock_guard lock(mutex_);
    held_ = held;
    changed_.notify_all();
}

void ClipDecoders::hurry(bool on)
{
    std::lock_guard lock(mutex_);
    if (hurried_ == on)
        return;
    hurried_ = on;
    for (const auto& [clip, decoder] : decoders_)
        if (decoder)
            decoder->hurry(on);
}

void ClipDecoders::injectFault(const ClipKey& clip, int index)
{
    std::lock_guard lock(mutex_);
    if (auto it = decoders_.find(clip); it != decoders_.end() && it->second)
        it->second->injectFault(index);
    else
        faults_[clip] = index;
}

void ClipDecoders::injectStall(const ClipKey& clip, int index, std::chrono::milliseconds duration)
{
    std::lock_guard lock(mutex_);
    if (auto it = decoders_.find(clip); it != decoders_.end() && it->second)
        it->second->injectStall(index, duration);
    else
        stalls_[clip] = {index, duration};
}

void ClipDecoders::onFailure(OnFailure callback)
{
    std::lock_guard lock(failureMutex_);
    onFailure_ = std::move(callback);
}

void ClipDecoders::report(const ClipKey& clip, const std::string& reason)
{
    std::fprintf(stderr, "clip %s on layer %llu: %s\n", clip.second.c_str(), (unsigned long long)clip.first,
                 reason.c_str());
    std::lock_guard lock(failureMutex_);
    if (onFailure_)
        onFailure_(clip, reason);
}

std::shared_ptr<VideoDecoder> ClipDecoders::waitOpen(const ClipKey& clip)
{
    std::unique_lock lock(mutex_);
    ++waiters_;
    changed_.notify_all();
    changed_.wait_for(lock, kOpenTimeout, [&] {
        return decoders_.count(clip) || missing_.count(clip) || !wanted_.count(clip) || held_;
    });
    --waiters_;
    auto it = decoders_.find(clip);
    return it == decoders_.end() ? nullptr : it->second;
}

// Decoders are destroyed on this thread only: destroying one joins its decode thread.
void ClipDecoders::run()
{
    while (round()) {
    }
}

// One round: close what is no longer wanted, open one clip. False once stopped. Its pool frees what AVFoundation
// autoreleases while opening and closing, which this long-lived thread would otherwise keep.
bool ClipDecoders::round()
{
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    const bool more = roundBody();
    pool->release();
    return more;
}

bool ClipDecoders::roundBody()
{
    std::shared_ptr<VideoDecoder> closing;
    std::optional<ClipKey> opening;
    {
        std::unique_lock lock(mutex_);
        const auto due = lastChange_ + kChangeGap;
        auto unopened = [&](const ClipKey& clip) { return !decoders_.count(clip) && !missing_.count(clip); };
        auto urgent = [&] { return !held_ && std::any_of(playing_.begin(), playing_.end(), unopened); };
        auto paced = [&] { return (!held_ && !allOpen()) || !retired_.empty(); };
        auto unpaced = [&] { return hurried_ || waiters_ > 0 || std::chrono::steady_clock::now() >= due; };
        auto pending = [&] {
            if (!retrying_.empty() || urgent() || retired_.size() > kMaxRetired)
                return true;
            for (const auto& [clip, decoder] : decoders_)
                if (!wanted_.count(clip))
                    return true;
            return paced() && unpaced();
        };
        auto deadline = std::chrono::steady_clock::now() + kMissingRetry;
        if (paced())
            deadline = std::min(deadline, due);
        changed_.wait_until(lock, deadline, [&] { return !running_ || pending(); });
        if (!running_)
            return false;
        if (std::chrono::steady_clock::now() - retried_ >= kMissingRetry && !missing_.empty()) {
            lock.unlock();
            retryMissing();
            return true;
        }
        retire();
        if (urgent()) {
            opening = *std::find_if(playing_.begin(), playing_.end(), unopened);
        } else if (unpaced()) {
            for (const Preload& p : preloads_)
                if (!held_ && unopened(p.clip)) {
                    opening = p.clip;
                    break;
                }
        }
        if (!opening && !retired_.empty() && (unpaced() || retired_.size() > kMaxRetired)) {
            closing = std::move(retired_.front().second);
            stale_.erase(closing.get());
            retired_.erase(retired_.begin());
        }
        if (!opening && !closing)
            return true;
    }
    if (closing) {
        AZL_ZONE("close clip");
        const auto started = std::chrono::steady_clock::now();
        closing.reset();
        stats_->closes++;
        raiseMax(stats_->maxCloseMicros, started);
        std::lock_guard lock(mutex_);
        lastChange_ = std::chrono::steady_clock::now();
        return true;
    }

    AZL_ZONE("open clip");
    const auto started = std::chrono::steady_clock::now();
    std::shared_ptr<VideoDecoder> decoder = open(*opening);
    stats_->opens++;
    raiseMax(stats_->maxOpenMicros, started);
    std::lock_guard lock(mutex_);
    if (auto fault = faults_.find(*opening); fault != faults_.end() && decoder) {
        decoder->injectFault(fault->second);
        faults_.erase(fault);
    }
    if (auto stall = stalls_.find(*opening); stall != stalls_.end() && decoder) {
        decoder->injectStall(stall->second.first, stall->second.second);
        stalls_.erase(stall);
    }
    if (decoder && hurried_)
        decoder->hurry(true);
    if (wanted_.count(*opening)) {
        decoders_[*opening] = std::move(decoder);
        park();
    }
    lastChange_ = std::chrono::steady_clock::now();
    changed_.notify_all();
    return true;
}

// Under the lock. Decoders no longer wanted stop decoding and drop their frames at once, but close one at a time, paced
// like opens; one wanted again before it closed comes back without opening its file again.
void ClipDecoders::retire()
{
    std::erase_if(decoders_, [&](auto& entry) {
        const bool retried = retrying_.count(entry.first.second) && entry.second && entry.second->broken();
        if (wanted_.count(entry.first) && !retried)
            return false;
        if (entry.second) {
            entry.second->idle();
            retired_.emplace_back(entry.first, std::move(entry.second));
        }
        return true;
    });
    retrying_.clear();
    bool revived = false;
    std::erase_if(retired_, [&](auto& entry) {
        if (!wanted_.count(entry.first) || decoders_.count(entry.first) || entry.second->broken() ||
            stale_.count(entry.second.get()))
            return false;
        decoders_[entry.first] = std::move(entry.second);
        revived = true;
        return true;
    });
    if (revived) {
        park();
        changed_.notify_all();
    }
}

// Every decoder of a missing file is closed before the file is looked for: while one holds the file of a removed drive,
// AVFoundation cannot open that path again once the drive is back. A file that is back is opened again by the next
// round wherever its clips are wanted.
void ClipDecoders::retryMissing()
{
    std::set<std::string> hashes;
    std::vector<std::shared_ptr<VideoDecoder>> closing;
    {
        std::lock_guard lock(mutex_);
        retried_ = std::chrono::steady_clock::now();
        for (const ClipKey& clip : missing_)
            hashes.insert(clip.second);
        for (auto& [clip, decoder] : decoders_)
            if (decoder && hashes.count(clip.second))
                closing.push_back(std::exchange(decoder, nullptr));
        std::erase_if(retired_, [&](auto& entry) {
            if (!hashes.count(entry.first.second))
                return false;
            closing.push_back(std::move(entry.second));
            return true;
        });
    }
    closing.clear();
    for (const std::string& hash : hashes) {
        const std::optional<MediaEntry> entry = library_->find(hash);
        if (!entry || entry->status != MediaStatus::Ok)
            continue;
        std::fprintf(stderr, "clip %s: the file is back at %s\n", hash.c_str(), entry->path.c_str());
        std::lock_guard lock(mutex_);
        std::erase_if(missing_, [&](const ClipKey& clip) { return clip.second == hash; });
        std::erase_if(decoders_, [&](const auto& d) { return d.first.second == hash && !d.second; });
        changed_.notify_all();
    }
}

std::shared_ptr<VideoDecoder> ClipDecoders::open(const ClipKey& clip)
{
    const std::string& hash = clip.second;
    if (!library_)
        return nullptr;
    const std::optional<MediaEntry> entry = library_->find(hash);
    if (!entry || entry->status != MediaStatus::Ok) {
        if (entry && entry->status == MediaStatus::Missing) {
            std::lock_guard lock(mutex_);
            missing_.insert(clip);
        }
        report(clip, entry ? entry->reason : "not in the media library");
        return nullptr;
    }
    if (crash::skipped("clip", hash)) {
        report(clip, "skipped: opening it crashed the engine before");
        return nullptr;
    }
    crash::Scope opening("clip", hash, entry->path.filename().string());
    try {
        auto broken = [this, clip, path = entry->path](const std::string& reason) {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec)) {
                library_->flag(clip.second, MediaStatus::Missing, reason);
                {
                    std::lock_guard lock(mutex_);
                    missing_.insert(clip);
                }
                return report(clip, "gone, flagged missing: " + reason);
            }
            library_->flag(clip.second, MediaStatus::Corrupt, reason);
            report(clip, "broken, flagged corrupt: " + reason);
        };
        return std::make_shared<VideoDecoder>(&device_, entry->path.string(),
                                              VideoDecoder::Options{cacheBytes_, stats_, hash, broken, frameCost_});
    } catch (const std::exception& e) {
        library_->flag(hash, MediaStatus::Corrupt, e.what());
        report(clip, e.what());
        return nullptr;
    }
}

} // namespace azl
