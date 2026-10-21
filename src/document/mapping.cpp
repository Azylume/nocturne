#include "document/mapping_edits.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace azl {

namespace {

Point lerp(const Point& a, const Point& b, double t)
{
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t};
}

std::vector<Point> flatGrid(std::array<int, 2> size)
{
    std::vector<Point> grid;
    grid.reserve(size_t(size[0] * size[1]));
    for (int r = 0; r < size[1]; ++r)
        for (int c = 0; c < size[0]; ++c)
            grid.push_back({double(c) / (size[0] - 1), double(r) / (size[1] - 1)});
    return grid;
}

Point cubic(const Point* p, size_t stride, double s, bool derivative)
{
    const double t = 1.0 - s;
    using W = std::array<double, 4>;
    const W w = derivative ? W{-3 * t * t, 3 * t * t - 6 * s * t, 6 * s * t - 3 * s * s, 3 * s * s}
                           : W{t * t * t, 3 * s * t * t, 3 * s * s * t, s * s * s};
    Point out{0.0, 0.0};
    for (size_t i = 0; i < 4; ++i)
        for (size_t k = 0; k < 2; ++k)
            out[k] += w[i] * p[i * stride][k];
    return out;
}

// The point and the derivative by t at global parameter t of a piecewise cubic of `spans` spans, using `span`.
std::pair<Point, Point> evaluate(const std::vector<Point>& curve, int spans, int span, double t)
{
    const Point* p = &curve[size_t(span) * 3];
    const double s = t * spans - span;
    Point d = cubic(p, 1, s, true);
    return {cubic(p, 1, s, false), {d[0] * spans, d[1] * spans}};
}

// The same curve over `to` spans. Each new span is the Hermite segment between its ends, which is the exact sub-curve
// whenever it lies within one old span.
std::vector<Point> resample(const std::vector<Point>& curve, int to)
{
    const int from = int(curve.size() - 1) / 3;
    std::vector<Point> out;
    for (int k = 0; k < to; ++k) {
        const double t0 = double(k) / to, t1 = double(k + 1) / to;
        const int s0 = std::clamp(int(std::floor(t0 * from + 1e-9)), 0, from - 1);
        const int s1 = std::clamp(int(std::ceil(t1 * from - 1e-9)) - 1, 0, from - 1);
        const auto [p0, d0] = evaluate(curve, from, s0, t0);
        const auto [p3, d3] = evaluate(curve, from, s1, t1);
        const double third = (t1 - t0) / 3.0;
        if (k == 0)
            out.push_back(p0);
        out.push_back({p0[0] + d0[0] * third, p0[1] + d0[1] * third});
        out.push_back({p3[0] - d3[0] * third, p3[1] - d3[1] * third});
        out.push_back(p3);
    }
    return out;
}

void checkRange(double v, double min, double max, const char* what)
{
    if (!(v >= min && v <= max))
        throw ProtocolError(std::string(what) + " must lie within " + std::to_string(min) + ".." + std::to_string(max));
}

void checkPoints(const std::vector<Point>& points, const char* what)
{
    for (const Point& p : points)
        if (!(p[0] >= -1.0 && p[0] <= 2.0 && p[1] >= -1.0 && p[1] <= 2.0))
            throw ProtocolError(std::string(what) + " points must lie within -1..2");
}

void checkPatches(std::array<int, 2> patches)
{
    for (int p : patches)
        if (p < 1 || p > kMaxWarpPatches)
            throw ProtocolError("warp patches must be 1.." + std::to_string(kMaxWarpPatches));
}

void checkWarp(const Warp& warp)
{
    checkPatches(warp.patches);
    const auto size = warpGridSize(warp);
    if (!warp.points.empty() && warp.points.size() != size_t(size[0] * size[1]))
        throw ProtocolError("a warp of " + std::to_string(warp.patches[0]) + "x" + std::to_string(warp.patches[1]) +
                            " patches has " + std::to_string(size[0] * size[1]) + " points");
    checkPoints(warp.points, "warp");
}

void checkSoftEdge(const SoftEdge& s)
{
    for (double w : {s.left, s.right, s.top, s.bottom})
        checkRange(w, 0.0, 1.0, "soft edge widths");
    checkRange(s.power, 0.1, 10.0, "soft edge power");
    checkRange(s.luminance, 0.0, 1.0, "soft edge luminance");
    checkRange(s.gamma, 0.1, 10.0, "soft edge gamma");
}

void checkMask(const Mask& mask)
{
    if (mask.points.size() < 3 || mask.points.size() > kMaxMaskPoints)
        throw ProtocolError("a mask has 3.." + std::to_string(kMaxMaskPoints) + " points");
    checkPoints(mask.points, "mask");
}

Screen& screenById(Composition& composition, uint64_t id)
{
    for (Screen& screen : composition.screens)
        if (screen.id == id)
            return screen;
    throw ProtocolError("no screen " + std::to_string(id));
}

Slice& sliceById(Composition& composition, uint64_t id)
{
    for (Screen& screen : composition.screens)
        for (Slice& slice : screen.slices)
            if (slice.id == id)
                return slice;
    throw ProtocolError("no slice " + std::to_string(id));
}

template <typename C, typename F>
void forEachMaskList(C& composition, F&& f)
{
    for (auto& screen : composition.screens) {
        f(screen, 0, screen.masks);
        for (auto& slice : screen.slices)
            f(screen, slice.id, slice.masks);
    }
}

struct MaskPlace {
    uint64_t screen = 0;
    uint64_t slice = 0;
    std::vector<Mask>* masks = nullptr;
    size_t index = 0;
};

MaskPlace findMask(Composition& composition, uint64_t id)
{
    MaskPlace place;
    forEachMaskList(composition, [&](Screen& screen, uint64_t slice, std::vector<Mask>& masks) {
        for (size_t i = 0; i < masks.size(); ++i)
            if (masks[i].id == id)
                place = {screen.id, slice, &masks, i};
    });
    if (!place.masks)
        throw ProtocolError("no mask " + std::to_string(id));
    return place;
}

// Prototype rule: a side fades across the part of the rect that another rect of the same input covers past that side.
SoftEdge overlapWidths(const Composition& composition, const Slice& slice, std::vector<uint64_t>* partners)
{
    SoftEdge soft = slice.softEdge;
    soft.left = soft.right = soft.top = soft.bottom = 0.0;
    const auto [x, y, w, h] = slice.inputRect;
    for (const Screen& screen : composition.screens)
        for (const Slice& o : screen.slices) {
            if (o.id == slice.id || o.input != slice.input)
                continue;
            const auto [ox, oy, ow, oh] = o.inputRect;
            const bool vy = oy < y + h && oy + oh > y;
            const bool vx = ox < x + w && ox + ow > x;
            bool hit = false;
            auto fade = [&](double& side, bool overlaps, double width) {
                if (overlaps) {
                    side = std::max(side, width);
                    hit = true;
                }
            };
            fade(soft.right, vy && ox > x && ox < x + w && ox + ow > x + w, (x + w - ox) / w);
            fade(soft.left, vy && ox < x && ox + ow > x && ox + ow < x + w, (ox + ow - x) / w);
            fade(soft.bottom, vx && oy > y && oy < y + h && oy + oh > y + h, (y + h - oy) / h);
            fade(soft.top, vx && oy < y && oy + oh > y && oy + oh < y + h, (oy + oh - y) / h);
            if (hit && partners)
                partners->push_back(o.id);
        }
    return soft;
}

std::vector<SliceSoftEdge> fromOverlap(Composition& composition, uint64_t id)
{
    const Slice& slice = sliceById(composition, id);
    std::vector<uint64_t> partners;
    std::vector<SliceSoftEdge> edges{{id, overlapWidths(composition, slice, &partners)}};
    if (partners.empty())
        throw ProtocolError("no slice of the same input overlaps slice " + std::to_string(id));
    for (uint64_t p : partners)
        edges.push_back({p, overlapWidths(composition, sliceById(composition, p), nullptr)});
    return edges;
}

template <typename T>
T field(const nlohmann::json& j, const char* key)
{
    auto it = j.find(key);
    if (it == j.end())
        throw ProtocolError(std::string("missing field ") + key);
    try {
        return it->get<T>();
    } catch (const nlohmann::json::exception&) {
        throw ProtocolError(std::string("bad field ") + key);
    }
}

template <typename T>
std::optional<T> optionalField(const nlohmann::json& j, const char* key)
{
    if (!j.contains(key) || j[key].is_null())
        return std::nullopt;
    return field<T>(j, key);
}

std::vector<SliceSoftEdge> softEdgesField(const nlohmann::json& j)
{
    std::vector<SliceSoftEdge> edges;
    const auto list = j.value("slices", nlohmann::json::array());
    if (!list.is_array())
        throw ProtocolError("bad field slices");
    for (const nlohmann::json& e : list)
        edges.push_back({field<uint64_t>(e, "slice"), field<SoftEdge>(e, "softEdge")});
    return edges;
}

} // namespace

std::vector<Point> resolvedWarpGrid(const Warp& warp)
{
    const auto [cx, cy] = warpGridSize(warp);
    std::vector<Point> g = warp.points.size() == size_t(cx * cy) ? warp.points : flatGrid({cx, cy});
    auto at = [&](int c, int r) -> Point& { return g[size_t(r * cx + c)]; };
    if (!warp.bezier) {
        for (int r = 0; r < cy; r += 3)
            for (int c = 0; c + 3 < cx; c += 3) {
                at(c + 1, r) = lerp(at(c, r), at(c + 3, r), 1.0 / 3.0);
                at(c + 2, r) = lerp(at(c, r), at(c + 3, r), 2.0 / 3.0);
            }
        for (int c = 0; c < cx; c += 3)
            for (int r = 0; r + 3 < cy; r += 3) {
                at(c, r + 1) = lerp(at(c, r), at(c, r + 3), 1.0 / 3.0);
                at(c, r + 2) = lerp(at(c, r), at(c, r + 3), 2.0 / 3.0);
            }
    }
    // Coons patch: the blend of opposite edges in both directions, less the bilinear blend of the corners.
    for (int r0 = 0; r0 + 3 < cy; r0 += 3)
        for (int c0 = 0; c0 + 3 < cx; c0 += 3)
            for (int i = 1; i <= 2; ++i)
                for (int j = 1; j <= 2; ++j) {
                    const double u = i / 3.0, v = j / 3.0;
                    const Point top = at(c0 + i, r0), bottom = at(c0 + i, r0 + 3);
                    const Point left = at(c0, r0 + j), right = at(c0 + 3, r0 + j);
                    const Point p00 = at(c0, r0), p30 = at(c0 + 3, r0), p03 = at(c0, r0 + 3), p33 = at(c0 + 3, r0 + 3);
                    for (int k = 0; k < 2; ++k)
                        at(c0 + i, r0 + j)[k] = (1 - v) * top[k] + v * bottom[k] + (1 - u) * left[k] + u * right[k] -
                                                ((1 - u) * (1 - v) * p00[k] + u * (1 - v) * p30[k] +
                                                 (1 - u) * v * p03[k] + u * v * p33[k]);
                }
    return g;
}

Warp regridWarp(const Warp& warp, std::array<int, 2> patches)
{
    Warp out{patches, warp.bezier, {}};
    if (warp.points.empty())
        return out;
    if (patches == warp.patches)
        return warp;
    const auto [cx, cy] = warpGridSize(warp);
    const auto [nx, ny] = warpGridSize(out);
    const std::vector<Point> g = resolvedWarpGrid(warp);
    std::vector<Point> rows(size_t(nx * cy));
    for (int r = 0; r < cy; ++r) {
        const std::vector<Point> row(g.begin() + r * cx, g.begin() + (r + 1) * cx);
        const std::vector<Point> resampled = resample(row, patches[0]);
        std::copy(resampled.begin(), resampled.end(), rows.begin() + r * nx);
    }
    out.points.resize(size_t(nx * ny));
    for (int c = 0; c < nx; ++c) {
        std::vector<Point> column;
        for (int r = 0; r < cy; ++r)
            column.push_back(rows[size_t(r * nx + c)]);
        const std::vector<Point> resampled = resample(column, patches[1]);
        for (int r = 0; r < ny; ++r)
            out.points[size_t(r * nx + c)] = resampled[size_t(r)];
    }
    return out;
}

void checkSliceMapping(const Slice& slice)
{
    checkWarp(slice.warp);
    checkSoftEdge(slice.softEdge);
    checkColor(slice.color);
}

void checkColor(const ColorAdjust& c)
{
    for (double v : {c.brightness, c.contrast, c.red, c.green, c.blue})
        checkRange(v, -1.0, 1.0, "colour adjustments");
}

void checkMaskRoom(const std::vector<Mask>& masks, size_t adding)
{
    if (masks.size() + adding > kMaxMasks)
        throw ProtocolError("at most " + std::to_string(kMaxMasks) + " masks on a screen or slice");
}

void completeMasks(const Composition& composition, const std::vector<Mask*>& masks)
{
    std::set<uint64_t> used;
    uint64_t next = 1;
    auto reserve = [&](uint64_t id) {
        used.insert(id);
        next = std::max(next, id + 1);
    };
    forEachMaskList(composition, [&](const Screen&, uint64_t, const std::vector<Mask>& list) {
        for (const Mask& m : list)
            reserve(m.id);
    });
    for (Mask* mask : masks) {
        checkMask(*mask);
        if (mask->id == 0)
            continue;
        if (used.contains(mask->id))
            throw ProtocolError("mask id " + std::to_string(mask->id) + " is in use");
        reserve(mask->id);
    }
    for (Mask* mask : masks) {
        if (mask->id == 0)
            reserve(mask->id = next);
        if (mask->name.empty())
            mask->name = "Mask " + std::to_string(mask->id);
    }
}

Edit apply(Composition& composition, SetSliceWarp& edit)
{
    Slice& slice = sliceById(composition, edit.slice);
    Warp warp = slice.warp;
    if (edit.bezier)
        warp.bezier = *edit.bezier;
    if (edit.patches) {
        checkPatches(*edit.patches);
        warp = regridWarp(warp, *edit.patches);
    }
    if (edit.points)
        warp.points = *edit.points;
    checkWarp(warp);
    SetSliceWarp inverse{slice.id, slice.warp.bezier, slice.warp.patches, slice.warp.points};
    slice.warp = std::move(warp);
    return inverse;
}

Edit apply(Composition& composition, SetSoftEdges& edit)
{
    if (edit.fromOverlap) {
        edit.slices = fromOverlap(composition, edit.fromOverlap);
        edit.fromOverlap = 0;
    }
    SetSoftEdges inverse;
    for (const SliceSoftEdge& e : edit.slices) {
        checkSoftEdge(e.softEdge);
        inverse.slices.push_back({e.slice, sliceById(composition, e.slice).softEdge});
    }
    for (const SliceSoftEdge& e : edit.slices)
        sliceById(composition, e.slice).softEdge = e.softEdge;
    std::reverse(inverse.slices.begin(), inverse.slices.end());
    return inverse;
}

Edit apply(Composition& composition, SetSliceColor& edit)
{
    checkColor(edit.color);
    Slice& slice = sliceById(composition, edit.slice);
    SetSliceColor inverse{slice.id, slice.color};
    slice.color = edit.color;
    return inverse;
}

Edit apply(Composition& composition, SetScreenColor& edit)
{
    checkColor(edit.color);
    Screen& screen = screenById(composition, edit.screen);
    SetScreenColor inverse{screen.id, screen.color};
    screen.color = edit.color;
    return inverse;
}

Edit apply(Composition& composition, AddMask& edit)
{
    Screen& screen = screenById(composition, edit.screen);
    std::vector<Mask>* masks = &screen.masks;
    if (edit.slice) {
        auto it = std::find_if(screen.slices.begin(), screen.slices.end(),
                               [&](const Slice& s) { return s.id == edit.slice; });
        if (it == screen.slices.end())
            throw ProtocolError("no slice " + std::to_string(edit.slice) + " on screen " + std::to_string(screen.id));
        masks = &it->masks;
    }
    checkMaskRoom(*masks, 1);
    const size_t index = edit.index.value_or(masks->size());
    if (index > masks->size())
        throw ProtocolError("mask index out of range");
    Mask mask = edit.mask;
    completeMasks(composition, {&mask});
    masks->insert(masks->begin() + std::ptrdiff_t(index), mask);
    edit.index = index;
    edit.mask = std::move(mask);
    return RemoveMask{edit.mask.id};
}

Edit apply(Composition& composition, RemoveMask& edit)
{
    const MaskPlace p = findMask(composition, edit.mask);
    AddMask inverse{p.screen, p.slice, p.index, std::move((*p.masks)[p.index])};
    p.masks->erase(p.masks->begin() + std::ptrdiff_t(p.index));
    return inverse;
}

Edit apply(Composition& composition, SetMask& edit)
{
    const MaskPlace p = findMask(composition, edit.mask);
    Mask& mask = (*p.masks)[p.index];
    Mask changed = mask;
    if (edit.points)
        changed.points = *edit.points;
    if (edit.invert)
        changed.invert = *edit.invert;
    if (edit.name)
        changed.name = *edit.name;
    checkMask(changed);
    SetMask inverse{mask.id, std::nullopt, std::nullopt, std::nullopt};
    if (edit.points)
        inverse.points = mask.points;
    if (edit.invert)
        inverse.invert = mask.invert;
    if (edit.name)
        inverse.name = mask.name;
    mask = std::move(changed);
    return inverse;
}

Edit apply(Composition& composition, SetGuides& edit)
{
    if (edit.guides.size() > kMaxGuides)
        throw ProtocolError("at most " + std::to_string(kMaxGuides) + " guides");
    for (const Guide& g : edit.guides)
        checkRange(g.position, 0.0, 1.0, "guide positions");
    SetGuides inverse{composition.guides};
    composition.guides = edit.guides;
    return inverse;
}

nlohmann::json toJson(const SetSliceWarp& e)
{
    nlohmann::json j{{"op", "setSliceWarp"}, {"slice", e.slice}};
    if (e.bezier)
        j["bezier"] = *e.bezier;
    if (e.patches)
        j["patches"] = *e.patches;
    if (e.points)
        j["points"] = *e.points;
    return j;
}

nlohmann::json toJson(const SetSoftEdges& e)
{
    nlohmann::json slices = nlohmann::json::array();
    for (const SliceSoftEdge& s : e.slices)
        slices.push_back({{"slice", s.slice}, {"softEdge", s.softEdge}});
    nlohmann::json j{{"op", "setSoftEdges"}, {"slices", slices}};
    if (e.fromOverlap)
        j["fromOverlap"] = e.fromOverlap;
    return j;
}

nlohmann::json toJson(const SetSliceColor& e)
{
    return {{"op", "setSliceColor"}, {"slice", e.slice}, {"color", e.color}};
}

nlohmann::json toJson(const SetScreenColor& e)
{
    return {{"op", "setScreenColor"}, {"screen", e.screen}, {"color", e.color}};
}

nlohmann::json toJson(const AddMask& e)
{
    return {{"op", "addMask"},
            {"screen", e.screen},
            {"slice", e.slice},
            {"index", e.index ? nlohmann::json(*e.index) : nullptr},
            {"mask", e.mask}};
}

nlohmann::json toJson(const RemoveMask& e)
{
    return {{"op", "removeMask"}, {"mask", e.mask}};
}

nlohmann::json toJson(const SetMask& e)
{
    nlohmann::json j{{"op", "setMask"}, {"mask", e.mask}};
    if (e.points)
        j["points"] = *e.points;
    if (e.invert)
        j["invert"] = *e.invert;
    if (e.name)
        j["name"] = *e.name;
    return j;
}

nlohmann::json toJson(const SetGuides& e)
{
    return {{"op", "setGuides"}, {"guides", e.guides}};
}

std::optional<Edit> mappingEditFromJson(const std::string& op, const nlohmann::json& j)
{
    if (op == "setSliceWarp")
        return SetSliceWarp{field<uint64_t>(j, "slice"), optionalField<bool>(j, "bezier"),
                            optionalField<std::array<int, 2>>(j, "patches"),
                            optionalField<std::vector<Point>>(j, "points")};
    if (op == "setSoftEdges")
        return SetSoftEdges{softEdgesField(j), optionalField<uint64_t>(j, "fromOverlap").value_or(0)};
    if (op == "setSliceColor")
        return SetSliceColor{field<uint64_t>(j, "slice"), field<ColorAdjust>(j, "color")};
    if (op == "setScreenColor")
        return SetScreenColor{field<uint64_t>(j, "screen"), field<ColorAdjust>(j, "color")};
    if (op == "addMask")
        return AddMask{field<uint64_t>(j, "screen"), optionalField<uint64_t>(j, "slice").value_or(0),
                       optionalField<size_t>(j, "index"), field<Mask>(j, "mask")};
    if (op == "removeMask")
        return RemoveMask{field<uint64_t>(j, "mask")};
    if (op == "setMask")
        return SetMask{field<uint64_t>(j, "mask"), optionalField<std::vector<Point>>(j, "points"),
                       optionalField<bool>(j, "invert"), optionalField<std::string>(j, "name")};
    if (op == "setGuides")
        return SetGuides{field<std::vector<Guide>>(j, "guides")};
    return std::nullopt;
}

void to_json(nlohmann::json& j, const Warp& warp)
{
    j = {{"patches", warp.patches}, {"bezier", warp.bezier}, {"points", warp.points}};
}

void from_json(const nlohmann::json& j, Warp& warp)
{
    warp = Warp{};
    warp.patches = j.value("patches", warp.patches);
    warp.bezier = j.value("bezier", false);
    warp.points = j.value("points", std::vector<Point>{});
}

void to_json(nlohmann::json& j, const SoftEdge& s)
{
    j = {{"left", s.left},   {"right", s.right},         {"top", s.top},    {"bottom", s.bottom},
         {"power", s.power}, {"luminance", s.luminance}, {"gamma", s.gamma}};
}

void from_json(const nlohmann::json& j, SoftEdge& s)
{
    const SoftEdge d;
    s = {j.value("left", d.left),   j.value("right", d.right),         j.value("top", d.top),
         j.value("bottom", d.bottom), j.value("power", d.power), j.value("luminance", d.luminance),
         j.value("gamma", d.gamma)};
}

void to_json(nlohmann::json& j, const ColorAdjust& c)
{
    j = {{"brightness", c.brightness}, {"contrast", c.contrast}, {"red", c.red}, {"green", c.green}, {"blue", c.blue}};
}

void from_json(const nlohmann::json& j, ColorAdjust& c)
{
    c = {j.value("brightness", 0.0), j.value("contrast", 0.0), j.value("red", 0.0), j.value("green", 0.0),
         j.value("blue", 0.0)};
}

void to_json(nlohmann::json& j, const Mask& mask)
{
    j = {{"id", mask.id}, {"name", mask.name}, {"invert", mask.invert}, {"points", mask.points}};
}

void from_json(const nlohmann::json& j, Mask& mask)
{
    mask = {j.value("id", uint64_t(0)), j.value("name", std::string()), j.value("invert", false),
            j.value("points", std::vector<Point>{})};
}

void to_json(nlohmann::json& j, const Guide& guide)
{
    j = {{"axis", guide.vertical ? "x" : "y"}, {"position", guide.position}};
}

void from_json(const nlohmann::json& j, Guide& guide)
{
    const std::string axis = j.value("axis", std::string("x"));
    if (axis != "x" && axis != "y")
        throw ProtocolError("guide axis must be x or y");
    guide = {axis == "x", j.value("position", 0.5)};
}

} // namespace azl
