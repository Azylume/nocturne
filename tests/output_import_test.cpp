#include "effects/effect_registry.hpp"
#include "golden.hpp"
#include "import/resolume_import.hpp"
#include "test_client.hpp"

#include <doctest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace {

namespace fs = std::filesystem;

using P = std::array<double, 2>;
using M = std::array<double, 9>;

constexpr azl::EngineConfig kSize{.width = 480, .height = 270};
constexpr double kStillFps = 1e6;

fs::path fixture(const std::string& name)
{
    const char* dir = std::getenv("NOCTURNE_GOLDEN_DIR");
    return fs::path(dir ? dir : NOCTURNE_GOLDEN_DIR) / "import" / name;
}

std::string read(const fs::path& path)
{
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

struct Imported {
    std::vector<azl::Screen> screens;
    azl::ImportReport report;
};

Imported importText(const std::string& text)
{
    Imported out;
    out.screens = azl::importAdvancedOutput(*azl::parseXml(text), {}, out.report);
    return out;
}

Imported importFixture(const std::string& name)
{
    return importText(read(fixture(name)));
}

bool has(const std::vector<azl::ImportNote>& notes, const std::string& where, const std::string& what)
{
    for (const azl::ImportNote& n : notes)
        if (n.where == where && n.what.find(what) != std::string::npos)
            return true;
    return false;
}

// Numbers within 1e-9, so a snapshot holds on both architectures.
bool sameJson(const nlohmann::json& a, const nlohmann::json& b)
{
    if (a.is_number() && b.is_number())
        return std::abs(a.get<double>() - b.get<double>()) < 1e-9;
    if (a.type() != b.type() || a.size() != b.size())
        return false;
    if (a.is_array()) {
        for (size_t i = 0; i < a.size(); ++i)
            if (!sameJson(a[i], b[i]))
                return false;
        return true;
    }
    if (a.is_object()) {
        for (auto it = a.begin(); it != a.end(); ++it)
            if (!b.contains(it.key()) || !sameJson(it.value(), b[it.key()]))
                return false;
        return true;
    }
    return a == b;
}

nlohmann::json snapshot(const Imported& imported)
{
    auto list = [](const std::vector<azl::ImportNote>& in) {
        nlohmann::json out = nlohmann::json::array();
        for (const azl::ImportNote& n : in)
            out.push_back(n.where + ": " + n.what);
        return out;
    };
    return {{"screens", imported.screens},
            {"report",
             {{"mapped", imported.report.mapped},
              {"approximated", list(imported.report.approximated)},
              {"unsupported", list(imported.report.unsupported)},
              {"ignored", imported.report.ignored}}}};
}

// Resolume's slice geometry as saved, evaluated the way its warper draws it.
struct SourceSlice {
    int width = 4;
    int height = 4;
    bool bezier = false;
    std::vector<P> grid;
    M homography{1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::vector<P> input;
    int flip = 0;
    P output{1920, 1080};
};

std::vector<P> vertices(const azl::XmlNode* node)
{
    std::vector<P> out;
    for (const azl::XmlNode* v : node->all("v"))
        out.push_back({std::stod(v->attr("x")), std::stod(v->attr("y"))});
    return out;
}

M adjugate(const M& m)
{
    return {m[4] * m[8] - m[5] * m[7], m[2] * m[7] - m[1] * m[8], m[1] * m[5] - m[2] * m[4],
            m[5] * m[6] - m[3] * m[8], m[0] * m[8] - m[2] * m[6], m[2] * m[3] - m[0] * m[5],
            m[3] * m[7] - m[4] * m[6], m[1] * m[6] - m[0] * m[7], m[0] * m[4] - m[1] * m[3]};
}

M multiply(const M& a, const M& b)
{
    M m{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 3; ++k)
                m[size_t(r * 3 + c)] += a[size_t(r * 3 + k)] * b[size_t(k * 3 + c)];
    return m;
}

// Heckbert's closed form: the unit square onto a quad, corners in TL, TR, BR, BL order.
M squareToQuad(const std::vector<P>& q)
{
    const double dx1 = q[1][0] - q[2][0], dx2 = q[3][0] - q[2][0], sx = q[0][0] - q[1][0] + q[2][0] - q[3][0];
    const double dy1 = q[1][1] - q[2][1], dy2 = q[3][1] - q[2][1], sy = q[0][1] - q[1][1] + q[2][1] - q[3][1];
    const double den = dx1 * dy2 - dx2 * dy1;
    const double g = (sx * dy2 - dx2 * sy) / den, h = (dx1 * sy - sx * dy1) / den;
    return {q[1][0] - q[0][0] + g * q[1][0], q[3][0] - q[0][0] + h * q[3][0], q[0][0],
            q[1][1] - q[0][1] + g * q[1][1], q[3][1] - q[0][1] + h * q[3][1], q[0][1],
            g, h, 1};
}

P apply(const M& m, P p)
{
    const double w = m[6] * p[0] + m[7] * p[1] + m[8];
    return {(m[0] * p[0] + m[1] * p[1] + m[2]) / w, (m[3] * p[0] + m[4] * p[1] + m[5]) / w};
}

SourceSlice sourceSlice(const azl::XmlNode& root, size_t screen)
{
    const azl::XmlNode* s = root.at("ScreenSetup/screens")->children[screen].get();
    const azl::XmlNode* slice = s->at("layers/Slice");
    const azl::XmlNode* warper = slice->child("Warper");
    const azl::XmlNode* bezier = warper->child("BezierWarper");
    const azl::XmlNode* device = s->child("OutputDevice")->children.front().get();
    SourceSlice out;
    out.width = std::stoi(bezier->attr("controlWidth"));
    out.height = std::stoi(bezier->attr("controlHeight"));
    out.grid = vertices(bezier->child("vertices"));
    out.bezier = warper->child("Params")->child("ParamChoice")->attr("value") == "PM_BEZIER";
    out.homography = multiply(squareToQuad(vertices(warper->at("Homography/dst"))),
                              adjugate(squareToQuad(vertices(warper->at("Homography/src")))));
    out.input = vertices(slice->child("InputRect"));
    for (const auto& c : slice->child("Params", "Output")->children)
        if (c->attr("name") == "Flip")
            out.flip = std::stoi(c->attr("value"));
    out.output = {std::stod(device->attr("width")), std::stod(device->attr("height"))};
    return out;
}

// Edge handles at thirds in linear mode, then every interior handle from its patch's edges (Coons).
std::vector<P> resolveHandles(const SourceSlice& s)
{
    std::vector<P> g = s.grid;
    auto at = [&](int c, int r) -> P& { return g[size_t(r * s.width + c)]; };
    auto lerp = [](P a, P b, double t) { return P{a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t}; };
    if (!s.bezier) {
        for (int r = 0; r < s.height; r += 3)
            for (int c = 0; c + 3 < s.width; c += 3) {
                at(c + 1, r) = lerp(at(c, r), at(c + 3, r), 1 / 3.0);
                at(c + 2, r) = lerp(at(c, r), at(c + 3, r), 2 / 3.0);
            }
        for (int c = 0; c < s.width; c += 3)
            for (int r = 0; r + 3 < s.height; r += 3) {
                at(c, r + 1) = lerp(at(c, r), at(c, r + 3), 1 / 3.0);
                at(c, r + 2) = lerp(at(c, r), at(c, r + 3), 2 / 3.0);
            }
    }
    for (int r0 = 0; r0 + 3 < s.height; r0 += 3)
        for (int c0 = 0; c0 + 3 < s.width; c0 += 3)
            for (int i = 1; i <= 2; ++i)
                for (int j = 1; j <= 2; ++j) {
                    const double u = i / 3.0, v = j / 3.0;
                    for (int k = 0; k < 2; ++k)
                        at(c0 + i, r0 + j)[k] =
                            (1 - v) * at(c0 + i, r0)[k] + v * at(c0 + i, r0 + 3)[k] + (1 - u) * at(c0, r0 + j)[k] +
                            u * at(c0 + 3, r0 + j)[k] -
                            ((1 - u) * (1 - v) * at(c0, r0)[k] + u * (1 - v) * at(c0 + 3, r0)[k] +
                             (1 - u) * v * at(c0, r0 + 3)[k] + u * v * at(c0 + 3, r0 + 3)[k]);
                }
    return g;
}

// Where Resolume draws surface point (s, t) on the harness's output, in pixels.
P outputAt(const SourceSlice& src, const std::vector<P>& grid, double s, double t)
{
    const int px = (src.width - 1) / 3, py = (src.height - 1) / 3;
    const int i = std::clamp(int(s * px), 0, px - 1), j = std::clamp(int(t * py), 0, py - 1);
    const double u = s * px - i, v = t * py - j;
    auto bernstein = [](double x) {
        const double y = 1 - x;
        return std::array<double, 4>{y * y * y, 3 * x * y * y, 3 * x * x * y, x * x * x};
    };
    const auto bu = bernstein(u), bv = bernstein(v);
    P p{0, 0};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 2; ++k)
                p[k] += bu[size_t(c)] * bv[size_t(r)] * grid[size_t((j * 3 + r) * src.width + i * 3 + c)][k];
    const P screen = apply(src.homography, p);
    return {screen[0] / src.output[0] * kSize.width, screen[1] / src.output[1] * kSize.height};
}

// The composition UV Resolume shows at surface point (s, t).
P inputAt(const SourceSlice& src, double s, double t)
{
    if (src.flip & 1)
        s = 1 - s;
    if (src.flip & 2)
        t = 1 - t;
    const std::vector<P>& in = src.input;
    return {(in[0][0] + s * (in[1][0] - in[0][0]) + t * (in[3][0] - in[0][0])) / 1920.0,
            (in[0][1] + s * (in[1][1] - in[0][1]) + t * (in[3][1] - in[0][1])) / 1080.0};
}

void registerTestEffects()
{
    if (azl::findEffect("importGradient"))
        return;
    azl::registerEffect({.type = "importGradient",
                         .kind = azl::StageKind::Pointwise,
                         .metal = "\n    float2 uv = px / h.size;\n    return float4(uv, 0.5, 1.0);\n"});
    azl::registerEffect({.type = "importWhite", .kind = azl::StageKind::Pointwise, .metal = "\n    return float4(1.0);\n"});
}

azl::Composition onLayer(const char* effect, std::vector<azl::Screen> screens)
{
    registerTestEffects();
    for (azl::Screen& s : screens)
        s.display.clear();
    return {.layers = {{.id = 1, .name = effect, .effects = {{10, effect, {}}}}}, .screens = std::move(screens)};
}

struct Sampled {
    int checked = 0;
    int wrong = 0;
};

// Every surface point inside the slice shows the composition pixel Resolume would show there.
Sampled compareWithSource(const azl::Image& image, const SourceSlice& src)
{
    const std::vector<P> grid = resolveHandles(src);
    Sampled out;
    for (int j = 1; j < 20; ++j)
        for (int i = 1; i < 20; ++i) {
            const double s = i / 20.0, t = j / 20.0;
            const P at = outputAt(src, grid, s, t);
            if (at[0] < 1 || at[1] < 1 || at[0] >= kSize.width - 1 || at[1] >= kSize.height - 1)
                continue;
            const P uv = inputAt(src, s, t);
            const uint8_t* p = pixel(image, uint32_t(at[0]), uint32_t(at[1]));
            out.checked++;
            if (std::abs(p[0] - uv[0] * 255) > 4 || std::abs(p[1] - uv[1] * 255) > 4) {
                out.wrong++;
                MESSAGE("s " << s << " t " << t << " at " << at[0] << "," << at[1] << ": " << int(p[0]) << ","
                             << int(p[1]) << " expected " << uv[0] * 255 << "," << uv[1] * 255);
            }
        }
    return out;
}

double bourke(double x, double power, double luminance)
{
    return x < 0.5 ? luminance * std::pow(2 * x, power) : 1 - (1 - luminance) * std::pow(2 * (1 - x), power);
}

} // namespace

TEST_CASE("real presets and the derived fixture import to their expected snapshots")
{
    for (const std::string name : {"five-screens", "advanced-output", "mapped"}) {
        CAPTURE(name);
        const nlohmann::json actual = snapshot(importFixture(name + ".xml"));
        const fs::path expectedPath = fixture(name + ".expected.json");
        if (std::getenv("NOCTURNE_UPDATE_GOLDEN"))
            std::ofstream(expectedPath) << actual.dump(2) << "\n";
        CHECK_MESSAGE(sameJson(actual, nlohmann::json::parse(read(expectedPath))), actual.dump(2));
    }
}

TEST_CASE("AdvancedOutput.xml: the DMX screen and its fixture are reported with where they were")
{
    const Imported i = importFixture("advanced-output.xml");
    REQUIRE(i.screens.size() == 1);
    CHECK(i.screens[0].display == "VirtualScreen 1");
    CHECK(i.screens[0].slices[0].corners == std::array<double, 8>{0, 0, 2.4, 0, 2.4, 1.8, 0, 1.8});
    CHECK(has(i.report.unsupported, "screen setup / DMX screen Lumiverse 1", "Nocturne has no DMX output"));
    CHECK(has(i.report.unsupported, "screen setup / DMX screen Lumiverse 1 / DmxSlice '1 - 1806 LED 300'",
              "not imported"));
}

TEST_CASE("unknown elements anywhere in a preset are reported, never dropped")
{
    std::string text = read(fixture("five-screens.xml"));
    auto insertBefore = [&](const std::string& marker, const std::string& element) {
        const size_t at = text.find(marker);
        REQUIRE(at != std::string::npos);
        text.insert(at, element);
    };
    insertBefore("</layers>", "<Polygon><Params name=\"Common\"><Param name=\"Name\" value=\"Roof\"/></Params></Polygon>");
    insertBefore("<InputRect", "<InputMask/>");
    insertBefore("<BezierWarper", "<Lens/>");
    insertBefore("</screens>", "<Projector name=\"P1\"/>");
    insertBefore("<guides>", "<Params name=\"Extra\"><Param name=\"X\" default=\"0\" value=\"1\"/></Params>");
    const Imported i = importText(text);
    CHECK(has(i.report.unsupported, "screen Inner left", "Polygon 'Roof' not imported"));
    CHECK(has(i.report.unsupported, "screen Inner left / slice Slice", "element InputMask not imported"));
    CHECK(has(i.report.unsupported, "screen Inner left / slice Slice / warper", "element Lens not imported"));
    CHECK(has(i.report.unsupported, "screen setup", "element Projector 'P1' not imported"));
    CHECK(has(i.report.unsupported, "screen Inner left", "element Params 'Extra' not imported"));
    CHECK(i.screens.size() == 5);
}

TEST_CASE("warps, corner pins, turned input rects, flip, soft edges and colour map onto screens and slices")
{
    const Imported i = importFixture("mapped.xml");
    REQUIRE(i.screens.size() == 6);
    std::set<uint64_t> ids;
    for (const azl::Screen& s : i.screens)
        for (const azl::Slice& slice : s.slices)
            ids.insert(slice.id);
    CHECK(ids.size() == 6);

    const azl::Slice& warp = i.screens[0].slices[0];
    CHECK(warp.warp.bezier);
    CHECK(warp.warp.patches == std::array<int, 2>{2, 1});
    CHECK(warp.warp.points.size() == 28);

    const azl::Slice& left = i.screens[1].slices[0];
    const azl::Slice& right = i.screens[2].slices[0];
    CHECK(left.softEdge.right == doctest::Approx(1.0 / 3.0));
    CHECK(right.softEdge.left == doctest::Approx(1.0 / 3.0));
    CHECK(left.softEdge.power == 3.0);
    CHECK(left.softEdge.luminance == 0.5);
    CHECK(left.softEdge.gamma == doctest::Approx(2.2));

    const azl::Slice& turned = i.screens[3].slices[0];
    REQUIRE(turned.masks.size() == 1);
    CHECK(turned.masks[0].invert);
    CHECK(has(i.report.approximated, "screen Turned / slice Slice", "input rect turned 30 degrees"));

    const azl::Slice& quarter = i.screens[4].slices[0];
    CHECK(quarter.inputRect[2] == doctest::Approx(400.0 / 1920.0));
    CHECK(quarter.softEdge == azl::SoftEdge{});
    CHECK(has(i.report.approximated, "screen Quarter / slice Slice", "soft edge off: its input rect is turned"));

    const azl::Screen& colour = i.screens[5];
    CHECK(colour.color == azl::ColorAdjust{0.05, 0.1, 0.0, 0.15, 0.0});
    CHECK(colour.slices[0].color == azl::ColorAdjust{0.1, 0.0, -0.2, 0.0, 0.3});
    CHECK(i.report.unsupported.empty());
}

TEST_CASE("golden frames: imported warps, corner pins and turned input rects render the source geometry")
{
    const std::string text = read(fixture("mapped.xml"));
    const auto root = azl::parseXml(text);
    const Imported i = importText(text);
    azl::EngineHarness harness(kSize, kStillFps, {}, onLayer("importGradient", i.screens));
    harness.step();
    for (const auto& [index, golden] : std::vector<std::pair<size_t, std::string>>{
             {0, "import_warp"}, {3, "import_turned"}, {4, "import_quarter"}}) {
        CAPTURE(golden);
        const azl::Image image = harness.readScreen(i.screens[index].id);
        const SourceSlice src = sourceSlice(*root, index);
        const Sampled sampled = compareWithSource(image, src);
        CHECK(sampled.checked > 150);
        CHECK(sampled.wrong == 0);
        checkGolden(image, golden);
    }

    // Outside the turned rect but inside its bounding box the mask leaves black, as Resolume shows nothing there.
    const SourceSlice turned = sourceSlice(*root, 3);
    const azl::Image image = harness.readScreen(i.screens[3].id);
    const std::vector<P> grid = resolveHandles(turned);
    int black = 0;
    for (const P st : {P{-0.06, 0.5}, P{1.06, 0.5}, P{0.5, -0.06}, P{0.5, 1.06}, P{-0.06, 0.2}, P{1.06, 0.8}}) {
        const P at = outputAt(turned, grid, st[0], st[1]);
        REQUIRE(at[0] >= 0);
        REQUIRE(at[1] >= 0);
        const uint8_t* p = pixel(image, uint32_t(at[0]), uint32_t(at[1]));
        black += p[0] < 3 && p[1] < 3 && p[2] < 3;
    }
    CHECK(black == 6);
}

TEST_CASE("golden frame: an imported soft edge follows the source's power, luminance and gamma")
{
    const Imported i = importFixture("mapped.xml");
    azl::EngineHarness harness(kSize, kStillFps, {}, onLayer("importWhite", {i.screens[1], i.screens[2]}));
    harness.step();
    const azl::Image left = harness.readScreen(i.screens[1].id);
    const azl::Image right = harness.readScreen(i.screens[2].id);
    // Left shows composition x 0..1152 and right 768..1920 of 1920, each across 480 output pixels.
    const double power = 3.0, luminance = 0.5, gamma = 2.0 * 1.1;
    int checked = 0;
    for (uint32_t x = 322; x < 478; x += 6) {
        const double comp = (x + 0.5) / 480.0 * 1152.0;
        const double expected = 255.0 * std::pow(bourke((1152.0 - comp) / 384.0, power, luminance), 1.0 / gamma);
        CHECK(std::abs(pixel(left, x, 135)[0] - expected) <= 3);
        const double mirrored = 255.0 * std::pow(bourke((comp - 768.0) / 384.0, power, luminance), 1.0 / gamma);
        CHECK(std::abs(pixel(right, x - 320, 135)[0] - mirrored) <= 3);
        CHECK(std::abs(std::pow(pixel(left, x, 135)[0] / 255.0, gamma) +
                       std::pow(pixel(right, x - 320, 135)[0] / 255.0, gamma) - 1.0) < 0.03);
        checked++;
    }
    CHECK(checked > 20);
    CHECK(pixel(left, 100, 135)[0] == 255);
    CHECK(pixel(right, 400, 135)[0] == 255);
    checkGolden(left, "import_soft_edge");
}
