#pragma once

#include "document/composition.hpp"

#include <array>
#include <optional>
#include <vector>

namespace azl::ui {

// The Map canvas's geometry, free of drawing: shapes are point lists in a screen's 0..1 space.
using Points = std::vector<Point>;

Points cornersOf(const Slice& slice);
std::array<double, 8> cornerArray(const Points& points);
Point centroid(const Points& points);

// Scaled and rotated about the centroid, then moved. `aspect` is the screen's width over its height, so a rotation
// keeps the shape on the real output.
Points transformed(const Points& base, double dx, double dy, double scale = 1.0, double radians = 0.0,
                   double aspect = 1.0);
// Edge k runs from point k to the next one; both move.
Points movedEdge(const Points& base, int edge, double dx, double dy);
// The angle swept around `centre` from `from` to `to`, in radians; snapped to 15 degrees.
double sweep(Point centre, Point from, Point to, bool snap);
// Every coordinate within the -1..2 the engine accepts.
Points clamped(Points points);
// Where the rotate handle sits: out from the centroid past the middle of the first edge, `reach` further.
Point rotateHandle(const Points& points, double reach);

// The perspective map of the unit square onto a quad (TL, TR, BR, BL), and back.
class Homography {
public:
    explicit Homography(const Points& quad);
    Point map(Point uv) const;
    std::optional<Point> unmap(Point p) const;

private:
    std::array<double, 9> m_{};
};

// The slice's outline on its screen: its warp grid's edges through its corner pin, `samples` points per patch edge.
Points sliceOutline(const Slice& slice, int samples = 8);

// The warp surface at a point of the slice's unit square: the bicubic Bezier patches over the resolved grid, as the
// engine tessellates them; the flat grid is the identity.
Point warpSurfaceAt(const Warp& warp, Point uv);
// Where a point of the slice's input (its UV) lands on the screen: input rect, warp, corner pin.
Point sliceToScreen(const Slice& slice, Point input);
// The input point that lands on `screen`, found from `guess`; none when it does not converge.
std::optional<Point> screenToSlice(const Slice& slice, Point screen, Point guess);

// The Map edits a warp as an interpolating grid: nx x ny points on the surface in the screen's space, the surface
// running through them as a Catmull-Rom patch with reflected border tangents. In the engine's Bezier model those
// points are the anchors under an identity corner pin, and each edge handle is a third of the Catmull-Rom tangent
// from its anchor, so the anchors and the grid lines are exactly the engine's.
Warp interpolatingWarp(int nx, int ny, const Points& anchors);
// The corners warp: one Bezier patch whose edges are cubic curves set by 12 points on the screen, TL, TR, BR, BL,
// then the tangent handles: top near TL, top near TR, right near TR, right near BR, bottom near BR, bottom near BL,
// left near BL, left near TL. The engine derives the interior from the edges by the Coons rule.
bool isCornerWarp(const Warp& warp);
Warp cornerWarp(const Points& points);
// The 12 points of a corners-warped slice on its screen; empty for another slice.
Points cornerPointsOf(const Slice& slice);
// The slice's current mapping as 12 points: its four outer corners, and handles at the thirds of each edge.
Points sampledCornerPoints(const Slice& slice);
// The 12 points with point `index` moved by `delta`; a corner takes its two handles along.
Points movedCornerPoint(const Points& points, int index, Point delta);
// The tangent handles of corner k.
std::array<int, 2> cornerHandles(int corner);

// Anchors per axis of a grid-warped slice, {0, 0} without one.
std::array<int, 2> warpAnchors(const Slice& slice);
// The anchors on the screen, row-major.
Points anchorsOf(const Slice& slice);
// The slice's current mapping sampled at nx x ny grid points on the screen.
Points sampledAnchors(const Slice& slice, int nx, int ny);
// The four outer anchors, TL, TR, BR, BL.
Points outerCorners(const Points& anchors, int nx, int ny);
// The slice with `shape` (its corners, its 12 corner-warp points, or its grid points) applied, as sending it would.
Slice withShape(const Slice& slice, const Points& shape);
// The warp grid's lines on the screen: every anchor row and column, `samples` points per patch.
std::vector<Points> warpCurves(const Slice& slice, int samples = 10);

bool inside(const Points& polygon, Point p);

// An input rect (x, y, width, height in 0..1) dragged by `delta`: moved (handle -1) or resized from a corner (0 TL,
// 1 TR, 2 BR, 3 BL) or an edge (4 top, 5 right, 6 bottom, 7 left). With `snap`, an edge within `reach` of 0, 1 or
// one of `edgesX` / `edgesY` lands on it.
struct InputSnap {
    std::vector<double> edgesX;
    std::vector<double> edgesY;
    Point reach{0.0, 0.0};
    bool snap = true;
};
std::array<double, 4> draggedRect(const std::array<double, 4>& base, int handle, Point delta, const InputSnap& snap);
// The overlap of two rects, if they overlap.
std::optional<std::array<double, 4>> overlap(const std::array<double, 4>& a, const std::array<double, 4>& b);

// A layer's Transform effect: position in composition widths and heights with +y up, scale, rotation in degrees
// counter-clockwise.
struct LayerTransform {
    double x = 0.0;
    double y = 0.0;
    double scale = 1.0;
    double rotation = 0.0;
};
// The box the layer's picture fills, TL, TR, BR, BL in composition UV; `aspect` is the composition's width over height.
Points transformBox(const LayerTransform& t, double aspect);
// About where a text clip's block lies, TL, TR, BR, BL in composition UV: its line count and longest line at its size.
Points textBox(const TextClip& text, double aspect);

// Frames on the board: one per screen, three per row, or the focused one alone. Board units, 16:9 frames.
struct BoardRect {
    double x = 0, y = 0, w = 0, h = 0;
    Point at(Point unit) const { return {x + unit[0] * w, y + unit[1] * h}; }
    Point unit(Point board) const { return {(board[0] - x) / w, (board[1] - y) / h}; }
};
inline constexpr double kFrameW = 288.0;
inline constexpr double kFrameH = 162.0;
BoardRect frameRect(int index, bool focused);
// The Input view: the input picture large, and the board around it.
inline constexpr BoardRect kInputRect{20.0, 20.0, 960.0, 540.0};
inline constexpr BoardRect kInputBoard{0.0, 0.0, 1000.0, 580.0};
BoardRect boardBounds(int screens, bool focused);
// A screen's frame among all screens: at its saved position, else in the grid, in list order, after the screens shown
// before it.
BoardRect mapFrame(const std::vector<Screen>& screens, uint64_t screen);
// The grid of the shown screens, grown to take in every shown frame at a saved position.
BoardRect mapBounds(const std::vector<Screen>& screens);

// Board units to canvas pixels: pixel = board * scale + offset.
struct Viewport {
    double scale = 1.0;
    Point offset{0.0, 0.0};
    Point toPixel(Point b) const { return {b[0] * scale + offset[0], b[1] * scale + offset[1]}; }
    Point toBoard(Point px) const { return {(px[0] - offset[0]) / scale, (px[1] - offset[1]) / scale}; }
};
Viewport fitted(const BoardRect& board, double width, double height);
// Zoomed by `factor` keeping the board point under `pixel` in place, the scale held within min..max.
Viewport zoomedAround(const Viewport& view, Point pixel, double factor, double minScale, double maxScale);

} // namespace azl::ui
