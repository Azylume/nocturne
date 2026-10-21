#include "ui/app/map_geometry.hpp"

#include "document/mapping.hpp"

#include <algorithm>
#include <cmath>

namespace azl::ui {

namespace {

constexpr double kPi = 3.14159265358979323846;

Point bezier(const Point& a, const Point& b, const Point& c, const Point& d, double t)
{
    const double s = 1.0 - t;
    Point p;
    for (int k = 0; k < 2; ++k)
        p[k] = s * s * s * a[k] + 3 * s * s * t * b[k] + 3 * s * t * t * c[k] + t * t * t * d[k];
    return p;
}

// Appends the cubic Bezier runs through `chain` (3n + 1 points), without its last point.
void appendRuns(Points& out, const Points& chain, int samples)
{
    for (size_t i = 0; i + 3 < chain.size(); i += 3)
        for (int s = 0; s < samples; ++s)
            out.push_back(bezier(chain[i], chain[i + 1], chain[i + 2], chain[i + 3], double(s) / samples));
}

} // namespace

Points cornersOf(const Slice& slice)
{
    const auto& c = slice.corners;
    return {{c[0], c[1]}, {c[2], c[3]}, {c[4], c[5]}, {c[6], c[7]}};
}

std::array<double, 8> cornerArray(const Points& points)
{
    std::array<double, 8> out{};
    for (size_t i = 0; i < 4 && i < points.size(); ++i) {
        out[i * 2] = points[i][0];
        out[i * 2 + 1] = points[i][1];
    }
    return out;
}

Point centroid(const Points& points)
{
    Point c{0.0, 0.0};
    for (const Point& p : points) {
        c[0] += p[0];
        c[1] += p[1];
    }
    const double n = std::max<size_t>(1, points.size());
    return {c[0] / n, c[1] / n};
}

Points transformed(const Points& base, double dx, double dy, double scale, double radians, double aspect)
{
    const Point c = centroid(base);
    const double cs = std::cos(radians), sn = std::sin(radians);
    Points out;
    for (const Point& p : base) {
        const double u = (p[0] - c[0]) * scale * aspect, v = (p[1] - c[1]) * scale;
        out.push_back({c[0] + (u * cs - v * sn) / aspect + dx, c[1] + u * sn + v * cs + dy});
    }
    return out;
}

Points movedEdge(const Points& base, int edge, double dx, double dy)
{
    Points out = base;
    for (size_t k : {size_t(edge), size_t(edge + 1) % base.size()}) {
        out[k][0] += dx;
        out[k][1] += dy;
    }
    return out;
}

double sweep(Point centre, Point from, Point to, bool snap)
{
    double r = std::atan2(to[1] - centre[1], to[0] - centre[0]) - std::atan2(from[1] - centre[1], from[0] - centre[0]);
    r = std::remainder(r, 2 * kPi);
    if (snap)
        r = std::round(r / (kPi / 12)) * (kPi / 12);
    return r;
}

Points clamped(Points points)
{
    for (Point& p : points)
        for (double& v : p)
            v = std::clamp(v, -1.0, 2.0);
    return points;
}

Point rotateHandle(const Points& points, double reach)
{
    const Point c = centroid(points);
    const Point m{(points[0][0] + points[1][0]) / 2, (points[0][1] + points[1][1]) / 2};
    const double dx = m[0] - c[0], dy = m[1] - c[1];
    const double len = std::hypot(dx, dy);
    if (len < 1e-9)
        return {m[0], m[1] - reach};
    return {m[0] + dx / len * reach, m[1] + dy / len * reach};
}

Homography::Homography(const Points& q)
{
    const double x0 = q[0][0], y0 = q[0][1], x1 = q[1][0], y1 = q[1][1];
    const double x2 = q[2][0], y2 = q[2][1], x3 = q[3][0], y3 = q[3][1];
    const double dx1 = x1 - x2, dx2 = x3 - x2, dy1 = y1 - y2, dy2 = y3 - y2;
    const double sx = x0 - x1 + x2 - x3, sy = y0 - y1 + y2 - y3;
    double g = 0.0, h = 0.0;
    const double den = dx1 * dy2 - dx2 * dy1;
    if ((sx != 0.0 || sy != 0.0) && std::abs(den) > 1e-12) {
        g = (sx * dy2 - dx2 * sy) / den;
        h = (dx1 * sy - sx * dy1) / den;
    }
    m_ = {x1 - x0 + g * x1, x3 - x0 + h * x3, x0, y1 - y0 + g * y1, y3 - y0 + h * y3, y0, g, h, 1.0};
}

Point Homography::map(Point uv) const
{
    const auto& m = m_;
    const double w = m[6] * uv[0] + m[7] * uv[1] + m[8];
    return {(m[0] * uv[0] + m[1] * uv[1] + m[2]) / w, (m[3] * uv[0] + m[4] * uv[1] + m[5]) / w};
}

std::optional<Point> Homography::unmap(Point p) const
{
    const auto& m = m_;
    const double a = m[4] * m[8] - m[5] * m[7], b = m[2] * m[7] - m[1] * m[8], c = m[1] * m[5] - m[2] * m[4];
    const double d = m[5] * m[6] - m[3] * m[8], e = m[0] * m[8] - m[2] * m[6], f = m[2] * m[3] - m[0] * m[5];
    const double g = m[3] * m[7] - m[4] * m[6], h = m[1] * m[6] - m[0] * m[7], i = m[0] * m[4] - m[1] * m[3];
    const double w = g * p[0] + h * p[1] + i;
    if (std::abs(w) < 1e-12)
        return std::nullopt;
    return Point{(a * p[0] + b * p[1] + c) / w, (d * p[0] + e * p[1] + f) / w};
}

Points sliceOutline(const Slice& slice, int samples)
{
    const Homography pin(cornersOf(slice));
    if (slice.warp.points.empty())
        return cornersOf(slice);
    const auto [cx, cy] = warpGridSize(slice.warp);
    const Points g = resolvedWarpGrid(slice.warp);
    auto at = [&](int c, int r) { return g[size_t(r * cx + c)]; };
    Points top, right, bottom, left;
    for (int c = 0; c < cx; ++c) {
        top.push_back(at(c, 0));
        bottom.push_back(at(cx - 1 - c, cy - 1));
    }
    for (int r = 0; r < cy; ++r) {
        right.push_back(at(cx - 1, r));
        left.push_back(at(0, cy - 1 - r));
    }
    Points unit;
    for (const Points* chain : {&top, &right, &bottom, &left})
        appendRuns(unit, *chain, samples);
    Points out;
    for (const Point& p : unit)
        out.push_back(pin.map(p));
    return out;
}

namespace {

std::array<double, 4> bernstein(double t)
{
    const double s = 1.0 - t;
    return {s * s * s, 3 * s * s * t, 3 * s * t * t, t * t * t};
}

bool warped(const Warp& warp)
{
    const auto [cx, cy] = warpGridSize(warp);
    return warp.points.size() == size_t(cx * cy);
}

} // namespace

Point warpSurfaceAt(const Warp& warp, Point uv)
{
    if (!warped(warp))
        return uv;
    const auto [cx, cy] = warpGridSize(warp);
    const Points g = resolvedWarpGrid(warp);
    const int px = warp.patches[0], py = warp.patches[1];
    const double x = uv[0] * px, y = uv[1] * py;
    const int i = std::clamp(int(std::floor(x)), 0, px - 1), j = std::clamp(int(std::floor(y)), 0, py - 1);
    const auto wx = bernstein(x - i), wy = bernstein(y - j);
    Point p{0.0, 0.0};
    for (int b = 0; b < 4; ++b)
        for (int a = 0; a < 4; ++a) {
            const Point& q = g[size_t((3 * j + b) * cx + 3 * i + a)];
            p[0] += q[0] * wx[size_t(a)] * wy[size_t(b)];
            p[1] += q[1] * wx[size_t(a)] * wy[size_t(b)];
        }
    return p;
}

Point sliceToScreen(const Slice& slice, Point input)
{
    const auto& r = slice.inputRect;
    const Point unit{(input[0] - r[0]) / r[2], (input[1] - r[1]) / r[3]};
    return Homography(cornersOf(slice)).map(warpSurfaceAt(slice.warp, unit));
}

std::optional<Point> screenToSlice(const Slice& slice, Point screen, Point guess)
{
    Point p = guess;
    constexpr double h = 1e-6;
    for (int iteration = 0; iteration < 30; ++iteration) {
        const Point f = sliceToScreen(slice, p);
        const double ex = f[0] - screen[0], ey = f[1] - screen[1];
        if (std::abs(ex) < 1e-12 && std::abs(ey) < 1e-12)
            return p;
        const Point fx = sliceToScreen(slice, {p[0] + h, p[1]}), fy = sliceToScreen(slice, {p[0], p[1] + h});
        const double a = (fx[0] - f[0]) / h, b = (fy[0] - f[0]) / h, c = (fx[1] - f[1]) / h, d = (fy[1] - f[1]) / h;
        const double det = a * d - b * c;
        if (std::abs(det) < 1e-14)
            return std::nullopt;
        p[0] -= (d * ex - b * ey) / det;
        p[1] -= (-c * ex + a * ey) / det;
    }
    const Point f = sliceToScreen(slice, p);
    if (std::hypot(f[0] - screen[0], f[1] - screen[1]) > 1e-7)
        return std::nullopt;
    return p;
}

Warp interpolatingWarp(int nx, int ny, const Points& anchors)
{
    Warp warp{{nx - 1, ny - 1}, true, {}};
    const auto [cx, cy] = warpGridSize(warp);
    Points g(size_t(cx * cy), Point{0.0, 0.0});
    auto at = [&](int c, int r) -> Point& { return g[size_t(r * cx + c)]; };
    // Catmull-Rom handles along a run of anchors, the ends reflected.
    auto run = [](const std::vector<Point>& p, size_t i, int side) {
        const size_t n = p.size();
        auto ext = [&](long k) {
            if (k < 0)
                return Point{2 * p[0][0] - p[1][0], 2 * p[0][1] - p[1][1]};
            if (k >= long(n))
                return Point{2 * p[n - 1][0] - p[n - 2][0], 2 * p[n - 1][1] - p[n - 2][1]};
            return p[size_t(k)];
        };
        const long k = long(i);
        if (side == 1) {
            const Point a = ext(k - 1), b = ext(k + 1);
            return Point{p[i][0] + (b[0] - a[0]) / 6, p[i][1] + (b[1] - a[1]) / 6};
        }
        const Point a = ext(k), b = ext(k + 2);
        return Point{p[i + 1][0] - (b[0] - a[0]) / 6, p[i + 1][1] - (b[1] - a[1]) / 6};
    };
    for (int j = 0; j < ny; ++j) {
        std::vector<Point> row;
        for (int i = 0; i < nx; ++i)
            row.push_back(anchors[size_t(j * nx + i)]);
        for (int i = 0; i < nx; ++i)
            at(3 * i, 3 * j) = row[size_t(i)];
        for (int i = 0; i + 1 < nx; ++i) {
            at(3 * i + 1, 3 * j) = run(row, size_t(i), 1);
            at(3 * i + 2, 3 * j) = run(row, size_t(i), 2);
        }
    }
    for (int i = 0; i < nx; ++i) {
        std::vector<Point> column;
        for (int j = 0; j < ny; ++j)
            column.push_back(anchors[size_t(j * nx + i)]);
        for (int j = 0; j + 1 < ny; ++j) {
            at(3 * i, 3 * j + 1) = run(column, size_t(j), 1);
            at(3 * i, 3 * j + 2) = run(column, size_t(j), 2);
        }
    }
    // Interior handles are derived by the engine; these only keep the grid in range.
    for (int r = 0; r < cy; ++r)
        for (int c = 0; c < cx; ++c)
            if (r % 3 != 0 && c % 3 != 0)
                at(c, r) = at(c - c % 3, r - r % 3);
    warp.points = clamped(std::move(g));
    return warp;
}

namespace {

// Grid indices (4x4) of the 12 corner-warp points.
constexpr int kCornerGrid[12] = {0, 3, 15, 12, 1, 2, 7, 11, 14, 13, 8, 4};

} // namespace

bool isCornerWarp(const Warp& warp)
{
    return warp.bezier && warp.patches == std::array<int, 2>{1, 1} && warp.points.size() == 16;
}

Warp cornerWarp(const Points& points)
{
    Warp warp{{1, 1}, true, Points(16, Point{0.0, 0.0})};
    for (int k = 0; k < 12; ++k)
        warp.points[size_t(kCornerGrid[k])] = points[size_t(k)];
    for (int index : {5, 6, 9, 10})
        warp.points[size_t(index)] = points[0];
    warp.points = clamped(std::move(warp.points));
    return warp;
}

Points cornerPointsOf(const Slice& slice)
{
    if (!isCornerWarp(slice.warp))
        return {};
    const Homography pin(cornersOf(slice));
    Points out;
    for (int index : kCornerGrid)
        out.push_back(pin.map(slice.warp.points[size_t(index)]));
    return out;
}

Points sampledCornerPoints(const Slice& slice)
{
    const Homography pin(cornersOf(slice));
    auto at = [&](double u, double v) { return pin.map(warpSurfaceAt(slice.warp, {u, v})); };
    const Point tl = at(0, 0), tr = at(1, 0), br = at(1, 1), bl = at(0, 1);
    auto third = [](Point a, Point b, double t) { return Point{a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t}; };
    return {tl, tr, br, bl, third(tl, tr, 1.0 / 3), third(tl, tr, 2.0 / 3), third(tr, br, 1.0 / 3), third(tr, br, 2.0 / 3),
            third(br, bl, 1.0 / 3), third(br, bl, 2.0 / 3), third(bl, tl, 1.0 / 3), third(bl, tl, 2.0 / 3)};
}

std::array<int, 2> cornerHandles(int corner)
{
    static constexpr std::array<int, 2> handles[4] = {{4, 11}, {5, 6}, {7, 8}, {9, 10}};
    return handles[corner];
}

Points movedCornerPoint(const Points& points, int index, Point delta)
{
    Points out = points;
    std::vector<int> moved{index};
    if (index < 4)
        for (int h : cornerHandles(index))
            moved.push_back(h);
    for (int k : moved) {
        out[size_t(k)][0] += delta[0];
        out[size_t(k)][1] += delta[1];
    }
    return out;
}

std::array<int, 2> warpAnchors(const Slice& slice)
{
    if (!warped(slice.warp) || isCornerWarp(slice.warp))
        return {0, 0};
    return {slice.warp.patches[0] + 1, slice.warp.patches[1] + 1};
}

Points anchorsOf(const Slice& slice)
{
    const auto [nx, ny] = warpAnchors(slice);
    if (!nx)
        return {};
    const auto [cx, cy] = warpGridSize(slice.warp);
    const Points g = resolvedWarpGrid(slice.warp);
    const Homography pin(cornersOf(slice));
    Points out;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i)
            out.push_back(pin.map(g[size_t(3 * j * cx + 3 * i)]));
    return out;
}

Points sampledAnchors(const Slice& slice, int nx, int ny)
{
    const Homography pin(cornersOf(slice));
    Points out;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i)
            out.push_back(pin.map(warpSurfaceAt(slice.warp, {double(i) / (nx - 1), double(j) / (ny - 1)})));
    return out;
}

Points outerCorners(const Points& anchors, int nx, int ny)
{
    return {anchors[0], anchors[size_t(nx - 1)], anchors[size_t(nx * ny - 1)], anchors[size_t(nx * (ny - 1))]};
}

Slice withShape(const Slice& slice, const Points& shape)
{
    Slice out = slice;
    if (isCornerWarp(slice.warp)) {
        out.corners = {0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0};
        out.warp = cornerWarp(clamped(shape));
        return out;
    }
    const auto [nx, ny] = warpAnchors(slice);
    if (!nx) {
        out.corners = cornerArray(clamped(shape));
        return out;
    }
    out.corners = {0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0};
    out.warp = interpolatingWarp(nx, ny, clamped(shape));
    return out;
}

std::vector<Points> warpCurves(const Slice& slice, int samples)
{
    std::vector<Points> out;
    if (!warped(slice.warp))
        return out;
    const auto [cx, cy] = warpGridSize(slice.warp);
    const Points g = resolvedWarpGrid(slice.warp);
    const Homography pin(cornersOf(slice));
    auto curve = [&](Points chain) {
        Points unit;
        appendRuns(unit, chain, samples);
        unit.push_back(chain.back());
        Points line;
        for (const Point& p : unit)
            line.push_back(pin.map(p));
        out.push_back(std::move(line));
    };
    for (int r = 0; r < cy; r += 3) {
        Points chain;
        for (int c = 0; c < cx; ++c)
            chain.push_back(g[size_t(r * cx + c)]);
        curve(chain);
    }
    for (int c = 0; c < cx; c += 3) {
        Points chain;
        for (int r = 0; r < cy; ++r)
            chain.push_back(g[size_t(r * cx + c)]);
        curve(chain);
    }
    return out;
}

bool inside(const Points& polygon, Point p)
{
    bool in = false;
    for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const Point& a = polygon[i];
        const Point& b = polygon[j];
        if ((a[1] > p[1]) != (b[1] > p[1]) && p[0] < (b[0] - a[0]) * (p[1] - a[1]) / (b[1] - a[1]) + a[0])
            in = !in;
    }
    return in;
}

Points transformBox(const LayerTransform& t, double aspect)
{
    const double a = t.rotation * kPi / 180.0, k = std::cos(a), s = std::sin(a);
    const Point c{0.5 + t.x, 0.5 - t.y};
    Points out;
    for (Point corner : {Point{-0.5, -0.5}, Point{0.5, -0.5}, Point{0.5, 0.5}, Point{-0.5, 0.5}}) {
        const double x = corner[0] * aspect * t.scale, y = corner[1] * t.scale;
        out.push_back({c[0] + (k * x + s * y) / aspect, c[1] - s * x + k * y});
    }
    return out;
}

Points textBox(const TextClip& text, double aspect)
{
    size_t lines = 1, longest = 0, run = 0;
    for (char ch : text.text) {
        if (ch == '\n') {
            ++lines;
            run = 0;
        } else if ((ch & 0xc0) != 0x80) {
            longest = std::max(longest, ++run);
        }
    }
    const double h = text.size * text.lineSpacing * double(lines);
    const double w = std::max(text.size, text.size * 0.6 * double(longest)) / aspect;
    const double left = text.align == TextAlign::Left ? text.x : text.align == TextAlign::Right ? text.x - w : text.x - w / 2;
    const double top = text.y - h / 2;
    return {{left, top}, {left + w, top}, {left + w, top + h}, {left, top + h}};
}

namespace {

double snapped(double v, const std::vector<double>& edges, double reach)
{
    double best = reach;
    double out = v;
    for (double e : edges)
        if (std::abs(e - v) < best) {
            best = std::abs(e - v);
            out = e;
        }
    return out;
}

// Moves the span [a, a + size] so that whichever end lies nearer an edge lands on it.
double snappedSpan(double a, double size, std::vector<double> edges, double reach)
{
    const double lo = snapped(a, edges, reach), hi = snapped(a + size, edges, reach);
    if (lo == a && hi == a + size)
        return a;
    if (lo != a && (hi == a + size || std::abs(lo - a) <= std::abs(hi - a - size)))
        return lo;
    return hi - size;
}

} // namespace

std::array<double, 4> draggedRect(const std::array<double, 4>& base, int handle, Point delta, const InputSnap& snap)
{
    constexpr double kMin = 0.01;
    auto [bx, by, bw, bh] = base;
    std::vector<double> xs = snap.edgesX, ys = snap.edgesY;
    for (std::vector<double>* edges : {&xs, &ys}) {
        edges->push_back(0.0);
        edges->push_back(1.0);
    }
    const double rx = snap.snap ? snap.reach[0] : 0.0, ry = snap.snap ? snap.reach[1] : 0.0;
    if (handle < 0) {
        double x = std::clamp(bx + delta[0], 0.0, 1.0 - bw), y = std::clamp(by + delta[1], 0.0, 1.0 - bh);
        x = std::clamp(snappedSpan(x, bw, xs, rx), 0.0, 1.0 - bw);
        y = std::clamp(snappedSpan(y, bh, ys, ry), 0.0, 1.0 - bh);
        return {x, y, bw, bh};
    }
    double x0 = bx, y0 = by, x1 = bx + bw, y1 = by + bh;
    const bool left = handle == 0 || handle == 3 || handle == 7, right = handle == 1 || handle == 2 || handle == 5;
    const bool top = handle == 0 || handle == 1 || handle == 4, bottom = handle == 2 || handle == 3 || handle == 6;
    if (left)
        x0 = std::clamp(snapped(bx + delta[0], xs, rx), 0.0, x1 - kMin);
    if (right)
        x1 = std::clamp(snapped(bx + bw + delta[0], xs, rx), x0 + kMin, 1.0);
    if (top)
        y0 = std::clamp(snapped(by + delta[1], ys, ry), 0.0, y1 - kMin);
    if (bottom)
        y1 = std::clamp(snapped(by + bh + delta[1], ys, ry), y0 + kMin, 1.0);
    return {x0, y0, x1 - x0, y1 - y0};
}

std::optional<std::array<double, 4>> overlap(const std::array<double, 4>& a, const std::array<double, 4>& b)
{
    const double x0 = std::max(a[0], b[0]), y0 = std::max(a[1], b[1]);
    const double x1 = std::min(a[0] + a[2], b[0] + b[2]), y1 = std::min(a[1] + a[3], b[1] + b[3]);
    if (x1 <= x0 || y1 <= y0)
        return std::nullopt;
    return std::array<double, 4>{x0, y0, x1 - x0, y1 - y0};
}

BoardRect frameRect(int index, bool focused)
{
    if (focused)
        return {12.0, 24.0, kFrameW, kFrameH};
    return {40.0 + (index % 3) * 320.0, 50.0 + (index / 3) * 230.0, kFrameW, kFrameH};
}

BoardRect boardBounds(int screens, bool focused)
{
    if (focused)
        return {0.0, 0.0, kFrameW + 24.0, kFrameH + 36.0};
    const int rows = std::max(1, (screens + 2) / 3);
    return {0.0, 0.0, 1000.0, 80.0 + rows * 230.0};
}

BoardRect mapFrame(const std::vector<Screen>& screens, uint64_t screen)
{
    int index = 0;
    for (const Screen& s : screens) {
        if (s.id == screen && s.mapPosition)
            return {(*s.mapPosition)[0], (*s.mapPosition)[1], kFrameW, kFrameH};
        if (s.id == screen)
            return frameRect(index, false);
        if (!s.hidden)
            index++;
    }
    return {};
}

BoardRect mapBounds(const std::vector<Screen>& screens)
{
    const auto shown = std::count_if(screens.begin(), screens.end(), [](const Screen& s) { return !s.hidden; });
    BoardRect b = boardBounds(int(shown), false);
    for (const Screen& s : screens) {
        if (s.hidden || !s.mapPosition)
            continue;
        const BoardRect f = mapFrame(screens, s.id);
        const double x0 = std::min(b.x, f.x - 40.0), y0 = std::min(b.y, f.y - 50.0);
        const double x1 = std::max(b.x + b.w, f.x + f.w + 40.0), y1 = std::max(b.y + b.h, f.y + f.h + 30.0);
        b = {x0, y0, x1 - x0, y1 - y0};
    }
    return b;
}

Viewport fitted(const BoardRect& board, double width, double height)
{
    Viewport v;
    v.scale = std::max(1e-6, std::min(width / board.w, height / board.h));
    v.offset = {(width - board.w * v.scale) / 2 - board.x * v.scale, (height - board.h * v.scale) / 2 - board.y * v.scale};
    return v;
}

Viewport zoomedAround(const Viewport& view, Point pixel, double factor, double minScale, double maxScale)
{
    const Point b = view.toBoard(pixel);
    Viewport v;
    v.scale = std::clamp(view.scale * factor, minScale, maxScale);
    v.offset = {pixel[0] - b[0] * v.scale, pixel[1] - b[1] * v.scale};
    return v;
}

} // namespace azl::ui
