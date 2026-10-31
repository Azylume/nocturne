#include "output/display_output.hpp"

#include "engine/thread_priority.hpp"
#include "platform/profile.hpp"

#include <algorithm>
#include <chrono>

namespace azl {

namespace {

// `started` is when the frame's render began, in seconds, or negative for a repeated frame.
void recordShown(OutputStats& stats, double presentedTime, double started)
{
    if (presentedTime <= 0.0)
        return;
    stats.shown++;
    if (stats.lastShown > 0.0) {
        const auto micros = uint64_t((presentedTime - stats.lastShown) * 1e6);
        stats.maxIntervalMicros = std::max(stats.maxIntervalMicros.load(), micros);
    }
    stats.lastShown = presentedTime;
    if (started < 0.0)
        return;
    const double halfRefresh = stats.refreshHz > 0.0 ? 0.5 / stats.refreshHz : 0.0;
    if (stats.lastNewShown > 0.0 && presentedTime - stats.lastNewShown > started - stats.lastNewStarted + halfRefresh)
        stats.hitches++;
    stats.lastNewShown = presentedTime;
    stats.lastNewStarted = started;
}

} // namespace

DisplayOutput::DisplayOutput(Engine& engine, std::string display, VsyncPhase& vsync)
    : engine_(engine), display_(std::move(display)), vsync_(vsync),
      painter_(engine.device(), gpu::metalFormat(kSurfaceFormat), {engine.config().width, engine.config().height},
               engine.config().debugFrameBits, engine.config().opaqueOutputs),
      queue_(engine.gpuDevice().makeQueue()), gpu_(engine.device(), "present ", display_),
      screen_(std::make_shared<const ScreenView>()), thread_([this] {
          setThreadRole(ThreadRole::Frames);
          AZL_THREAD_NAME("present " + display_);
          run();
      })
{
    painter_.setGpuTimeline(&gpu_);
}

DisplayOutput::~DisplayOutput()
{
    detach();
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void DisplayOutput::attach(std::unique_ptr<Surface> surface)
{
    {
        std::lock_guard lock(mutex_);
        surface_ = std::move(surface);
        presenting_ = true;
    }
    wake_.notify_all();
}

void DisplayOutput::detach()
{
    std::unique_lock lock(mutex_);
    presenting_ = false;
    wake_.wait(lock, [this] { return !inRefreshes_; });
    std::unique_ptr<Surface> closed = std::move(surface_);
    lock.unlock();
}

void DisplayOutput::setScreen(std::shared_ptr<const ScreenView> screen)
{
    std::lock_guard lock(mutex_);
    screen_ = std::move(screen);
}

uint64_t DisplayOutput::screen() const
{
    std::lock_guard lock(mutex_);
    return screen_->screen.id;
}

void DisplayOutput::run()
{
    std::unique_lock lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this] { return stopping_ || presenting_; });
        if (stopping_)
            return;
        Surface& surface = *surface_;
        inRefreshes_ = true;
        resumed_ = true;
        lock.unlock();
        surface.runRefreshes(presenting_, [this, &surface](auto vsync) { present(surface, vsync); });
        lock.lock();
        inRefreshes_ = false;
        wake_.notify_all();
    }
}

void DisplayOutput::present(Surface& surface, std::chrono::steady_clock::time_point vsync)
{
    gpu::Scope scope;
    presentFrame(surface, vsync);
}

void DisplayOutput::presentFrame(Surface& surface, std::chrono::steady_clock::time_point vsync)
{
    stats_->refreshHz = surface.refreshHz();
    const auto refresh = std::chrono::duration_cast<VsyncPhase::Clock::duration>(
        std::chrono::duration<double>(1.0 / stats_->refreshHz));
    if (main_)
        vsync_.refreshed(vsync, refresh);
    AZL_ZONE("present");
    const auto begun = std::chrono::steady_clock::now();
    const gpu::Texture* target = nullptr;
    {
        AZL_ZONE("next target");
        target = surface.nextTarget();
    }
    if (!target)
        return;
    auto mark = engine_.config().frameTrace ? std::make_shared<FrameTrace::Present>(FrameTrace::Present{
                                                  .display = display_,
                                                  .vsync = traceSeconds(vsync),
                                                  .woke = traceSeconds(begun),
                                                  .targeted = traceNow()})
                                            : nullptr;

    // Only frames begun at least half a refresh before this vsync, which are finished by now. The main output's frames
    // begin just after its vsync, far from that edge.
    FrameRef frame = engine_.nextFrame(lastIndex_, vsync - refresh / 2);
    std::shared_ptr<const ScreenView> screen;
    {
        std::lock_guard lock(mutex_);
        screen = screen_;
    }
    gpu::CommandBuffer cmd = queue_.commandBuffer();
    painter_.encode(cmd.native(), frame.get(), *screen, target->native());
    double started = -1.0;
    if (frame && frame->index != lastIndex_) {
        if (!resumed_ && lastIndex_ != UINT64_MAX && frame->index > lastIndex_ + 1)
            stats_->dropped += frame->index - lastIndex_ - 1;
        resumed_ = false;
        lastIndex_ = frame->index;
        stats_->newFrames++;
        started = std::chrono::duration<double>(frame->started.time_since_epoch()).count();
    }
    if (mark && frame) {
        mark->index = frame->index;
        mark->fresh = started >= 0.0;
        mark->started = traceSeconds(frame->started);
    }
    cmd.onCompleted([held = std::move(frame), stats = stats_, mark, gpu = gpu_](const gpu::Completion& done) mutable {
        held.reset();
        AZL_GPU_COLLECT(gpu);
        stats->gpuMicros += uint64_t(std::max(0.0, done.gpuEnd - done.gpuStart) * 1e6);
        stats->gpuFrames++;
        if (mark) {
            mark->gpuStart = traceFromHost(done.gpuStart);
            mark->gpuEnd = traceFromHost(done.gpuEnd);
        }
    });
    surface.present(cmd, [stats = stats_, started, mark, trace = engine_.config().frameTrace](double presentedTime) {
        recordShown(*stats, presentedTime, started);
        if (mark) {
            mark->shown = presentedTime;
            trace->add(*mark);
        }
    });
    if (mark)
        mark->committed = traceNow();
    cmd.commit();
    stats_->presents++;
    const auto took = uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begun).count());
    stats_->maxPresentMicros = std::max(stats_->maxPresentMicros.load(), took);
    AZL_FRAME_MARK_NAMED("present " + display_);
    if (const int delay = delayMs_)
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
}

} // namespace azl
