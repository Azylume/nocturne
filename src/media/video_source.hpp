#pragma once

#include "media/hap.hpp"
#include "media/media_library.hpp"
#include "media/video_decoder.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// What VideoDecoder needs from a platform's demuxer and decoder (AVFoundation on Apple, FFmpeg elsewhere). The
// decoder's cache, read-ahead, seeking and failure handling are the same on every platform; only this differs.
namespace azl {

struct MediaError : std::runtime_error {
    MediaError(MediaStatus s, const std::string& reason) : std::runtime_error(reason), status(s) {}
    MediaStatus status;
};

struct SourceTrack {
    double fps = 0.0;
    int frameCount = 0;
    int width = 0;
    int height = 0;
    // Clockwise quarter turns that show the picture upright.
    int turns = 0;
    // Read as stored, without a decoder, when the track is HAP.
    HapCodec hap = HapCodec::None;
    // The frames of the sync samples, sorted; always starts with frame 0.
    std::vector<int> keyframes;
    // What one decoded picture of a track that is not HAP holds in the frame cache.
    int64_t pictureBytes = 0;
};

// One read: the frame index of its presentation time, and its decoded picture (opaque to the decoder; null when the
// sample had none) or a HAP track's sample as stored, which `owner` keeps alive.
struct SourceSample {
    int index = -1;
    std::shared_ptr<const void> picture;
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    std::shared_ptr<const void> owner;
};

// A sample, or none at the end of the stream (also when reading failed: failure() says).
using SourceRead = std::optional<SourceSample>;

// Reads a track forward from where it started. read() may block for good: the decoder calls it on a worker thread and
// may stop waiting for it. One read at a time, and cancel() is never called while a read runs.
class SourceReader {
public:
    virtual ~SourceReader() = default;
    virtual SourceRead read() = 0;
    virtual void cancel() = 0;
    // Why the stream ended early; none when it was read to its end.
    virtual std::optional<MediaError> failure() const = 0;
};

// An open video track. Decode thread only, apart from what readers do on their workers.
class VideoSource {
public:
    virtual ~VideoSource() = default;
    virtual const SourceTrack& track() const = 0;
    // A reader whose first sample is keyframe `fromFrame`. Throws MediaError.
    virtual std::shared_ptr<SourceReader> startReading(int fromFrame) = 0;
    // The frame showing a decoded picture; null when it cannot be made.
    virtual VideoFrameRef frame(const std::shared_ptr<const void>& picture, int index) = 0;
    // The frame of a HAP sample's textures; null when they cannot be made.
    virtual VideoFrameRef hapFrame(const std::vector<HapImage>& images, PictureFormat format, int index) = 0;
};

// Opens the file's first video track. Blocking; throws MediaError.
std::unique_ptr<VideoSource> openVideoSource(MediaDevice* device, const std::string& path);

// Runs a read on a worker thread and returns at once. A read that blocks for good keeps only its own worker.
void runRead(std::function<void()> read);

// The calling thread decodes from now on (its priority).
void becomeDecodeThread();

// While alive, what the platform's frameworks hand out to be freed later is freed when it ends (an autorelease pool on
// Apple; nothing elsewhere). Decoders run on long-lived threads, where that would otherwise pile up for the whole show.
class FrameworkScope {
public:
    FrameworkScope();
    ~FrameworkScope();
    FrameworkScope(const FrameworkScope&) = delete;
    FrameworkScope& operator=(const FrameworkScope&) = delete;

private:
    [[maybe_unused]] void* pool_ = nullptr;
};

} // namespace azl
