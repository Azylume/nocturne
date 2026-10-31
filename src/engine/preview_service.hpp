#pragma once

#include "engine/clip_effects_preview.hpp"
#include "engine/engine.hpp"
#include "engine/screen_painter.hpp"
#include "media/media_library.hpp"
#include "protocol/document_server.hpp"

#ifdef __APPLE__
#include <IOSurface/IOSurfaceRef.h>
#else
#include "platform/preview_channel.hpp"
#endif

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace azl {

// Serves small live pictures of the composition, layers and groups, screens with their mapping applied, and clips, as
// "preview" messages, for the UI's monitors and clip cells. Previews are painted from the newest finished frame on
// their own thread and command queue, never on the render thread. A client has one watch per name (the empty name
// by default); a new one replaces the watch of its name.
//
//   {"type": "preview", "op": "watch", "watch": "clips", "sources": ["composition", "layer:3", "screen:2", "clip:H"],
//    "via": "surface" | "jpeg", "width": 320, "height": 180, "fps": 30}
//
// Via "surface" (a UI on the same machine) each source is painted into a ring of shared pictures, announced once, and
// every new picture names the surface it is in. On Apple they are global IOSurfaces the GPU paints into, so nothing is
// copied or encoded on the way; elsewhere they are PreviewChannelWriters, filled from a GPU readback:
//     -> {"type": "preview", "op": "surfaces", "source": S, "width", "height", "surfaces": [id, id, id]}
//     -> {"type": "preview", "op": "frame", "source": S, "frame": index, "surface": i}
// The ring comes round again while a late reader still has to read a surface, so each surface also names the frame
// it holds, -1 while it is painted (previewFrameKey).
// Via "jpeg" (a remote UI) every new picture travels as a JPEG:
//     -> {"type": "preview", "op": "frame", "source": S, "frame": index, "jpeg": <binary>}
//
// An empty source list ends the watch, as does the client going away. A watched layer publishes its picture with
// every frame (Engine::setPreviewPictures), which costs one full-resolution pass per layer.
//
// "clip:H" shows the media of hash H moving, whether or not a clip of it plays; the render thread never sees it. Its
// picture moves on to the next of kClipPictures pictures spread over the file at every tick of the watch, looping.
// They are sampled once per hash and size at keyframes (VideoSampler) on one background thread (ThreadRole::Background) with its own
// reader, which works at most kClipBusyShare of the time, and kept while some watch shows them (kKeptClips unwatched
// ones stay for a selection back). A watch serves kMaxClips clip source, the first it names (the UI's selected clip);
// a hash with no video shows nothing.
// A watch may name the cell its clip source shows, `"clip": {"layer": L, "column": C}`; while that cell holds a clip of
// the hash with effects, each picture is drawn through the clip's own chain (ClipEffectsPreview, on this service's
// thread and queue) once that chain is compiled, and shows the bare media until then.
// Clip sources are off unless turned on (setClipPreviews, or live by any client), and a UI then keeps showing stills:
//   {"type": "preview", "op": "clips", "on": true | false}  turns them on or off and broadcasts the state
//   {"type": "preview", "op": "clips"}                      asks for the state
//     -> {"type": "preview", "op": "clips", "on": B}
// Off ends their sampling and effect drawing and frees their pictures and textures; a watch keeps naming its clip
// source and is served again once they are on. The state is machine-local, not part of the document.
class PreviewService {
public:
    static constexpr int kClipPictures = 12;
    static constexpr size_t kMaxClips = 1;
    static constexpr size_t kKeptClips = 8;
    static constexpr double kClipBusyShare = 0.25;

    // Clip pictures sampled so far, and the time that took.
    struct ClipStats {
        std::atomic<uint64_t> pictures{0};
        std::atomic<uint64_t> busyMicros{0};
    };
    // Pictures painted from the newest frame, and their GPU time on this service's queue.
    struct PaintStats {
        std::atomic<uint64_t> pictures{0};
        std::atomic<uint64_t> gpuMicros{0};
    };
    struct Cell {
        uint64_t layer = 0;
        uint64_t column = 0;
    };

    PreviewService(DocumentServer& document, Engine& engine);
    ~PreviewService();

    // Names the library clip media is found in. Call it before clients connect; clip sources stay off until
    // setClipPreviews, since they cost keyframe decodes and GPU time beside the show's (--clip-previews).
    void setClipLibrary(MediaLibrary* library);
    // Without a library clip sources stay off.
    void setClipPreviews(bool on);
    bool clipPreviews() const { return clipsOn_; }
    // How many unwatched clips keep their pictures for a selection back (default kKeptClips); fewer drops the oldest
    // at once.
    void setKeptClips(size_t kept);
    // Clips with pictures kept and not watched now, and the bytes all clip pictures take.
    std::pair<size_t, size_t> keptClipUse();

    const ClipStats& clipStats() const { return clipStats_; }
    const PaintStats& paintStats() const { return paintStats_; }
    // How many watches show each kind of source ("composition", "layer", "screen", "clip"); kinds none shows are left
    // out.
    std::map<std::string, size_t> watchers();
    // Watches, their targets, IOSurfaces and textures, clip strips with their pictures and JPEGs, and the clip
    // effects' sizes; byte sizes in KB.
    std::map<std::string, size_t> sizes();
    // Null until setClipLibrary.
    ClipEffectsPreview* clipEffects() { return effects_.get(); }

private:
    struct Target {
        std::string source;
        std::vector<gpu::Texture> textures;
        // The JPEG path's picture as the CPU reads it.
        gpu::Readback readback;
#ifdef __APPLE__
        std::vector<std::shared_ptr<std::remove_pointer_t<IOSurfaceRef>>> surfaces;
#else
        std::vector<std::shared_ptr<PreviewChannelWriter>> surfaces;
#endif
        size_t next = 0;
        bool announced = false;
        // A clip source: its strip, and how many pictures it has sent.
        std::string strip;
        uint64_t sent = 0;
        std::optional<Cell> cell;
    };
    struct Watch {
        bool surface = false;
        uint32_t width = 320;
        uint32_t height = 180;
        double interval = 1.0 / 30.0;
        std::chrono::steady_clock::time_point due{};
        uint64_t frame = UINT64_MAX;
        std::vector<Target> targets;
        // Kept while clip sources are off, so they come back when turned on.
        std::vector<std::string> clipSources;
        std::optional<Cell> cell;
    };
    // A clip's pictures in file order, as far as sampled.
    struct Strip {
        std::string hash;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<std::vector<uint8_t>> pictures;
        std::vector<std::vector<uint8_t>> jpegs;
        int tried = 0;
        bool failed = false;
        bool watched = false;
        uint64_t lastWatched = 0;
    };
    using WatchKey = std::pair<ClientId, std::string>;

    void handle(const Message& message, const DocumentServer::Sink& reply);
    void run();
    // False once the client is gone. `frame` is null when the watch has painted the newest frame already.
    bool serve(ClientId client, Watch& watch, const FrameRef& frame,
               const std::shared_ptr<const Composition>& composition);
    bool serveClip(ClientId client, Watch& watch, Target& target, const std::shared_ptr<const Composition>& composition);
    // Draws the target cell's clip effects over strip picture `index` into `picture`; false while the strip's own
    // picture is what shows.
    bool drawEffects(const Target& target, const Strip& strip, size_t index,
                     const std::shared_ptr<const Composition>& composition, std::vector<uint8_t>& picture);
    Target makeTarget(const std::string& source, const Watch& watch) const;
    void addSurfaceTextures(Target& target, const Watch& watch) const;
    void addClipTargets(Watch& watch) const;
    Message clipsState() const;
    void updatePictures();
    void updateStrips();
    void sample();
    void dropClient(ClientId client);

    DocumentServer& document_;
    Engine& engine_;
    std::atomic<MediaLibrary*> library_{nullptr};
    std::atomic<bool> clipsOn_{false};
    gpu::Queue queue_;
    profile::GpuTimeline gpu_;
    ScreenPainter painter_;
    std::unique_ptr<ClipEffectsPreview> effects_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable sampleWake_;
    std::map<WatchKey, Watch> watches_;
    std::map<std::string, Strip> strips_;
    size_t keptClips_ = kKeptClips;
    uint64_t watchGeneration_ = 0;
    ClipStats clipStats_;
    PaintStats paintStats_;
    bool stopping_ = false;
    std::thread thread_;
    std::thread sampler_;
};

// The hash of a "clip:H" source, if it is one.
std::optional<std::string> clipHash(const std::string& source);

// Parses "composition", "layer:N", "screen:N" into the Screen a preview paints; false for an unknown source.
bool previewScreen(const std::string& source, const Composition& composition, Screen& screen);

} // namespace azl
