#pragma once

#include "audio/audio_analysis.hpp"
#include "document/composition.hpp"
#include "engine/chain_compiler.hpp"
#include "engine/modulation_plan.hpp"
#include "engine/side_targets.hpp"
#include "gpu/gpu.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace azl {

// Draws a clip's own effect chain (not its layer's, group's or the composition's) over a small picture of its media,
// for the clip previews, entirely apart from the show: its own compiler (one Background thread), its own textures,
// its own side targets, and the caller's command queue. Modulators run live from the show's beat clock and
// audio analysis, evaluated in a plan of its own. Not thread-safe: one caller thread.
class ClipEffectsPreview {
public:
    struct Stats {
        std::atomic<uint64_t> pictures{0};
        std::atomic<uint64_t> gpuMicros{0};
    };

    explicit ClipEffectsPreview(const gpu::Device& device);

    // Draws `clip`'s chain over `bgra` (width x height, opaque BGRA) in place and returns true; false, leaving the
    // picture as it is, while its chain is not compiled yet, which this asks for. `slot` names the clip: another one
    // starts Trails over.
    bool draw(const gpu::Queue& queue, const std::shared_ptr<const Composition>& composition, const ClipRef& clip,
              const std::string& slot, std::vector<uint8_t>& bgra, uint32_t width, uint32_t height, double seconds,
              double beat, const AudioAnalysis* audio);

    // Frees the textures, side targets and modulation plan; the next draw makes them again. Compiled
    // chains stay.
    void release();

    // Compiled chains, side targets and spares, and the allocated KB of all textures. Never during a draw.
    std::map<std::string, size_t> sizes() const;

    ChainCompiler& compiler() { return compiler_; }
    const Stats& stats() const { return stats_; }

private:
    const gpu::Texture& texture(gpu::Texture& texture, gpu::Format format, gpu::Memory memory);
    void encodePass(gpu::RenderPass& into, const CompiledPass& pass, const EffectList& effects,
                    const gpu::Texture& input, float seconds);

    gpu::Device device_;
    ChainCompiler compiler_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::unique_ptr<SideTargets> sides_;
    gpu::Texture source_;
    gpu::Texture target_;
    gpu::Readback readback_;
    gpu::Texture intermediates_[2][2];
    std::shared_ptr<const Composition> planned_;
    std::unique_ptr<ModulationPlan> plan_;
    std::vector<float> params_;
    std::vector<float> rates_;
    // Seconds since the previous draw, for effects that fade by elapsed time.
    double dt_ = 1.0 / 60.0;
    double lastSeconds_ = -1.0;
    Stats stats_;
};

} // namespace azl
