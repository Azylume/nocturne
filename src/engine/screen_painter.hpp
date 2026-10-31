#pragma once

#include "document/composition.hpp"
#include "engine/frame_ring.hpp"
#include "gpu/gpu.hpp"
#include "platform/profile.hpp"

#include <array>
#include <chrono>
#include <deque>
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
    ScreenPainter(const gpu::Device& device, gpu::Format targetFormat, std::array<uint32_t, 2> composition,
                  bool frameBits = false, bool opaqueSlices = false);
    ~ScreenPainter();

    // A null frame paints black, as does a slice whose input picture the frame lacks; the test card needs no frame.
    void encode(gpu::CommandBuffer& cmd, const Frame* frame, const ScreenView& view, const gpu::Texture& target);
    // The Tracy timeline of the queue it encodes for, which must outlive it.
    void setGpuTimeline(const profile::GpuTimeline* timeline) { gpu_ = timeline; }

private:
    struct Draw;
    struct Prepared;

    void prepare(gpu::CommandBuffer& cmd, const Screen& screen, uint32_t width, uint32_t height);
    gpu::Texture texture(gpu::Format format, uint32_t width, uint32_t height) const;
    gpu::RenderPass begin(gpu::CommandBuffer& cmd, const gpu::RenderTarget& target, const char* name) const;
    const gpu::Buffer& meshIndices(std::array<int, 2> patches);
    void renderMasks(gpu::CommandBuffer& cmd, const gpu::Texture& coverage, uint32_t at, uint32_t count) const;
    const gpu::Texture* card(gpu::CommandBuffer& cmd, const Screen& screen);
    void drawSlice(gpu::RenderPass& pass, const Draw& draw, const gpu::RenderPipeline& quad,
                   const gpu::RenderPipeline& warped) const;
    void encodeSlices(gpu::RenderPass& pass, const Frame* frame, const gpu::Texture* card) const;
    void encodeGuides(gpu::RenderPass& pass, const std::vector<Guide>& guides) const;
    void encodeTarget(gpu::RenderPass& pass, const Point& target, const gpu::Texture& texture) const;
    void encodeFrameBits(gpu::RenderPass& pass, uint64_t index, const gpu::Texture& target) const;
    void encodeIdentify(gpu::RenderPass& pass, const Screen& screen, const gpu::Texture& target) const;
    void bindSliceBuffers(gpu::RenderPass& pass) const;

    gpu::Device device_;
    const profile::GpuTimeline* gpu_ = nullptr;
    std::array<uint32_t, 2> composition_;
    struct SlicePipelines {
        gpu::RenderPipeline quad;
        gpu::RenderPipeline warped;
    };
    gpu::RenderPipeline pipeline(const gpu::ShaderModule& module, const char* vertex, const char* fragment,
                                 gpu::Format format, gpu::Blending blending = gpu::Blending::None) const;
    SlicePipelines slicePipelines(const gpu::ShaderModule& module, const char* fragment, gpu::Format format,
                                  bool blend) const;

    // Blended, then (with opaqueSlices) unblended.
    SlicePipelines slices_[2];
    // For slices with colour, a soft edge or masks.
    SlicePipelines fullSlices_[2];
    gpu::RenderPipeline guides_;
    gpu::RenderPipeline warpedGuides_;
    gpu::RenderPipeline maskCoverage_;
    gpu::RenderPipeline target_;
    gpu::RenderPipeline card_;
    gpu::RenderPipeline identify_;
    // Null unless the strip is on.
    gpu::RenderPipeline frameBits_;

    std::unique_ptr<Prepared> prepared_;
    std::map<std::array<int, 2>, gpu::Buffer> meshes_;
    // Mask coverage, 0 where masks hide: the screen's at the target's size, slices' at the composition's.
    gpu::Texture screenMaskTexture_;
    // A deque, so a Draw's pointer to one survives the next being added.
    std::deque<gpu::Texture> sliceMaskTextures_;
    gpu::Texture noMask_;
    gpu::Texture cardTexture_;
    std::string cardKey_;
};

} // namespace azl
