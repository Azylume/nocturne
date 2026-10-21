#pragma once

#include "engine/engine.hpp"
#include "engine/screen_painter.hpp"
#include "output/h264_stream.hpp"
#include "output/return_video.hpp"
#include "protocol/document_server.hpp"

#include <IOSurface/IOSurfaceRef.h>
#include <Metal/Metal.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace azl {

// Serves the return video ("video" messages, return_video.hpp): every output, a Screen bound to a display, streamed at
// return_video::kHeight lines as H.264 to the clients that watch.
//
// Only while a client watches: with none, its thread sleeps and the encoders are closed. Pictures are painted from the
// newest finished frame on this service's own thread and command queue, the way an output paints its screen (test
// card included, no guides or identify overlays), straight into the encoder's buffers; the render thread and the
// present threads never wait for it. A slow viewer drops frames on its link (return_video::Gate), never here.
class ReturnVideoService {
public:
    struct Stats {
        std::atomic<uint64_t> pictures{0};
        // Encoded bytes sent, counted once per watcher.
        std::atomic<uint64_t> bytes{0};
        std::atomic<uint64_t> gpuMicros{0};
        std::atomic<uint64_t> encodeMicros{0};
    };

    ReturnVideoService(DocumentServer& document, Engine& engine);
    ~ReturnVideoService();

    size_t watchers() const;
    const Stats& stats() const { return stats_; }

private:
    struct Output {
        std::string display;
        uint64_t screen = 0;
        std::unique_ptr<H264Encoder> encoder;
        uint64_t frame = 0;
    };
    struct Watcher {
        double fps = return_video::kFps;
    };

    void handle(const Message& message, const DocumentServer::Sink& reply);
    void run();
    // Service thread: true when the outputs changed.
    bool follow(const Composition& composition, double fps);
    // The watchers that are gone.
    std::set<ClientId> serve(const FrameRef& frame, const Composition& composition, bool key,
                             const std::vector<ClientId>& watchers);
    MTL::Texture* textureOf(CVPixelBufferRef buffer);
    Message watchMessage(bool on) const;
    double fpsLocked() const;

    DocumentServer& document_;
    Engine& engine_;
    NS::SharedPtr<MTL::CommandQueue> queue_;
    ScreenPainter painter_;
    Stats stats_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::map<ClientId, Watcher> watchers_;
    bool newWatcher_ = false;
    // The watch message the watchers have, to announce changes of the outputs.
    Message announced_;
    bool stopping_ = false;
    std::optional<bool> hardware_;

    // Service thread only.
    return_video::Plan plan_;
    std::vector<Output> outputs_;
    std::map<IOSurfaceRef, NS::SharedPtr<MTL::Texture>> textures_;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    std::thread thread_;
};

} // namespace azl
