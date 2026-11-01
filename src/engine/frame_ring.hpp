#pragma once

#include "gpu/gpu.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <tuple>
#include <utility>
#include <vector>

namespace azl {

inline constexpr gpu::Format kFrameFormat = gpu::Format::BGRA8;

// A finished picture. Whoever holds a FrameRef keeps the frame and everything it depends on alive.
struct Frame {
    // The composition.
    gpu::Texture texture;
    uint64_t index = 0;
    // When its render began.
    std::chrono::steady_clock::time_point started;
    // The layer and group pictures some slice reads, by layer id: premultiplied, before opacity and blending.
    std::vector<std::pair<uint64_t, gpu::Texture>> pictures;

    // The composition for 0, otherwise that layer's picture; null when the frame has none.
    const gpu::Texture* picture(uint64_t input) const;
};
using FrameRef = std::shared_ptr<const Frame>;

// The frame textures the render thread renders into and outputs read from. A frame's textures go back to the ring
// only when its last holder lets go, so a frame is never overwritten while anyone still reads it. Composition
// textures are a fixed set; picture textures are made on first need and reused after.
class FrameRing : public std::enable_shared_from_this<FrameRing> {
public:
    FrameRing(const gpu::Device& device, uint32_t width, uint32_t height, int count);

    // Null when every frame is still held.
    std::shared_ptr<Frame> claim();
    gpu::Texture claimPicture();
    void publish(FrameRef frame);
    FrameRef latest() const;
    // Free frames, free pictures, published frames.
    std::tuple<size_t, size_t, size_t> sizes() const;
    // The frame an output shows after the one with index `shown`, among those whose render began before `time`: the
    // next one in order, so an output at the render rate that fell one frame behind stays there instead of skipping,
    // else the newest such, else the oldest kept. An output asks with its refresh's vsync, so how late its callback
    // runs never changes the frame it shows.
    FrameRef next(uint64_t shown, std::chrono::steady_clock::time_point time) const;

private:
    void giveBack(Frame& frame);
    gpu::Texture newTexture() const;

    gpu::Device device_;
    uint32_t width_;
    uint32_t height_;
    mutable std::mutex mutex_;
    std::vector<gpu::Texture> free_;
    std::vector<gpu::Texture> freePictures_;
    // The newest published frames, oldest first: enough for an output one frame behind while the newest is too young.
    std::deque<FrameRef> published_;
};

} // namespace azl
