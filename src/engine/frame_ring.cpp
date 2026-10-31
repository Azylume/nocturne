#include "engine/frame_ring.hpp"

#include <utility>

namespace azl {

namespace {

constexpr size_t kKept = 3;

} // namespace

const gpu::Texture* Frame::picture(uint64_t input) const
{
    if (input == 0)
        return &texture;
    for (const auto& [id, picture] : pictures)
        if (id == input)
            return &picture;
    return nullptr;
}

FrameRing::FrameRing(const gpu::Device& device, uint32_t width, uint32_t height, int count)
    : device_(device), width_(width), height_(height)
{
    for (int i = 0; i < count; ++i)
        free_.push_back(newTexture());
}

gpu::Texture FrameRing::newTexture() const
{
    return device_.makeTexture({.format = kFrameFormat,
                                .width = width_,
                                .height = height_,
                                .usage = gpu::Usage::RenderTarget | gpu::Usage::Sampled});
}

std::shared_ptr<Frame> FrameRing::claim()
{
    std::lock_guard lock(mutex_);
    if (free_.empty())
        return nullptr;
    auto* frame = new Frame{.texture = std::move(free_.back())};
    free_.pop_back();
    return std::shared_ptr<Frame>(frame, [ring = weak_from_this()](Frame* f) {
        if (auto owner = ring.lock())
            owner->giveBack(*f);
        delete f;
    });
}

gpu::Texture FrameRing::claimPicture()
{
    {
        std::lock_guard lock(mutex_);
        if (!freePictures_.empty()) {
            auto picture = std::move(freePictures_.back());
            freePictures_.pop_back();
            return picture;
        }
    }
    return newTexture();
}

void FrameRing::publish(FrameRef frame)
{
    // Declared outside the lock: the dropped frame's deleter takes the lock again.
    FrameRef dropped;
    std::lock_guard lock(mutex_);
    published_.push_back(std::move(frame));
    if (published_.size() > kKept) {
        dropped = std::move(published_.front());
        published_.pop_front();
    }
}

std::tuple<size_t, size_t, size_t> FrameRing::sizes() const
{
    std::lock_guard lock(mutex_);
    return {free_.size(), freePictures_.size(), published_.size()};
}

FrameRef FrameRing::latest() const
{
    std::lock_guard lock(mutex_);
    return published_.empty() ? nullptr : published_.back();
}

FrameRef FrameRing::next(uint64_t shown, std::chrono::steady_clock::time_point time) const
{
    std::lock_guard lock(mutex_);
    FrameRef newest;
    for (const FrameRef& frame : published_) {
        if (frame->started >= time)
            break;
        if (frame->index == shown + 1)
            return frame;
        newest = frame;
    }
    if (newest || published_.empty())
        return newest;
    return published_.front();
}

void FrameRing::giveBack(Frame& frame)
{
    std::lock_guard lock(mutex_);
    free_.push_back(std::move(frame.texture));
    for (auto& [id, picture] : frame.pictures)
        freePictures_.push_back(std::move(picture));
}

} // namespace azl
