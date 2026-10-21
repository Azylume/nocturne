#include "ui/app/map_geometry.hpp"

#include <doctest.h>

#include <cmath>

using namespace azl;
using namespace azl::ui;

namespace {

constexpr double kPi = 3.14159265358979323846;

void near(Point a, Point b, double eps = 1e-9)
{
    CHECK(a[0] == doctest::Approx(b[0]).epsilon(eps));
    CHECK(a[1] == doctest::Approx(b[1]).epsilon(eps));
}

} // namespace

TEST_CASE("map geometry: the corner pin maps the unit square onto its quad and back")
{
    const Points quad{{0.1, 0.05}, {0.9, 0.15}, {0.95, 0.9}, {0.02, 0.8}};
    const Homography pin(quad);
    near(pin.map({0, 0}), quad[0]);
    near(pin.map({1, 0}), quad[1]);
    near(pin.map({1, 1}), quad[2]);
    near(pin.map({0, 1}), quad[3]);
    const Point inside = pin.map({0.3, 0.7});
    near(*pin.unmap(inside), {0.3, 0.7}, 1e-7);
}

TEST_CASE("map geometry: move, edge, rotation with 15 degree snap, and the output's aspect")
{
    const Points square{{0.4, 0.4}, {0.6, 0.4}, {0.6, 0.6}, {0.4, 0.6}};
    const Points moved = transformed(square, 0.1, -0.05);
    near(moved[2], {0.7, 0.55});

    const Points edge = movedEdge(square, 1, 0.1, 0.0);
    near(edge[0], square[0]);
    near(edge[1], {0.7, 0.4});
    near(edge[2], {0.7, 0.6});
    near(edge[3], square[3]);
    const Points wrap = movedEdge(square, 3, 0.0, 0.1);
    near(wrap[3], {0.4, 0.7});
    near(wrap[0], {0.4, 0.5});

    const Point c{0.5, 0.5};
    const double twenty = 20.0 * kPi / 180.0;
    const Point from{0.6, 0.5}, to{0.5 + 0.1 * std::cos(twenty), 0.5 + 0.1 * std::sin(twenty)};
    CHECK(sweep(c, from, to, false) == doctest::Approx(twenty));
    CHECK(sweep(c, from, to, true) == doctest::Approx(15.0 * kPi / 180.0));

    // A quarter turn on a 16:9 output keeps the square's width in pixels as its height.
    const double aspect = 16.0 / 9.0;
    const Points turned = transformed(square, 0, 0, 1, kPi / 2, aspect);
    const double widthPx = (square[1][0] - square[0][0]) * 1920.0;
    const double heightPx = std::abs(turned[1][1] - turned[0][1]) * 1080.0;
    CHECK(heightPx == doctest::Approx(widthPx));

    const Points scaled = transformed(square, 0, 0, 2.0);
    near(scaled[0], {0.3, 0.3});
    near(clamped({{-3.0, 5.0}})[0], {-1.0, 2.0});
}

TEST_CASE("map geometry: an interpolating warp runs through its grid points, and the edges are Catmull-Rom")
{
    Slice slice;
    CHECK(sliceOutline(slice) == cornersOf(slice));
    CHECK(warpAnchors(slice) == std::array<int, 2>{0, 0});

    // Sampling a perspective quad, then going back to a corner pin, keeps its corners.
    slice.corners = {0.1, 0.05, 0.9, 0.15, 0.95, 0.9, 0.02, 0.8};
    const Points sampled = sampledAnchors(slice, 3, 3);
    near(sampled[0], {0.1, 0.05});
    near(sampled[8], {0.95, 0.9});
    near(sampled[4], Homography(cornersOf(slice)).map({0.5, 0.5}));
    for (size_t k = 0; k < 4; ++k)
        near(outerCorners(sampled, 3, 3)[k], cornersOf(slice)[k]);

    // The anchors are exactly the engine surface's points at the grid's parameters.
    Points anchors = sampledAnchors(Slice{}, 4, 4);
    anchors[5] = {0.42, 0.29};
    anchors[10] = {0.71, 0.6};
    const Slice warped = withShape(slice, cornersOf(slice));
    CHECK(warped.corners == slice.corners);
    Slice grid;
    grid.warp = interpolatingWarp(4, 4, anchors);
    CHECK(warpAnchors(grid) == std::array<int, 2>{4, 4});
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) {
            near(warpSurfaceAt(grid.warp, {i / 3.0, j / 3.0}), anchors[size_t(j * 4 + i)], 1e-12);
            near(anchorsOf(grid)[size_t(j * 4 + i)], anchors[size_t(j * 4 + i)], 1e-12);
        }
    // Along a grid line the surface is the Catmull-Rom curve through that row.
    const Point p0{2 * anchors[4][0] - anchors[5][0], 2 * anchors[4][1] - anchors[5][1]};
    const Point p1 = anchors[4], p2 = anchors[5], p3 = anchors[6];
    const double t = 0.3;
    Point cr;
    for (int k = 0; k < 2; ++k)
        cr[size_t(k)] = 0.5 * (2 * p1[k] + (p2[k] - p0[k]) * t + (2 * p0[k] - 5 * p1[k] + 4 * p2[k] - p3[k]) * t * t +
                               (3 * p1[k] - p0[k] - 3 * p2[k] + p3[k]) * t * t * t);
    near(warpSurfaceAt(grid.warp, {t / 3.0, 1.0 / 3.0}), cr, 1e-12);
    CHECK(warpCurves(grid, 4).size() == 8);

    // A shape applied to a warped slice gives an identity pin and the anchors as the warp.
    Slice pinned;
    pinned.corners = {0.1, 0.1, 0.9, 0.1, 0.9, 0.9, 0.1, 0.9};
    pinned.warp = interpolatingWarp(3, 3, sampledAnchors(Slice{}, 3, 3));
    const Points pinnedAnchors = anchorsOf(pinned);
    near(pinnedAnchors[0], {0.1, 0.1});
    const Slice applied = withShape(pinned, pinnedAnchors);
    CHECK(applied.corners == std::array<double, 8>{0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0});
    for (size_t k = 0; k < 9; ++k)
        near(anchorsOf(applied)[k], pinnedAnchors[k], 1e-12);
}

TEST_CASE("map geometry: the corners warp maps 12 points onto one Bezier patch, corners take their handles along")
{
    Slice slice;
    slice.corners = {0.1, 0.05, 0.9, 0.15, 0.95, 0.9, 0.02, 0.8};
    const Points sampled = sampledCornerPoints(slice);
    REQUIRE(sampled.size() == 12);
    near(sampled[1], {0.9, 0.15});
    near(sampled[4], {0.1 + 0.8 / 3, 0.05 + 0.1 / 3});
    near(sampled[11], {0.1 + (0.02 - 0.1) / 3, 0.05 + (0.8 - 0.05) / 3});
    Slice warped = withShape(Slice{.warp = cornerWarp(sampled)}, sampled);
    CHECK(isCornerWarp(warped.warp));
    CHECK(warpAnchors(warped) == std::array<int, 2>{0, 0});
    const Points back = cornerPointsOf(warped);
    for (size_t k = 0; k < 12; ++k)
        near(back[k], sampled[k]);
    // The top edge is the cubic through TL, its two handles and TR.
    near(warpSurfaceAt(warped.warp, {0.5, 0.0}),
         {0.125 * sampled[0][0] + 0.375 * sampled[4][0] + 0.375 * sampled[5][0] + 0.125 * sampled[1][0],
          0.125 * sampled[0][1] + 0.375 * sampled[4][1] + 0.375 * sampled[5][1] + 0.125 * sampled[1][1]});
    const Points moved = movedCornerPoint(sampled, 2, {0.1, -0.1});
    for (int k : {2, 7, 8})
        near(moved[size_t(k)], {sampled[size_t(k)][0] + 0.1, sampled[size_t(k)][1] - 0.1});
    near(moved[6], sampled[6]);
    const Points handle = movedCornerPoint(sampled, 7, {0.1, 0.0});
    near(handle[2], sampled[2]);
}

TEST_CASE("map geometry: slice mask points map through input rect, warp and pin, and back")
{
    Slice slice;
    slice.inputRect = {0.2, 0.0, 0.6, 1.0};
    slice.corners = {0.1, 0.05, 0.9, 0.15, 0.95, 0.9, 0.02, 0.8};
    Points anchors = sampledAnchors(Slice{}, 3, 3);
    anchors[4] = {0.55, 0.45};
    slice.warp = interpolatingWarp(3, 3, anchors);
    const Point input{0.5, 0.5};
    const Point screen = sliceToScreen(slice, input);
    const auto back = screenToSlice(slice, screen, {0.3, 0.3});
    REQUIRE(back);
    near(*back, input, 1e-7);
}

TEST_CASE("map geometry: input rects move, resize, snap to edges and clamp; overlaps")
{
    const std::array<double, 4> base{0.0, 0.0, 0.55, 1.0};
    InputSnap snap{{0.45, 1.0}, {0.0, 1.0}, {0.01, 0.01}, true};
    auto r = draggedRect(base, 5, {-0.095, 0.0}, snap);
    CHECK(r[2] == doctest::Approx(0.45));
    snap.snap = false;
    r = draggedRect(base, 5, {-0.095, 0.0}, snap);
    CHECK(r[2] == doctest::Approx(0.455));
    r = draggedRect(base, -1, {0.7, 0.2}, snap);
    CHECK(r[0] == doctest::Approx(0.45));
    CHECK(r[1] == doctest::Approx(0.0));
    r = draggedRect(base, 7, {0.9, 0.0}, snap);
    CHECK(r[2] == doctest::Approx(0.01));
    r = draggedRect({0.2, 0.2, 0.3, 0.3}, 0, {-0.05, -0.05}, snap);
    CHECK(r[0] == doctest::Approx(0.15));
    CHECK(r[3] == doctest::Approx(0.35));

    const auto o = overlap({0.0, 0.0, 0.55, 1.0}, {0.45, 0.0, 0.55, 1.0});
    REQUIRE(o);
    CHECK((*o)[0] == doctest::Approx(0.45));
    CHECK((*o)[2] == doctest::Approx(0.10));
    CHECK(!overlap({0.0, 0.0, 0.4, 1.0}, {0.5, 0.0, 0.5, 1.0}));
}

TEST_CASE("map geometry: the view fits the board and zooms around the cursor")
{
    const BoardRect board = boardBounds(5, false);
    const Viewport fit = fitted(board, 1000, 800);
    CHECK(fit.scale == doctest::Approx(1.0));
    const Point pixel{300, 200};
    const Point under = fit.toBoard(pixel);
    const Viewport zoomed = zoomedAround(fit, pixel, 2.0, 0.5, 10.0);
    CHECK(zoomed.scale == doctest::Approx(2.0));
    near(zoomed.toBoard(pixel), under);
    CHECK(zoomedAround(fit, pixel, 100.0, 0.5, 10.0).scale == doctest::Approx(10.0));
    const BoardRect frame = frameRect(4, false);
    CHECK(frame.x == doctest::Approx(360.0));
    CHECK(frame.y == doctest::Approx(280.0));
}
