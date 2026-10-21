#pragma once

#include <Metal/Metal.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace azl {

// GPU time of every render pass of a frame by label, from timestamps around each pass: at the pass's stage
// boundaries where the GPU samples there (Apple GPUs), else as the pass's first and last commands (Intel).
class GpuProfile {
public:
    // Disabled when the device samples timestamps neither way, unless `commandBuffer`: then a frame's timed passes get
    // the command buffer's GPU time by their share of its passes, and only finish's total counts. `attachment` is the
    // pass's sample buffer attachment it uses, so two profiles can time the same pass.
    explicit GpuProfile(MTL::Device* device, NS::UInteger attachment = 0, bool commandBuffer = false);

    // Test seam: profiles made after this believe the device has no counter sampling.
    static void forceNoCounterSampling(bool force);

    bool enabled() const { return mode_ != Mode::Off; }
    // The mode in use, for the log: "stage boundary", "draw boundary", "command buffer" or "off".
    const char* mode() const;

    struct Frame {
        NS::SharedPtr<MTL::CounterSampleBuffer> buffer;
        std::vector<std::string> labels;
        // Every pass of the command buffer, timed or not.
        size_t passes = 0;
    };
    // Render thread: null when disabled or every buffer is still on the GPU.
    std::shared_ptr<Frame> begin();
    // Around each render pass: `attach` before its encoder is made, `sample` once it is made and before it ends.
    void attach(Frame& frame, MTL::RenderPassDescriptor* desc, std::string label);
    void sample(Frame& frame, MTL::RenderCommandEncoder* enc, bool start);
    // Once the frame's command buffer has completed (`gpuStart` and `gpuEnd` are its GPUStartTime and GPUEndTime): adds
    // its passes' times, gives the buffer back and returns the frame's total.
    uint64_t finish(Frame& frame, bool ok, double gpuStart, double gpuEnd);

    struct Total {
        uint64_t micros = 0;
        // Frames that drew the label.
        uint64_t frames = 0;
    };
    // Any thread: the totals by label since the last take. A label "who side | effects" also adds to "who side".
    std::map<std::string, Total> take();

private:
    enum class Mode { Off, Stage, Draw, CommandBuffer };
    static constexpr size_t kMaxPasses = 256;

    Mode mode_ = Mode::Off;
    NS::UInteger attachment_ = 0;
    NS::SharedPtr<MTL::Device> device_;
    NS::SharedPtr<MTL::CounterSet> timestamps_;
    std::mutex mutex_;
    std::vector<NS::SharedPtr<MTL::CounterSampleBuffer>> free_;
    std::map<std::string, Total> totals_;
    // A command buffer's start in seconds and its first timestamp: counter ticks are in no documented unit.
    std::optional<std::pair<double, uint64_t>> anchor_;
    double microsPerTick_ = 0.0;
};

} // namespace azl
