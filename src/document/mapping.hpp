#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace azl {

using Point = std::array<double, 2>;

inline constexpr int kMaxWarpPatches = 16;

// A Bezier warp grid over a slice's unit square, applied before its corner pin, as Resolume's warper. Each axis has
// `patches` bicubic patches sharing edges, so 3 * patches + 1 control points; a point whose column and row are both
// multiples of 3 is an anchor, the rest are handles. In linear mode the edge handles sit at thirds between their
// anchors, so every patch is a bilinear quad; in Bezier mode they shape the edges. Interior handles are always derived
// from the patch's edges. Points are row-major; none is the flat grid.
struct Warp {
    std::array<int, 2> patches{1, 1};
    bool bezier = false;
    std::vector<Point> points;

    bool operator==(const Warp&) const = default;
};

// Control points per axis.
inline std::array<int, 2> warpGridSize(const Warp& warp)
{
    return {warp.patches[0] * 3 + 1, warp.patches[1] * 3 + 1};
}

// The grid as it renders: every point, with linear mode's edge handles and the interior handles derived.
std::vector<Point> resolvedWarpGrid(const Warp& warp);

// The same surface over another number of patches; exact when the new counts are multiples of the old.
Warp regridWarp(const Warp& warp, std::array<int, 2> patches);

// Fades a slice's picture out towards its edges, for blending overlapping projectors. Widths are fractions of the
// slice's input rect from the left, right, top and bottom edge. Across a width the gain follows Paul Bourke's
// two-segment curve: `luminance` at its middle, `power` its steepness, then raised to 1 / `gamma` for the projector.
struct SoftEdge {
    double left = 0.0;
    double right = 0.0;
    double top = 0.0;
    double bottom = 0.0;
    double power = 2.0;
    double luminance = 0.5;
    double gamma = 2.2;

    bool operator==(const SoftEdge&) const = default;
};

// Each -1..1, 0 being no change. A slice's adds to its screen's, clamped, as in Resolume: the colour gets the RGB
// offsets, then brightness 1 + b scales it and contrast 1 + c spreads it around grey.
struct ColorAdjust {
    double brightness = 0.0;
    double contrast = 0.0;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;

    bool operator==(const ColorAdjust&) const = default;
};

inline constexpr size_t kMaxMasks = 16;
inline constexpr size_t kMaxMaskPoints = 64;

// A polygon that blacks out the picture inside it, or with `invert` everything outside it. A screen's masks lie in
// its 0..1 space and black out every slice; a slice's lie in its input's UV, like its input rect, and hide that
// slice only, following its warp.
struct Mask {
    uint64_t id = 0;
    std::string name;
    bool invert = false;
    std::vector<Point> points;

    bool operator==(const Mask&) const = default;
};

inline constexpr size_t kMaxGuides = 32;

// A vertical (x) or horizontal (y) line at a position in composition UV, which outputs draw through every slice that
// covers it while guides are shown.
struct Guide {
    bool vertical = true;
    double position = 0.5;

    bool operator==(const Guide&) const = default;
};

// Changes only the fields that are set, in this order: the point mode, then the patches (keeping the shape the warp has
// in that mode), then the points (as many as the grid has, or none for the flat grid).
struct SetSliceWarp {
    uint64_t slice = 0;
    std::optional<bool> bezier;
    std::optional<std::array<int, 2>> patches;
    std::optional<std::vector<Point>> points;
};

struct SliceSoftEdge {
    uint64_t slice = 0;
    SoftEdge softEdge;

    bool operator==(const SliceSoftEdge&) const = default;
};

// Sets slices' soft edges in one step. With `fromOverlap`, the widths come from overlaps instead: that slice and every
// slice of the same input whose input rect overlaps it get a fade across each overlap, on both sides; applying it fills
// in `slices`.
struct SetSoftEdges {
    std::vector<SliceSoftEdge> slices;
    uint64_t fromOverlap = 0;
};

struct SetSliceColor {
    uint64_t slice = 0;
    ColorAdjust color;
};

struct SetScreenColor {
    uint64_t screen = 0;
    ColorAdjust color;
};

// Onto a screen, or with a slice onto that slice of it. A zero id gets a fresh one; a missing index means the end.
struct AddMask {
    uint64_t screen = 0;
    uint64_t slice = 0;
    std::optional<size_t> index;
    Mask mask;
};

struct RemoveMask {
    uint64_t mask = 0;
};

// Changes only the fields that are set.
struct SetMask {
    uint64_t mask = 0;
    std::optional<std::vector<Point>> points;
    std::optional<bool> invert;
    std::optional<std::string> name;
};

struct SetGuides {
    std::vector<Guide> guides;
};

} // namespace azl
