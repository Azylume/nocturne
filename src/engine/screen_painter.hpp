#pragma once

#include "document/composition.hpp"
#include "engine/frame_ring.hpp"
#include "platform/profile.hpp"

#include <Metal/Metal.hpp>

#include <array>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace azl {

// The square-to-quad homography of a slice's output corners in clip space:
//   x = (a u + b v + c) / w,  y = (d u + e v + f) / w,  w = g u + h v + 1
// Null for corners no projective map reaches without folding the quad (w <= 0 somewhere in it).
std::optional<std::array<float, 8>> sliceHomography(const std::array<double, 8>& corners);

// What an output paints: its screen, with the composition's test card already folded into the screen's, the guides
// while they are shown, and the corner target the machine shows on that output (in its 0..1 space).
struct ScreenView {
    Screen screen;
    std::vector<Guide> guides{};
    std::optional<Point> target{};
    // Until then the output shows its screen's name and output large over the test card, to tell projectors apart.
    std::chrono::steady_clock::time_point identify{};

    bool operator==(const ScreenView&) const = default;
};

ScreenView screenView(const Composition& composition, const Screen& screen, bool showGuides,
                      std::optional<Point> target, std::chrono::steady_clock::time_point identify = {});

// Draws a screen onto its target in one pass: each slice's input picture (or the composition-space test card) through
// its warp and corner pin, with its colour, soft edge and masks; then the screen's masks, the guides through the slices
// and the corner target. Display outputs and the harness share it, so the harness reads back exactly what a display
// shows. Every pipeline compiles in the constructor; edits only rebuild one buffer of uniforms.
//
// With `frameBits`, a strip along the bottom edge shows the render frame index in binary: kFrameBits bars, the most
// significant left, white for 1. The rightmost bar flips every frame, so a dropped or repeated frame shows.
//
// One thread at a time.
class ScreenPainter {
public:
    static constexpr uint32_t kFrameBits = 16;

    // `composition` is the composition picture's size, which the test card takes. With `opaqueSlices`, a slice of the
    // composition picture with no masks or soft edge draws without blending: the picture is opaque, so blending over
    // the cleared target gives the same.
    ScreenPainter(MTL::Device* device, MTL::PixelFormat targetFormat, std::array<uint32_t, 2> composition,
                  bool frameBits = false, bool opaqueSlices = false);
    ~ScreenPainter();

    // A null frame paints black, as does a slice whose input picture the frame lacks; the test card needs no frame.
    void encode(MTL::CommandBuffer* cmd, const Frame* frame, const ScreenView& view, MTL::Texture* target);
    // The Tracy timeline of the queue it encodes for, which must outlive it.
    void setGpuTimeline(const profile::GpuTimeline* timeline) { gpu_ = timeline; }

private:
    struct Draw;
    struct Prepared;

    void prepare(MTL::CommandBuffer* cmd, const Screen& screen, uint32_t width, uint32_t height);
    NS::SharedPtr<MTL::Texture> texture(MTL::PixelFormat format, uint32_t width, uint32_t height) const;
    MTL::Buffer* meshIndices(std::array<int, 2> patches);
    void renderMasks(MTL::CommandBuffer* cmd, MTL::Texture* coverage, uint32_t at, uint32_t count) const;
    MTL::Texture* card(MTL::CommandBuffer* cmd, const Screen& screen);
    void drawSlice(MTL::RenderCommandEncoder* enc, const Draw& draw, MTL::RenderPipelineState* quad,
                   MTL::RenderPipelineState* warped) const;
    void encodeSlices(MTL::RenderCommandEncoder* enc, const Frame* frame, MTL::Texture* card) const;
    void encodeGuides(MTL::RenderCommandEncoder* enc, const std::vector<Guide>& guides) const;
    void encodeTarget(MTL::RenderCommandEncoder* enc, const Point& target, MTL::Texture* texture) const;
    void encodeFrameBits(MTL::RenderCommandEncoder* enc, uint64_t index, MTL::Texture* target) const;
    void encodeIdentify(MTL::RenderCommandEncoder* enc, const Screen& screen, MTL::Texture* target) const;
    void bindSliceBuffers(MTL::RenderCommandEncoder* enc) const;

    MTL::Device* device_;
    const profile::GpuTimeline* gpu_ = nullptr;
    std::array<uint32_t, 2> composition_;
    struct SlicePipelines {
        NS::SharedPtr<MTL::RenderPipelineState> quad;
        NS::SharedPtr<MTL::RenderPipelineState> warped;
    };
    SlicePipelines slicePipelines(MTL::Device* device, MTL::Library* library, const char* fragment,
                                  MTL::PixelFormat format, bool blend) const;

    // Blended, then (with opaqueSlices) unblended.
    SlicePipelines slices_[2];
    // For slices with colour, a soft edge or masks.
    SlicePipelines fullSlices_[2];
    NS::SharedPtr<MTL::RenderPipelineState> guides_;
    NS::SharedPtr<MTL::RenderPipelineState> warpedGuides_;
    NS::SharedPtr<MTL::RenderPipelineState> maskCoverage_;
    NS::SharedPtr<MTL::RenderPipelineState> target_;
    NS::SharedPtr<MTL::RenderPipelineState> card_;
    NS::SharedPtr<MTL::RenderPipelineState> identify_;
    // Null unless the strip is on.
    NS::SharedPtr<MTL::RenderPipelineState> frameBits_;

    std::unique_ptr<Prepared> prepared_;
    std::map<std::array<int, 2>, NS::SharedPtr<MTL::Buffer>> meshes_;
    // Mask coverage, 0 where masks hide: the screen's at the target's size, slices' at the composition's.
    NS::SharedPtr<MTL::Texture> screenMaskTexture_;
    std::vector<NS::SharedPtr<MTL::Texture>> sliceMaskTextures_;
    NS::SharedPtr<MTL::Texture> noMask_;
    NS::SharedPtr<MTL::Texture> cardTexture_;
    std::string cardKey_;
};

} // namespace azl
