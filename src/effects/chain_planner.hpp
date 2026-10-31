#pragma once

#include "effects/effect_registry.hpp"
#include "effects/isf.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace azl {

// What the first stage of a pass reads: the procedural test card, a clip's Y'CbCr planes, an RGBA texture such as a
// materialised intermediate, a group's sub-stack (premultiplied RGBA), or a text clip's coverage. YuvFade is an alpha
// transition between two clips' planes, mixed before the chain, so a chain both clips share runs once. Generator reads
// nothing: the chain's first stage, a generator, draws the picture.
enum class SourceKind { TestCard, Yuv, Texture, Group, Text, YuvFade, Generator };

struct PlannedStage {
    const EffectDef* effect = nullptr;
    // Position in the layer's chain, where its parameter values live.
    size_t chainIndex = 0;
};

// Texture slots of a pass: the source at 0 and 1, the copy of the target `passBlend` reads, then the last side target
// of each multi-pass stage the pass combines, in stage order; a side pass also reads its own last-frame target and its
// effect's earlier side targets.
inline constexpr size_t kBelowSlot = 2;
inline constexpr size_t kAuxSlot = 3;
inline constexpr size_t kMaxAux = 6;
inline constexpr size_t kPrevSlot = kAuxSlot + kMaxAux;
inline constexpr size_t kSideSlot = kPrevSlot + 1;
// A last pass during a transition: the outgoing clip's picture, and the TransitionMix at buffer(2).
inline constexpr size_t kFromSlot = kPrevSlot;
// A single-pass chain on a video source evaluates the outgoing clip itself: its luma and chroma at these slots, its
// PassHeader at buffer(3) and its parameters at buffer(4).
inline constexpr size_t kFromLumaSlot = kPrevSlot;
inline constexpr size_t kFromChromaSlot = kPrevSlot + 1;
// A YuvFade source reads the outgoing clip's luma and chroma here, its PassHeader at buffer(3) and the TransitionMix at
// buffer(2), in every pass.
inline constexpr size_t kFadeLumaSlot = kSideSlot + 16;
inline constexpr size_t kFadeChromaSlot = kFadeLumaSlot + 1;

// One GPU pass: GLSL with a fragment entry point per name in passEntries, each compiled with `#define ENTRY_<name>`
// (withEntry). It reads the textures above at their slots, the Header at buffer 0 and the stages' parameters, in
// stage order, at buffer 1 (shader_layout.hpp).
struct PlannedPass {
    SourceKind source = SourceKind::TestCard;
    // For a side pass: the stages fused before its multi-pass stage, then that stage.
    std::vector<PlannedStage> stages;
    // The last pass blends straight into the composition (or its group): `passMain` writes premultiplied colour for
    // the fixed-function blend modes, and `passBlend` composites over a copy of the target for the others. The other
    // passes write an intermediate or a side target.
    bool intoComposition = false;
    // Set for side pass number `side` of the last stage.
    std::optional<size_t> side;
    // The target is the composition size divided by this.
    uint32_t divisor = 1;
    // The target is RGBA16F instead of 8-bit.
    bool float16 = false;
    std::string glsl;
};

// Layout of buffer(0), shared with the generated shaders. A text source reads its fill colour, its outline colour and
// its placement (its picture's top left in pixels, then picture pixels per output pixel) from `yuvToRgb`.
struct PassHeader {
    float yuvToRgb[12];
    float size[2];
    float time;
    float opacity;
    // BlendMode, read by passBlend.
    uint32_t blend;
    // Clockwise quarter turns of the YUV source.
    uint32_t turns;
    // The beat clock's beats.
    float beat;
    // In a last pass, the stage (from 1) whose mask's 0 marks pixels that get no colour, which it discards; 0 for none.
    uint32_t blank;
    // Seconds since the previous rendered frame.
    float dt;
};

// Layout of buffer(2) of a last pass in a transition. `weight` is the incoming clip's share in 1/256 (alpha) or the
// share of its pixels (dissolve); `edge` is the pixel column where the incoming clip starts (wipe) or ends (push).
// `side` 1 draws only the incoming clip's share and 2 only the outgoing one's, the other clear, so each can blend on
// its own; 0 draws both. `params` are an ISF transition's parameter values.
struct TransitionMix {
    uint32_t type;
    uint32_t weight;
    uint32_t edge;
    uint32_t side;
    float params[kMaxTransitionParams];
};

// Fuses source + chain into as few passes as possible: pointwise and remap stages join the current pass, and a
// neighbourhood stage starts a new pass over the materialised output of the previous one. A multi-pass stage joins
// the current pass as the step that combines its side targets; its side passes come first and evaluate the stages
// before it themselves, so nothing is materialised for them. Passes are in the order they are drawn.
std::vector<PlannedPass> planChain(SourceKind source, const std::vector<const EffectDef*>& chain);

// Indexes into pass.stages of the multi-pass stages whose last side target the pass reads, slot kAuxSlot onwards.
std::vector<size_t> combinedStages(const PlannedPass& pass);
// Whether a last pass has the single-pass fade entry points.
bool singlePassFade(const PlannedPass& pass);
// The pass's fragment entry points in pipeline order: passMain alone, or for a last pass a pair per way of mixing in
// (plain, a transition's outgoing picture, the single-pass fade), the fixed-function entry before the one that
// composites over a copy of the target.
std::vector<std::string> passEntries(const PlannedPass& pass);


} // namespace azl
