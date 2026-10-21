#include "document/mapping_edits.hpp"
#include "effects/effect_registry.hpp"
#include "golden.hpp"
#include "test_client.hpp"

#include <doctest.h>

#include <array>
#include <cmath>
#include <thread>

namespace {

constexpr azl::EngineConfig kSize{.width = 480, .height = 270};
constexpr double kStillFps = 1e6;

using Corners = std::array<double, 8>;
using Points = std::vector<azl::Point>;

constexpr Corners kFullCorners{0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0};
constexpr Corners kKeystone{0.2, 0.1, 0.8, 0.1, 0.95, 0.9, 0.05, 0.9};

void registerTestEffects()
{
    if (azl::findEffect("mapGradient"))
        return;
    // Red is x, green is y, blue a soft wave: smooth, so warped goldens stay stable across GPUs.
    azl::registerEffect({.type = "mapGradient",
                         .kind = azl::StageKind::Pointwise,
                         .metal = "\n    float2 uv = px / h.size;\n"
                                  "    return float4(uv, 0.5 + 0.5 * sin(px.x * 0.12) * sin(px.y * 0.12), 1.0);\n"});
    azl::registerEffect({.type = "mapWhite", .kind = azl::StageKind::Pointwise, .metal = "\n    return float4(1.0);\n"});
}

azl::Layer effectLayer(uint64_t id, const char* effect)
{
    registerTestEffects();
    return {.id = id, .name = effect, .effects = {{id * 10, effect, {}}}};
}

azl::Slice slice(uint64_t id, Corners corners = kFullCorners, std::array<double, 4> input = {0.0, 0.0, 1.0, 1.0},
                 uint64_t source = 0)
{
    return {.id = id, .input = source, .inputRect = input, .corners = corners};
}

azl::Screen screen(uint64_t id, std::vector<azl::Slice> slices, std::string display = "")
{
    return {.id = id, .name = "Screen " + std::to_string(id), .display = std::move(display), .slices = std::move(slices)};
}

azl::Composition gradient(std::vector<azl::Screen> screens)
{
    return {.layers = {effectLayer(1, "mapGradient")}, .screens = std::move(screens)};
}

size_t differingPixels(const azl::Image& a, const azl::Image& b, int tolerance = 3)
{
    REQUIRE(a.rgba.size() == b.rgba.size());
    size_t differing = 0;
    for (size_t i = 0; i < a.rgba.size(); i += 4)
        for (int c = 0; c < 3; ++c)
            if (std::abs(int(a.rgba[i + c]) - int(b.rgba[i + c])) > tolerance) {
                differing++;
                break;
            }
    return differing;
}

bool same(const azl::Image& a, const azl::Image& b)
{
    return differingPixels(a, b) <= a.width * a.height / 2000;
}

Points flatGrid(int patchesX, int patchesY)
{
    Points points;
    for (int r = 0; r <= patchesY * 3; ++r)
        for (int c = 0; c <= patchesX * 3; ++c)
            points.push_back({c / (patchesX * 3.0), r / (patchesY * 3.0)});
    return points;
}

// The top edge's handles pulled down, so its middle bows to a third of the way down.
Points bowedTop()
{
    Points points = flatGrid(1, 1);
    points[1][1] = 0.4;
    points[2][1] = 0.4;
    return points;
}

bool black(const uint8_t* p)
{
    return p[0] < 3 && p[1] < 3 && p[2] < 3;
}

bool near(const uint8_t* p, int r, int g, int tolerance = 3)
{
    return std::abs(int(p[0]) - r) <= tolerance && std::abs(int(p[1]) - g) <= tolerance;
}

// The gradient's red and green at a pixel the slice shows unwarped and full screen.
std::array<int, 2> gradientAt(uint32_t x, uint32_t y)
{
    return {int(std::lround((x + 0.5) / kSize.width * 255.0)), int(std::lround((y + 0.5) / kSize.height * 255.0))};
}

double linear(uint8_t v)
{
    return std::pow(v / 255.0, 2.2);
}

azl::DisplayInfo display(const std::string& id)
{
    return {id, id, kSize.width, kSize.height, 60.0};
}

} // namespace

TEST_CASE("a Bezier warp bends a slice before its corner pin; linear mode, subdivision and undo")
{
    azl::EngineHarness harness(kSize, kStillFps, {}, gradient({screen(1, {slice(1)})}));
    Client client(harness.document());
    harness.step();
    const azl::Image flat = harness.readScreen(1);

    const Message changed = apply(client, azl::SetSliceWarp{1, true, std::nullopt, bowedTop()});
    CHECK(changed["points"].size() == 16);
    harness.step();
    const azl::Image bowed = harness.readScreen(1);
    CHECK(black(pixel(bowed, 240, 40)));
    CHECK(!black(pixel(bowed, 240, 120)));
    // Only y moves, so every column keeps its input's x (red); a pixel near the bowed edge would ride on rounding.
    for (uint32_t x : {20u, 240u, 460u})
        CHECK(std::abs(int(pixel(bowed, x, 200)[0]) - gradientAt(x, 200)[0]) <= 3);

    // Linear mode puts edge handles at thirds: the same anchors give the flat slice.
    apply(client, azl::SetSliceWarp{1, false, std::nullopt, std::nullopt});
    harness.step();
    CHECK(same(harness.readScreen(1), flat));

    // Subdividing keeps the shape.
    apply(client, azl::SetSliceWarp{1, true, std::array<int, 2>{2, 2}, std::nullopt});
    harness.step();
    const Message snapshot = [&] {
        client.send(msg::snapshotRequest());
        return client.next();
    }();
    const Message warp = snapshot["composition"]["screens"][0]["slices"][0]["warp"];
    CHECK(warp["patches"] == Message({2, 2}));
    CHECK(warp["points"].size() == 49);
    CHECK(differingPixels(harness.readScreen(1), bowed) < bowed.width * bowed.height / 200);

    // In a linear 2x2 grid the centre anchor carries the input's centre with it.
    Points moved = flatGrid(2, 2);
    moved[3 * 7 + 3] = {0.6, 0.4};
    apply(client, azl::SetSliceWarp{1, false, std::array<int, 2>{2, 2}, moved});
    harness.step();
    CHECK(near(pixel(harness.readScreen(1), 288, 108), 128, 128));

    for (int i = 0; i < 4; ++i) {
        client.send(msg::undo());
        CHECK(client.next()["type"] == "change");
    }
    harness.step();
    CHECK(same(harness.readScreen(1), flat));

    CHECK(command(client, azl::SetSliceWarp{1, std::nullopt, std::nullopt, flatGrid(2, 1)})["type"] == "error");
    CHECK(command(client, azl::SetSliceWarp{1, std::nullopt, std::array<int, 2>{0, 1}, std::nullopt})["type"] == "error");
    CHECK(command(client, azl::SetSliceWarp{1, std::nullopt, std::array<int, 2>{17, 1}, std::nullopt})["type"] == "error");
    CHECK(command(client, azl::SetSliceWarp{9, true, std::nullopt, std::nullopt})["type"] == "error");
}

TEST_CASE("golden frame of a Bezier warp under a corner pin")
{
    Points points = flatGrid(2, 2);
    points[1] = {1.0 / 6.0, 0.15};
    points[5] = {5.0 / 6.0, 0.15};
    points[3 * 7 + 3] = {0.45, 0.6};
    points[3 * 7] = {0.08, 0.5};
    azl::Slice warped = slice(1, kKeystone);
    warped.warp = {{2, 2}, true, points};
    azl::EngineHarness harness(kSize, kStillFps, {}, gradient({screen(1, {warped})}));
    harness.step();
    checkGolden(harness.readScreen(1), "warp");
}

TEST_CASE("soft edges fade with power, luminance and gamma, and come from overlapping input rects")
{
    // Two projectors sharing the middle fifth of the composition.
    azl::EngineHarness harness(kSize, kStillFps, {},
                               {.layers = {effectLayer(2, "mapGradient"), effectLayer(1, "mapWhite")},
                                .screens = {screen(1, {slice(1, kFullCorners, {0.0, 0.0, 0.6, 1.0})}),
                                            screen(2, {slice(2, kFullCorners, {0.4, 0.0, 0.6, 1.0})}),
                                            screen(3, {slice(3, kFullCorners, {0.3, 0.0, 0.6, 1.0}, 2)})}});
    Client client(harness.document());

    const Message changed = apply(client, azl::SetSoftEdges{{}, 1});
    REQUIRE(changed["slices"].size() == 2);
    CHECK(changed["slices"][0]["slice"] == 1);
    CHECK(std::abs(changed["slices"][0]["softEdge"]["right"].get<double>() - 1.0 / 3.0) < 1e-9);
    CHECK(changed["slices"][0]["softEdge"]["left"] == 0.0);
    CHECK(changed["slices"][1]["slice"] == 2);
    CHECK(std::abs(changed["slices"][1]["softEdge"]["left"].get<double>() - 1.0 / 3.0) < 1e-9);
    CHECK(!changed.contains("fromOverlap"));

    harness.step();
    const azl::Image left = harness.readScreen(1);
    const azl::Image right = harness.readScreen(2);
    CHECK(pixel(left, 100, 135)[0] == 255);
    CHECK(pixel(left, 479, 135)[0] < 8);
    CHECK(std::abs(int(pixel(left, 400, 135)[0]) - 185) <= 3);
    // The same composition pixel on both projectors adds up to full light once the projector's gamma applies.
    for (uint32_t x : {330u, 360u, 400u, 440u, 470u})
        CHECK(std::abs(linear(pixel(left, x, 135)[0]) + linear(pixel(right, x - 320, 135)[0]) - 1.0) < 0.03);
    checkGolden(left, "soft_edge");

    apply(client, azl::SetSoftEdges{{{1, {.right = 1.0 / 3.0, .gamma = 1.0}}}, 0});
    harness.step();
    CHECK(std::abs(int(pixel(harness.readScreen(1), 400, 135)[0]) - 126) <= 3);
    apply(client, azl::SetSoftEdges{{{1, {.right = 1.0 / 3.0, .power = 1.0, .luminance = 0.5, .gamma = 1.0}}}, 0});
    harness.step();
    CHECK(std::abs(int(pixel(harness.readScreen(1), 440, 135)[0]) - 63) <= 3);

    for (int i = 0; i < 3; ++i) {
        client.send(msg::undo());
        CHECK(client.next()["type"] == "change");
    }
    harness.step();
    CHECK(pixel(harness.readScreen(1), 400, 135)[0] == 255);

    // Slice 3 takes another input, so nothing overlaps it.
    CHECK(command(client, azl::SetSoftEdges{{}, 3})["type"] == "error");
    CHECK(command(client, azl::SetSoftEdges{{{1, {.left = 1.5}}}, 0})["type"] == "error");
    CHECK(command(client, azl::SetSoftEdges{{{1, {.gamma = 0.0}}}, 0})["type"] == "error");
}

TEST_CASE("masks black out inside, or outside when inverted; a slice's mask hides only that slice")
{
    azl::EngineHarness harness(kSize, kStillFps, {},
                               {.layers = {effectLayer(1, "mapGradient"), effectLayer(2, "mapWhite")},
                                .screens = {screen(1, {slice(1, kFullCorners, {0.0, 0.0, 1.0, 1.0}, 1),
                                                       slice(2, {0.5, 0.5, 0.9, 0.5, 0.9, 0.9, 0.5, 0.9},
                                                             {0.0, 0.0, 1.0, 1.0}, 2)})}});
    Client client(harness.document());
    harness.step();
    CHECK(pixel(harness.readScreen(1), 336, 189)[0] == 255);

    const Points square{{0.1, 0.1}, {0.4, 0.1}, {0.4, 0.4}, {0.1, 0.4}};
    const Message added = apply(client, azl::AddMask{1, 0, std::nullopt, {.points = square}});
    CHECK(added["mask"]["id"] == 1);
    CHECK(added["mask"]["name"] == "Mask 1");
    harness.step();
    azl::Image image = harness.readScreen(1);
    CHECK(black(pixel(image, 120, 60)));
    CHECK(near(pixel(image, 300, 60), gradientAt(300, 60)[0], gradientAt(300, 60)[1]));

    apply(client, azl::SetMask{1, std::nullopt, true, std::nullopt});
    harness.step();
    image = harness.readScreen(1);
    CHECK(near(pixel(image, 120, 60), gradientAt(120, 60)[0], gradientAt(120, 60)[1]));
    CHECK(black(pixel(image, 300, 60)));
    apply(client, azl::RemoveMask{1});

    // Over the white slice's whole input: the gradient below shows through, not black.
    apply(client, azl::AddMask{1, 2, std::nullopt, {.points = {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}}}});
    harness.step();
    CHECK(near(pixel(harness.readScreen(1), 336, 189), gradientAt(336, 189)[0], gradientAt(336, 189)[1]));

    client.send(msg::undo());
    client.next();
    client.send(msg::undo());
    CHECK(client.next()["op"] == "addMask");
    harness.step();
    CHECK(black(pixel(harness.readScreen(1), 300, 60)));

    CHECK(command(client, azl::AddMask{1, 0, std::nullopt, {.points = {{0.0, 0.0}, {1.0, 1.0}}}})["type"] == "error");
    CHECK(command(client, azl::AddMask{1, 9, std::nullopt, {.points = square}})["type"] == "error");
    CHECK(command(client, azl::AddMask{1, 0, std::nullopt, {.id = 1, .points = square}})["type"] == "error");
    CHECK(command(client, azl::RemoveMask{7})["type"] == "error");
}

TEST_CASE("golden frame of a screen mask and an inverted slice mask under a corner pin")
{
    azl::Slice pinned = slice(1, kKeystone);
    Points octagon;
    for (int i = 0; i < 8; ++i)
        octagon.push_back({0.5 + 0.4 * std::cos(i * M_PI / 4.0), 0.5 + 0.4 * std::sin(i * M_PI / 4.0)});
    pinned.masks = {{.id = 1, .invert = true, .points = octagon}};
    azl::Screen masked = screen(1, {pinned});
    masked.masks = {{.id = 2, .points = {{0.6, 0.55}, {0.85, 0.45}, {0.95, 0.8}, {0.7, 0.75}}}};
    azl::EngineHarness harness(kSize, kStillFps, {}, gradient({masked}));
    harness.step();
    checkGolden(harness.readScreen(1), "mask");
}

TEST_CASE("screen and slice colour: the slice's adds to the screen's, clamped")
{
    azl::EngineHarness harness(kSize, kStillFps, {}, gradient({screen(1, {slice(1)})}));
    Client client(harness.document());
    const auto [r, g] = gradientAt(300, 200);

    apply(client, azl::SetScreenColor{1, {.brightness = -1.0}});
    harness.step();
    CHECK(black(pixel(harness.readScreen(1), 300, 200)));
    apply(client, azl::SetSliceColor{1, {.brightness = 0.5}});
    harness.step();
    CHECK(near(pixel(harness.readScreen(1), 300, 200), r / 2, g / 2));
    apply(client, azl::SetScreenColor{1, {.red = 1.0}});
    harness.step();
    CHECK(near(pixel(harness.readScreen(1), 300, 200), 255, std::min(255, g * 3 / 2)));
    apply(client, azl::SetSliceColor{1, {.contrast = -1.0}});
    harness.step();
    const uint8_t* grey = pixel(harness.readScreen(1), 300, 200);
    CHECK((std::abs(grey[0] - 128) <= 2 && std::abs(grey[1] - 128) <= 2 && std::abs(grey[2] - 128) <= 2));

    CHECK(command(client, azl::SetSliceColor{1, {.brightness = 2.0}})["type"] == "error");
    CHECK(command(client, azl::SetScreenColor{5, {}})["type"] == "error");
}

TEST_CASE("the test card lives in composition space: overlapping slices show the same pixels; all screens at once")
{
    azl::EngineHarness harness(kSize, kStillFps, {},
                               gradient({screen(1, {slice(1, kFullCorners, {0.0, 0.0, 0.6, 1.0})}),
                                         screen(2, {slice(2, kFullCorners, {0.4, 0.0, 0.6, 1.0})})}));
    Client client(harness.document());
    harness.step();
    const azl::Image mapped = harness.readScreen(2);

    apply(client, azl::SetScreenTestCard{0, true});
    harness.step();
    const azl::Image left = harness.readScreen(1);
    const azl::Image right = harness.readScreen(2);
    CHECK(!same(right, mapped));
    // Below the name labels, the shared part of the composition is pixel for pixel the same on both screens.
    size_t differing = 0;
    for (uint32_t y = kSize.height / 2; y < kSize.height - 4; ++y)
        for (uint32_t x = 4; x < 156; ++x)
            for (int c = 0; c < 3; ++c)
                if (std::abs(int(pixel(left, x + 320, y)[c]) - int(pixel(right, x, y)[c])) > 3) {
                    differing++;
                    break;
                }
    CHECK(differing == 0);

    // The composition's card wins over a screen's own setting, and turning it off restores each screen's.
    apply(client, azl::SetScreenTestCard{2, true});
    apply(client, azl::SetScreenTestCard{0, false});
    harness.step();
    CHECK(same(harness.readScreen(2), right));
    CHECK(!same(harness.readScreen(1), left));
    apply(client, azl::SetScreenTestCard{2, false});
    harness.step();
    CHECK(same(harness.readScreen(2), mapped));
}

TEST_CASE("golden frame of the test card through a Bezier warp")
{
    azl::Slice warped = slice(1, kKeystone);
    warped.warp = {{1, 1}, true, bowedTop()};
    azl::Screen card = screen(1, {warped}, "proj-a");
    card.testCard = true;
    azl::EngineHarness harness(kSize, kStillFps, {}, gradient({card}));
    harness.step();
    checkGolden(harness.readScreen(1), "test_card_warp");
}

TEST_CASE("the corner target and the guides are machine-local overlays drawn on the outputs")
{
    azl::EngineHarness harness(kSize, 60.0, {}, gradient({screen(1, {slice(1)}, "proj-a")}));
    harness.displays().attach(display("proj-a"));
    Client client(harness.document());
    harness.step();
    const azl::Image plain = harness.readScreen(1);

    client.send({{"type", "outputs"}, {"op", "target"}, {"display", "proj-a"}, {"point", {0.25, 0.5}}});
    const Message list = client.next();
    CHECK(list["outputs"][0]["target"] == Message({0.25, 0.5}));
    harness.step();
    const azl::Image target = harness.readScreen(1);
    for (auto [x, y] : {std::pair{120u, 20u}, std::pair{121u, 250u}, std::pair{400u, 135u}, std::pair{10u, 134u}})
        CHECK_MESSAGE(pixel(target, x, y)[0] == 255, x << "," << y);
    CHECK(black(pixel(target, 123, 20)));
    CHECK(near(pixel(target, 126, 20), gradientAt(126, 20)[0], gradientAt(126, 20)[1]));
    checkGolden(target, "corner_target");

    // What the display shows is what the harness paints for its screen.
    for (int i = 0; i < 10; ++i) {
        harness.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(same(harness.displays().capture("proj-a"), target));

    apply(client, azl::SetGuides{{{true, 0.5}, {false, 0.25}}});
    harness.step();
    CHECK(same(harness.readScreen(1), target));
    client.send({{"type", "outputs"}, {"op", "guides"}, {"show", true}});
    CHECK(client.next()["guides"] == true);
    harness.step();
    const azl::Image guides = harness.readScreen(1);
    CHECK((pixel(guides, 240, 50)[2] > 250 && pixel(guides, 240, 50)[0] < 60));
    CHECK((pixel(guides, 300, 67)[2] > 250 && pixel(guides, 300, 67)[0] < 60));

    client.send({{"type", "outputs"}, {"op", "guides"}, {"show", false}});
    client.next();
    client.send({{"type", "outputs"}, {"op", "target"}, {"display", "proj-a"}, {"point", nullptr}});
    CHECK(!client.next()["outputs"][0].contains("target"));
    harness.step();
    CHECK(same(harness.readScreen(1), plain));
}

TEST_CASE("mapping survives a document round trip")
{
    azl::Slice s = slice(1, kKeystone);
    s.warp = {{1, 1}, true, bowedTop()};
    s.softEdge = {.left = 0.1, .bottom = 0.2, .power = 3.0, .luminance = 0.4, .gamma = 1.8};
    s.color = {.brightness = 0.1, .contrast = -0.2, .red = 0.3, .green = 0.0, .blue = -0.4};
    s.masks = {{.id = 3, .name = "Hole", .invert = true, .points = {{0.1, 0.1}, {0.5, 0.2}, {0.3, 0.6}}}};
    azl::Screen sc = screen(1, {s});
    sc.color = {.blue = 0.5};
    sc.masks = {{.id = 4, .name = "Pillar", .points = {{0.4, 0.0}, {0.5, 0.0}, {0.5, 1.0}}}};
    azl::Composition composition = gradient({sc});
    composition.testCard = true;
    composition.guides = {{true, 0.3}, {false, 0.7}};

    const auto back = nlohmann::json(composition).get<azl::Composition>();
    CHECK(back.screens == composition.screens);
    CHECK(back.testCard);
    CHECK(back.guides == composition.guides);
}

// A measurement, not a check: nocturne_tests -tc="GPU cost*" --no-skip
TEST_CASE("GPU cost of painting a 1080p screen" * doctest::skip())
{
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    auto device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    auto queue = NS::TransferPtr(device->newCommandQueue());
    auto texture = [&] {
        auto desc = NS::RetainPtr(MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatBGRA8Unorm, 1920, 1080, false));
        desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        desc->setStorageMode(MTL::StorageModePrivate);
        return NS::TransferPtr(device->newTexture(desc.get()));
    };
    azl::Frame frame{.texture = texture()};
    auto target = texture();
    const auto compileStart = std::chrono::steady_clock::now();
    azl::ScreenPainter painter(device.get(), MTL::PixelFormatBGRA8Unorm, {1920, 1080});
    MESSAGE("painter pipelines compiled in "
            << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - compileStart).count()
            << " ms");

    Points octagon;
    for (int i = 0; i < 8; ++i)
        octagon.push_back({0.5 + 0.4 * std::cos(i * M_PI / 4.0), 0.5 + 0.4 * std::sin(i * M_PI / 4.0)});
    auto with = [](auto f) {
        azl::Slice s = slice(1, kKeystone);
        f(s);
        return azl::ScreenView{screen(1, {s})};
    };
    const azl::Warp bowed{{1, 1}, true, bowedTop()};
    azl::Slice all = slice(1, kKeystone);
    all.warp = azl::regridWarp(bowed, {4, 4});
    all.softEdge = {.left = 0.2, .right = 0.2, .top = 0.1, .bottom = 0.1};
    all.color = {.brightness = 0.1, .red = 0.1};
    all.masks = {{.id = 1, .invert = true, .points = octagon}};
    azl::Screen full = screen(1, {all, all, all});
    full.masks = {{.id = 2, .points = octagon}};
    azl::ScreenView card{full};
    card.screen.testCard = true;
    azl::ScreenView overlays{screen(1, {slice(1, kKeystone)}), {{true, 0.5}, {false, 0.5}}, azl::Point{0.3, 0.3}};

    const std::vector<std::pair<const char*, azl::ScreenView>> cases{
        {"keystone slice", with([](auto&) {})},
        {"warp 1x1", with([&](auto& s) { s.warp = bowed; })},
        {"warp 4x4", with([&](auto& s) { s.warp = azl::regridWarp(bowed, {4, 4}); })},
        {"warp 16x16", with([&](auto& s) { s.warp = azl::regridWarp(bowed, {16, 16}); })},
        {"soft edge, 4 sides", with([](auto& s) { s.softEdge = {.left = 0.2, .right = 0.2, .top = 0.1, .bottom = 0.1}; })},
        {"colour", with([](auto& s) { s.color = {.brightness = 0.1, .contrast = 0.1, .red = 0.1}; })},
        {"slice mask, 8 points", with([&](auto& s) { s.masks = {{.id = 1, .points = octagon}}; })},
        {"screen mask, 8 points", [&] {
             azl::ScreenView v = with([](auto&) {});
             v.screen.masks = {{.id = 1, .points = octagon}};
             return v;
         }()},
        {"3 slices with all of it", azl::ScreenView{full}},
        {"test card through those", card},
        {"guides and corner target", overlays},
    };
    for (const auto& [name, view] : cases) {
        // Batches keep the GPU busy, so its clock does not drop between paints; the fastest batch is the least
        // disturbed by other work on the machine.
        constexpr int kBatches = 20, kPerBatch = 20;
        double best = 1e9;
        for (int b = 0; b < kBatches + 2; ++b) {
            auto* cmd = queue->commandBuffer();
            for (int i = 0; i < kPerBatch; ++i)
                painter.encode(cmd, &frame, view, target.get());
            cmd->commit();
            cmd->waitUntilCompleted();
            if (b >= 2)
                best = std::min(best, cmd->GPUEndTime() - cmd->GPUStartTime());
        }
        MESSAGE(std::string(name) << ": " << best / kPerBatch * 1000.0 << " ms");
    }
    pool->release();
}

TEST_CASE("regridding a warp keeps its surface")
{
    azl::Warp warp{{1, 1}, true, bowedTop()};
    const azl::Warp twice = azl::regridWarp(warp, {2, 2});
    const azl::Warp back = azl::regridWarp(twice, {1, 1});
    const Points grid = azl::resolvedWarpGrid(warp);
    REQUIRE(back.points.size() == grid.size());
    for (size_t i = 0; i < grid.size(); ++i)
        CHECK((std::abs(back.points[i][0] - grid[i][0]) < 1e-9 && std::abs(back.points[i][1] - grid[i][1]) < 1e-9));
    // The middle of the bowed top edge is now an anchor, at its curve's midpoint.
    CHECK(std::abs(twice.points[3][1] - 0.3) < 1e-9);
    CHECK(azl::regridWarp(azl::Warp{}, {3, 2}).points.empty());
}
