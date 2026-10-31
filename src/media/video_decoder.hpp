#pragma once

#include "gpu/gpu.hpp"
#include "media/decoded_picture.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace azl {

// The GPU decoders and text make their textures on. Without one (probing a file, tests that read pictures on the CPU)
// frames carry no textures: on Apple a source makes none, elsewhere a frame keeps its picture in system memory.
using MediaDevice = gpu::Device;

// How a video picture's two textures make its colour: Y'CbCr planes, or a HAP frame's compressed textures (the colour
// in `luma`; for YCoCgAlpha the alpha in `chroma`, else `chroma` repeats `luma`).
enum class PictureFormat : uint32_t { YCbCr, Rgb, Rgba, YCoCg, YCoCgAlpha };

// One decoded picture as two textures: luma (R8, R16) and interleaved chroma (RG8, RG16) at half resolution, or a HAP
// frame's textures (`format`). On Apple they are zero-copy views of the decoder's IOSurface; elsewhere the decode
// thread uploads the picture. `resources` keeps what the textures were made from alive (the pixel buffer and the
// producer's texture cache), so a frame stays valid on the GPU after its decoder is gone, and is never null for a frame
// with textures: it names the picture. Off Apple a frame decoded without a device carries its picture in system memory
// instead.
struct VideoFrame {
    int index = 0;
    gpu::Texture luma;
    gpu::Texture chroma;
#ifndef __APPLE__
    std::shared_ptr<const DecodedPicture> picture;
#endif
    // Rows of the affine Y'CbCr -> R'G'B' transform for the stream's matrix and range: rgb[i] = dot(row.xyz, yuv) + w.
    std::array<float, 12> yuvToRgb{};
    // Clockwise quarter turns that show the picture upright, from the track's rotation metadata (phone videos).
    int turns = 0;
    PictureFormat format = PictureFormat::YCbCr;
    std::shared_ptr<void> resources;
    // Set for a text clip's picture instead: `luma` (RG8) holds the fill's coverage in R and the outline's in G,
    // `bounds` places it (x, y, width, height in pixels from the text's anchor) for text of `size`; chroma is null.
    struct Text {
        std::array<float, 4> bounds{};
        float size = 0.0f;
    };
    std::optional<Text> text{};
    // A generator clip's picture: no textures, its chain's generator draws it.
    bool generator = false;

    // The pass header's `turns`: the quarter turns in bits 0-1, the format above them.
    uint32_t headerTurns() const { return uint32_t(turns) | uint32_t(format) << 2; }
};
using VideoFrameRef = std::shared_ptr<const VideoFrame>;

// Readers alive in the process (AVAssetReaders on Apple), readers that probe files included. Every closed decoder must
// free its readers, or what the frameworks keep for them piles up over a long show.
int64_t liveAssetReaders();

struct ClipLength {
    int frames = 0;
    double fps = 0.0;

    bool operator==(const ClipLength&) const = default;
};

// A frame to have decoded before the playhead gets there, e.g. the next Random Jump target, with the frames that
// follow it in its play direction.
struct FrameTarget {
    int index = 0;
    bool forward = true;

    bool operator==(const FrameTarget&) const = default;
};

// Totals over every decoder, for the stats line.
struct DecodeStats {
    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> busyMicros{0};
    std::atomic<uint64_t> seeks{0};
    std::atomic<uint64_t> errors{0};
    // Readers given up on because a read outlasted its deadline.
    std::atomic<uint64_t> stalls{0};
    // The slowest single read since the stats line last took it.
    mutable std::atomic<uint64_t> maxReadMicros{0};
    std::atomic<int64_t> cachedBytes{0};
    // Decoders opened and closed, and the slowest open and close since the stats line last took them.
    std::atomic<uint64_t> opens{0};
    std::atomic<uint64_t> closes{0};
    mutable std::atomic<uint64_t> maxOpenMicros{0};
    mutable std::atomic<uint64_t> maxCloseMicros{0};
};

// Decodes one video file around a playhead into a bounded frame cache. One background thread reads with VideoToolbox
// (through AVAssetReader; a HAP track's frames are only Snappy-decompressed and uploaded as compressed textures), or
// with FFmpeg off Apple, and owns up to two readers, so one can follow the playhead while the other waits parked at a
// prefetched target.
//
// Playing forward it decodes one second ahead and keeps a few frames behind. Playing backward it works in units of a
// GOP (keyframe to keyframe; short GOPs merged up to a second, GOPs over half the cache bound split): it holds the
// playhead's unit up to the playhead and decodes the unit below it whole in one forward pass while the playhead plays
// down through the current one, so each frame is decoded once. Each prefetch target gets one second of frames in its
// direction, kept whatever the playhead does.
//
// A decode error marks the frames from the failing one to the next keyframe as missing, and decoding goes on from that
// keyframe. They are retried once the playhead has passed them; after kMaxFailures errors in a row with no frame
// decoded in between, the file counts as broken and `onFailure` runs.
//
// The frame count is the number of frames the file actually delivers: a stream that ends early shortens it, so the
// last frame is always one that exists.
//
// A read can block for good (AVFoundation, seen on the Mac mini: copyNextSampleBuffer waiting inside MediaToolbox).
// Each read runs on a worker thread and the decode thread waits for it up to a deadline. A reader whose read outlasts
// it is given up on (the read cancels it if it ever returns), and decoding goes on with a new reader; kMaxStalls such
// reads in a row with no frame in between break the file. Closing stops waiting for a read at once, and waits for the
// decode thread only up to kCloseTimeout.
class VideoDecoder {
public:
    using OnFailure = std::function<void(const std::string& reason)>;

    static constexpr int kMaxFailures = 3;
    static constexpr int kMaxStalls = 3;
    static constexpr std::chrono::milliseconds kCloseTimeout{500};

    struct Options {
        size_t cacheBytes = size_t(2) << 30;
        std::shared_ptr<DecodeStats> stats = std::make_shared<DecodeStats>();
        // Names the clip in log lines.
        std::string name;
        // Runs on the decode thread once the file counts as broken.
        OnFailure onFailure;
        // Harness: every frame read takes this much longer, as on a slower or busier hardware decoder.
        std::chrono::microseconds frameCost{0};
        // A read that takes longer than this is given up on.
        std::chrono::milliseconds readDeadline{500};
    };

    // Opens the file (blocking: call it off the render thread). Throws std::runtime_error when it cannot be played.
    VideoDecoder(const MediaDevice* device, const std::string& path, Options options);
    ~VideoDecoder();

    ClipLength length() const;
    // Moves the playhead to frame `index` (clamped to the clip), sets the prefetch targets and returns the frame if it
    // is decoded, else null. Never blocks. A new decoder decodes nothing until the first call of frame() or park().
    VideoFrameRef frame(int index, bool forward, const std::vector<FrameTarget>& targets = {});
    // Prepares a start at frame `index` in that direction without playing: decodes only what starting there needs, a
    // few frames forward, or backward the unit up to it, and drops the rest. Never blocks.
    void park(int index, bool forward);
    // The bytes park() would hold.
    int64_t parkBytes(int index, bool forward) const;
    // Decodes nothing and drops its frames until frame() or park() is called. Never blocks.
    void idle();
    // Like frame(), but waits until that frame is decoded; null when it cannot be decoded or took too long.
    VideoFrameRef waitFrame(int index, bool forward, const std::vector<FrameTarget>& targets = {});
    // Waits until everything the cache wants for the current playhead and targets is decoded (deterministic harness).
    void waitIdle();
    // Everything the cache wants for the current playhead and targets is decoded. Never blocks.
    bool settled() const;
    // While on, nothing is decoded at a pace, as when nothing is on screen to be delayed by a burst of decoding.
    void hurry(bool on);
    // Fault injection for tests: decoding fails once when a reader reaches frame `index`.
    void injectFault(int index);
    // Fault injection for tests: one read by a reader reaching frame `index` blocks for `duration` first.
    void injectStall(int index, std::chrono::milliseconds duration);
    // The file counted as broken; the decoder delivers nothing more.
    bool broken() const;
    // Frames held in the cache, and failed spans remembered.
    std::pair<size_t, size_t> sizes() const;
    // What the current plan keeps (the playhead's second ahead and frames behind, or the two reverse units within the
    // cache bound, or the parked start, plus a second per prefetch target). Frames of an earlier plan stay until the
    // decode thread evicts them before its next frame, so the cache can briefly hold more.
    int64_t planBytes() const;
    // The frames the cache holds now.
    int64_t heldBytes() const;

    struct Impl;

private:
    void open(const MediaDevice* device, const std::string& path, Options options);

    std::shared_ptr<Impl> impl_;
};

} // namespace azl
