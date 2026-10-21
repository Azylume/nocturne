#pragma once

#include "effects/chain_planner.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace azl {

// Whether an effect's result depends only on its input and its parameter values: every parameter is a plain value
// (no phase, beat count or audio level), its shaders never read the clocks or the frame time, and it keeps no history.
bool clockFree(const EffectDef& effect);

// How many leading passes of a chain are static by their make-up: never the last pass (it blends into the stack), and
// it stops at the first pass that reads the test card, a group, a transition's two clips or a persistent side target,
// or has a stage that is not clockFree or that `driven` (a macro, modulator, step or palette moves it) says is driven.
size_t staticPrefix(const std::vector<const PlannedPass*>& passes,
                    const std::function<bool(const PlannedStage&)>& driven);

// What a draw does with its static prefix this frame. Render thread only, one per draw.
class StaticBake {
public:
    enum class Action {
        // Draws the prefix as usual.
        Live,
        // Draws the prefix and keeps its result.
        Bake,
        // Draws nothing of the prefix and reads the kept result.
        Reuse,
    };
    // The picture the prefix reads: the same identity means the same pixels.
    using Source = std::array<const void*, 4>;

    // Frames the prefix's inputs besides its picture must stay the same, this one included, before it bakes.
    static constexpr int kSettleFrames = 4;

    // `prefix` static passes (staticPrefix) that read `inputs` this frame besides their picture `source`: their make-up,
    // header and parameter values as the shaders get them.
    Action step(size_t prefix, const std::vector<uint32_t>& inputs, const Source& source);

private:
    size_t prefix_ = 0;
    std::vector<uint32_t> inputs_;
    int settled_ = 0;
    std::optional<Source> baked_;
};

} // namespace azl
