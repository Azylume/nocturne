#include "engine/gpu_profile.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <utility>

namespace azl {

namespace {

constexpr size_t kBuffers = 6;
// Counter ticks are calibrated against command buffer start times this many seconds apart before anything counts.
constexpr double kCalibration = 0.5;

std::atomic<bool> noCounterSampling{false};

} // namespace

void GpuProfile::forceNoCounterSampling(bool force)
{
    noCounterSampling = force;
}

GpuProfile::GpuProfile(const gpu::Device& device, uint32_t slot, bool commandBuffer) : slot_(slot)
{
    switch (noCounterSampling ? gpu::TimestampMode::None : device.timestampMode()) {
    case gpu::TimestampMode::StageBoundary: mode_ = Mode::Stage; break;
    case gpu::TimestampMode::DrawBoundary: mode_ = Mode::Draw; break;
    case gpu::TimestampMode::None: break;
    }
    for (size_t i = 0; mode_ != Mode::Off && i < kBuffers; ++i) {
        gpu::TimestampQueries queries = device.makeTimestampQueries(2 * kMaxPasses);
        if (!queries) {
            mode_ = Mode::Off;
            free_.clear();
            break;
        }
        free_.push_back(std::move(queries));
    }
    if (mode_ == Mode::Off && commandBuffer)
        mode_ = Mode::CommandBuffer;
}

const char* GpuProfile::mode() const
{
    switch (mode_) {
    case Mode::Stage: return "stage boundary";
    case Mode::Draw: return "draw boundary";
    case Mode::CommandBuffer: return "command buffer";
    case Mode::Off: break;
    }
    return "off";
}

std::shared_ptr<GpuProfile::Frame> GpuProfile::begin()
{
    if (mode_ == Mode::Off)
        return nullptr;
    if (mode_ == Mode::CommandBuffer)
        return std::make_shared<Frame>();
    std::lock_guard lock(mutex_);
    if (free_.empty())
        return nullptr;
    auto frame = std::make_shared<Frame>();
    frame->queries = std::move(free_.back());
    free_.pop_back();
    return frame;
}

void GpuProfile::attach(Frame& frame, gpu::RenderTarget& target, std::string label)
{
    if (frame.labels.size() == kMaxPasses)
        return;
    const auto index = uint32_t(2 * frame.labels.size());
    frame.labels.push_back(std::move(label));
    if (mode_ == Mode::Stage)
        target.timestamps[slot_] = {&frame.queries, index, index + 1};
}

void GpuProfile::sample(Frame& frame, gpu::RenderPass& pass, bool start)
{
    if (mode_ == Mode::Draw && !frame.labels.empty())
        pass.writeTimestamp(frame.queries, uint32_t(2 * (frame.labels.size() - 1) + (start ? 0 : 1)));
}

uint64_t GpuProfile::finish(Frame& frame, const gpu::Completion& done)
{
    if (mode_ == Mode::CommandBuffer) {
        if (!done.ok || frame.labels.empty() || frame.passes == 0 || done.gpuEnd <= done.gpuStart)
            return 0;
        const double share = double(std::min(frame.labels.size(), frame.passes)) / double(frame.passes);
        return uint64_t((done.gpuEnd - done.gpuStart) * 1e6 * share);
    }
    const size_t count = 2 * frame.labels.size();
    std::array<uint64_t, 2 * kMaxPasses> stamps{};
    const bool resolved =
        done.ok && count && frame.queries.resolve(0, std::span(stamps).first(count)) && stamps[0] != 0;
    std::lock_guard lock(mutex_);
    free_.push_back(std::move(frame.queries));
    if (!resolved)
        return 0;
    if (!anchor_)
        anchor_ = {done.gpuStart, stamps[0]};
    else if (done.gpuStart - anchor_->first >= kCalibration && stamps[0] > anchor_->second)
        microsPerTick_ = (done.gpuStart - anchor_->first) * 1e6 / double(stamps[0] - anchor_->second);
    if (microsPerTick_ == 0.0)
        return 0;
    std::map<std::string, uint64_t> micros;
    uint64_t total = 0;
    for (size_t i = 0; i < frame.labels.size(); ++i) {
        const uint64_t start = stamps[2 * i];
        const uint64_t end = stamps[2 * i + 1];
        if (start == 0 || end <= start)
            continue;
        const std::string& label = frame.labels[i];
        const auto us = uint64_t(double(end - start) * microsPerTick_);
        micros[label] += us;
        total += us;
        if (const size_t bar = label.find(" | "); bar != std::string::npos)
            micros[label.substr(0, bar)] += us;
    }
    for (const auto& [label, us] : micros) {
        totals_[label].micros += us;
        totals_[label].frames++;
    }
    return total;
}

std::map<std::string, GpuProfile::Total> GpuProfile::take()
{
    std::lock_guard lock(mutex_);
    return std::exchange(totals_, {});
}

} // namespace azl
