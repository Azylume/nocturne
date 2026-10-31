#include "harness/fake_displays.hpp"

#include "engine/frame_trace.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <functional>
#include <optional>
#include <random>
#include <thread>
#include <utility>

namespace azl {

namespace {

using Clock = std::chrono::steady_clock;

// A capture the test waits for; shared with completion handlers, which can outlive the surface.
struct Capture {
    std::mutex mutex;
    std::condition_variable done;
    bool wanted = false;
    std::optional<Image> image;
};

Image toRgba(const uint8_t* bgra, uint32_t width, uint32_t height)
{
    Image image{width, height, std::vector<uint8_t>(size_t(width) * height * 4)};
    for (size_t i = 0; i < image.rgba.size(); i += 4) {
        image.rgba[i] = bgra[i + 2];
        image.rgba[i + 1] = bgra[i + 1];
        image.rgba[i + 2] = bgra[i];
        image.rgba[i + 3] = bgra[i + 3];
    }
    return image;
}

} // namespace

class FakeSurface : public Surface {
public:
    FakeSurface(FakeDisplays& owner, const gpu::Device& device, const DisplayInfo& display)
        : owner_(owner), id_(display.id), refreshHz_(display.refreshHz), lateness_(owner.lateness_),
          random_(uint32_t(std::hash<std::string>{}(display.id))), device_(device),
          target_(device.makeTexture({.format = kSurfaceFormat,
                                      .width = display.width,
                                      .height = display.height,
                                      .usage = gpu::Usage::RenderTarget | gpu::Usage::Sampled}))
    {
        owner_.open_[id_] = this;
    }

    ~FakeSurface() override { owner_.open_.erase(id_); }

    double refreshHz() const override { return refreshHz_; }

    // A late refresh re-anchors the schedule, as a display link skips refreshes it missed.
    void runRefreshes(const std::atomic<bool>& running, const Refresh& onRefresh) override
    {
        const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / refreshHz_));
        auto next = Clock::now();
        while (running) {
            next += period;
            if (Clock::now() > next)
                next = Clock::now();
            std::this_thread::sleep_until(next + late());
            if (running) {
                refresh_ = next;
                onRefresh(next);
            }
        }
    }

    const gpu::Texture* nextTarget() override { return &target_; }

    // Shown as a display would: at the first refresh after its GPU work ends, the next one at the earliest, not when
    // the completion handler happens to run.
    void present(gpu::CommandBuffer& cmd, Shown shown) override
    {
        if (wanted())
            encodeCapture(cmd);
        const double refresh = traceSeconds(refresh_), period = 1.0 / refreshHz_;
        cmd.onCompleted([shown = std::move(shown), refresh, period](const gpu::Completion& done) {
            const double end = traceFromHost(done.gpuEnd);
            shown(refresh + period * std::max(1.0, std::ceil((end - refresh) / period)));
        });
    }

    Image capture()
    {
        std::unique_lock lock(capture_->mutex);
        capture_->image.reset();
        capture_->wanted = true;
        capture_->done.wait_for(lock, std::chrono::seconds(2), [&] { return capture_->image.has_value(); });
        capture_->wanted = false;
        return capture_->image.value_or(Image{});
    }

private:
    Clock::duration late()
    {
        const auto [min, max] = lateness_;
        return std::chrono::microseconds(std::uniform_int_distribution<int64_t>(min.count(), max.count())(random_));
    }

    bool wanted()
    {
        std::lock_guard lock(capture_->mutex);
        return std::exchange(capture_->wanted, false);
    }

    void encodeCapture(gpu::CommandBuffer& cmd)
    {
        const gpu::Readback readback(device_, target_);
        readback.encode(cmd);
        cmd.onCompleted([capture = capture_, readback, width = target_.width(),
                         height = target_.height()](const gpu::Completion&) {
            Image image = toRgba(readback.data(), width, height);
            std::lock_guard lock(capture->mutex);
            capture->image = std::move(image);
            capture->done.notify_all();
        });
    }

    FakeDisplays& owner_;
    const std::string id_;
    const double refreshHz_;
    const std::pair<std::chrono::microseconds, std::chrono::microseconds> lateness_;
    std::minstd_rand random_;
    Clock::time_point refresh_;
    gpu::Device device_;
    gpu::Texture target_;
    std::shared_ptr<Capture> capture_ = std::make_shared<Capture>();
};

void FakeDisplays::attach(DisplayInfo display)
{
    attached_.push_back(std::move(display));
    changed();
}

void FakeDisplays::mirror(const std::string& id, const std::string& of)
{
    for (DisplayInfo& d : attached_)
        if (d.id == id)
            d.mirrorOf = of;
    changed();
}

void FakeDisplays::replace(const std::string& id, DisplayInfo display)
{
    std::erase_if(attached_, [&](const DisplayInfo& d) { return d.id == id; });
    attached_.push_back(std::move(display));
    changed();
}

bool FakeDisplays::unmirror(const std::vector<std::string>& ids)
{
    unmirrored_.push_back(ids);
    for (DisplayInfo& d : attached_)
        if (std::find(ids.begin(), ids.end(), d.id) != ids.end())
            d.mirrorOf.clear();
    changed();
    return true;
}

void FakeDisplays::detach(const std::string& id)
{
    std::erase_if(attached_, [&](const DisplayInfo& d) { return d.id == id; });
    changed();
}

std::unique_ptr<Surface> FakeDisplays::open(const std::string& id, const gpu::Device& device)
{
    auto it = std::find_if(attached_.begin(), attached_.end(), [&](const DisplayInfo& d) { return d.id == id; });
    if (it == attached_.end() || !it->mirrorOf.empty())
        return nullptr;
    return std::make_unique<FakeSurface>(*this, device, *it);
}

Image FakeDisplays::capture(const std::string& id)
{
    auto it = open_.find(id);
    return it == open_.end() ? Image{} : it->second->capture();
}

} // namespace azl
