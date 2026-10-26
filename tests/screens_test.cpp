#include "document/composition_file.hpp"
#include "document/screen_roles.hpp"
#include "engine/thread_priority.hpp"
#include "golden.hpp"
#include "net/websocket.hpp"
#include "platform/mac_displays.hpp"
#include "test_client.hpp"
#include "test_paths.hpp"

#include <doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <thread>
#include <vector>

namespace {

constexpr azl::EngineConfig kSize{.width = 480, .height = 270};
// Keeps the clock near zero, so the test card's moving marker stays put.
constexpr double kStillFps = 1e6;

using Corners = std::array<double, 8>;
using InputRect = std::array<double, 4>;

constexpr InputRect kFull{0.0, 0.0, 1.0, 1.0};
// Narrower at the top, as a projector tilted up throws it.
constexpr Corners kKeystone{0.2, 0.1, 0.8, 0.1, 0.95, 0.9, 0.05, 0.9};

// A smooth picture of its own pixel position: red is x, green is y, blue a soft wave pattern. Smooth content keeps
// goldens of warped slices stable across GPUs, and position as colour shows where every input pixel landed.
void registerGradient()
{
    if (azl::findEffect("testGradient"))
        return;
    azl::registerEffect({.type = "testGradient",
                         .kind = azl::StageKind::Pointwise,
                         .metal = "\n    float2 uv = px / h.size;\n"
                                  "    return float4(uv, 0.5 + 0.5 * sin(px.x * 0.12) * sin(px.y * 0.12), 1.0);\n"});
}

azl::Layer gradientLayer(uint64_t id)
{
    registerGradient();
    return {.id = id, .name = "Gradient", .effects = {{id * 10, "testGradient", {}}}};
}

azl::Slice slice(uint64_t id, Corners corners, InputRect input = kFull, uint64_t source = 0)
{
    return {.id = id, .input = source, .inputRect = input, .corners = corners};
}

azl::Screen screen(uint64_t id, std::string display, std::vector<azl::Slice> slices)
{
    return {.id = id, .name = "Screen " + std::to_string(id), .display = std::move(display), .slices = std::move(slices)};
}

// The gradient below a group holding the test card.
azl::Composition stage(std::vector<azl::Screen> screens = {})
{
    return {.layers = {gradientLayer(1), {.id = 10, .name = "Stage", .group = true, .layers = {{.id = 11, .name = "Card"}}}},
            .screens = std::move(screens)};
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

// Where the quad's diagonals cross: a perspective map sends the input's centre there, an affine one would not.
std::array<double, 2> diagonalsCross(const Corners& c)
{
    const double x1 = c[0], y1 = c[1], x2 = c[4], y2 = c[5];
    const double x3 = c[2], y3 = c[3], x4 = c[6], y4 = c[7];
    const double t = ((x1 - x3) * (y3 - y4) - (y1 - y3) * (x3 - x4)) / ((x1 - x2) * (y3 - y4) - (y1 - y2) * (x3 - x4));
    return {x1 + t * (x2 - x1), y1 + t * (y2 - y1)};
}

azl::DisplayInfo display(const std::string& id)
{
    return {id, id, kSize.width, kSize.height, 60.0};
}

uint64_t presents(azl::EngineHarness& harness, const std::string& display)
{
    azl::DisplayOutput* output = harness.outputs().find(display);
    REQUIRE(output);
    return output->stats().presents.load();
}

// The presents each output makes while frames keep rendering for `seconds`.
std::map<std::string, uint64_t> presentsWhileRendering(azl::EngineHarness& harness, double seconds)
{
    std::map<std::string, uint64_t> before;
    for (azl::DisplayOutput* output : harness.outputs().all())
        before[output->display()] = output->stats().presents.load();
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        harness.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::map<std::string, uint64_t> made;
    for (const auto& [id, count] : before)
        made[id] = presents(harness, id) - count;
    return made;
}

std::string mainOutput(azl::EngineHarness& harness)
{
    std::string main;
    for (azl::DisplayOutput* output : harness.outputs().all())
        if (output->main()) {
            CHECK(main.empty());
            main = output->display();
        }
    return main;
}

} // namespace

TEST_CASE("assigning a display to a group creates a screen with one full slice of it, with undo")
{
    azl::EngineHarness harness(kSize, kStillFps, {}, stage());
    Client client(harness.document());
    CHECK(client.snapshot["composition"]["screens"].empty());

    const Message created = apply(client, azl::AssignDisplay{10, "proj-a"})["screen"];
    const uint64_t id = created["id"];
    CHECK(created["name"] == "Stage");
    CHECK(created["display"] == "proj-a");
    CHECK(created["testCard"] == false);
    REQUIRE(created["slices"].size() == 1);
    CHECK(created["slices"][0]["input"] == 10);
    CHECK(created["slices"][0]["inputRect"] == Message(kFull));
    CHECK(created["slices"][0]["corners"] == Message({0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0}));

    CHECK(command(client, azl::AssignDisplay{10, "proj-a"})["type"] == "error");
    CHECK(command(client, azl::AssignDisplay{1, "proj-b"})["type"] == "error");

    // The screen shows the group's own picture: the group covers the composition, but at half opacity only the
    // composition lets the gradient through.
    harness.step();
    const azl::Image group = harness.readScreen(id);
    CHECK(same(group, harness.readScreen()));
    apply(client, azl::SetLayerOpacity{10, 0.5});
    harness.step();
    CHECK(same(harness.readScreen(id), group));
    CHECK(!same(harness.readScreen(), group));

    client.send(msg::undo());
    client.next();
    client.send(msg::undo());
    CHECK(client.next()["op"] == "removeScreen");
    client.send(msg::snapshotRequest());
    CHECK(client.next()["composition"]["screens"].empty());
    client.send(msg::redo());
    CHECK(client.next()["screen"]["id"] == id);
}

TEST_CASE("slices are corner-pinned through a perspective-correct homography")
{
    azl::EngineHarness harness(kSize, kStillFps, {},
                               {.layers = {gradientLayer(1)}, .screens = {screen(1, "", {slice(1, kKeystone)})}});
    harness.step();
    const azl::Image image = harness.readScreen(1);

    // The input's centre, red and green at one half, lands where the diagonals cross, not halfway down.
    const auto [x, y] = diagonalsCross(kKeystone);
    const uint8_t* centre = pixel(image, uint32_t(x * image.width), uint32_t(y * image.height));
    CHECK(std::abs(int(centre[0]) - 128) <= 3);
    CHECK(std::abs(int(centre[1]) - 128) <= 3);
    // Outside the quad the screen is black.
    const uint8_t* corner = pixel(image, 2, 2);
    CHECK((corner[0] | corner[1] | corner[2]) == 0);

    checkGolden(image, "screen_keystone");
}

TEST_CASE("golden frames per screen for two-slice and four-slice mappings")
{
    const azl::Screen twoSlices = screen(1, "", {slice(1, {0.02, 0.1, 0.48, 0.02, 0.48, 0.98, 0.02, 0.9}, {0.0, 0.0, 0.5, 1.0}),
                                                 slice(2, {0.52, 0.02, 0.98, 0.1, 0.98, 0.9, 0.52, 0.98}, {0.5, 0.0, 0.5, 1.0})});
    // Three quadrants on the faces of a cube, the fourth on a small tilted panel.
    const azl::Screen fourSlices =
        screen(2, "", {slice(3, {0.5, 0.05, 0.9, 0.2, 0.5, 0.35, 0.1, 0.2}, {0.0, 0.0, 0.5, 0.5}),
                       slice(4, {0.1, 0.2, 0.5, 0.35, 0.5, 0.95, 0.1, 0.75}, {0.5, 0.0, 0.5, 0.5}),
                       slice(5, {0.5, 0.35, 0.9, 0.2, 0.9, 0.75, 0.5, 0.95}, {0.0, 0.5, 0.5, 0.5}),
                       slice(6, {0.75, 0.78, 0.97, 0.8, 0.95, 0.97, 0.7, 0.95}, {0.5, 0.5, 0.5, 0.5})});
    azl::EngineHarness harness(kSize, kStillFps, {}, {.layers = {gradientLayer(1)}, .screens = {twoSlices, fourSlices}});
    harness.step();
    checkGolden(harness.readScreen(1), "screen_two_slices");
    checkGolden(harness.readScreen(2), "screen_four_slices");
}

TEST_CASE("with opaque outputs every screen shows the same, unblended only where the composition is opaque")
{
    azl::Screen coloured = screen(1, "", {slice(1, {0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0})});
    coloured.color.brightness = 0.4;
    coloured.color.contrast = 0.8;
    const azl::Screen overlapping =
        screen(2, "", {slice(2, kKeystone), slice(3, {0.3, 0.0, 1.0, 0.2, 0.9, 1.0, 0.25, 0.8}, {0.25, 0.0, 0.75, 1.0})});
    // A layer's picture is transparent around its shrunk card, so it must blend over the black.
    const azl::Screen layer = screen(3, "", {slice(4, kKeystone, kFull, 20)});
    azl::Screen soft = screen(4, "", {slice(5, {0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0})});
    soft.slices[0].softEdge.left = 0.3;
    const azl::Composition composition{
        .layers = {gradientLayer(1), {.id = 20, .name = "Shrunk", .effects = {{200, "transform", {{"scale", 0.5}}}}}},
        .screens = {coloured, overlapping, layer, soft}};
    auto shown = [&](bool opaque) {
        azl::EngineConfig config = kSize;
        config.opaqueOutputs = opaque;
        azl::EngineHarness harness(config, kStillFps, {}, composition);
        harness.step();
        std::vector<azl::Image> images{harness.readScreen()};
        for (uint64_t id = 1; id <= 4; ++id)
            images.push_back(harness.readScreen(id));
        return images;
    };
    const std::vector<azl::Image> blended = shown(false);
    const std::vector<azl::Image> opaque = shown(true);
    for (size_t i = 0; i < blended.size(); ++i) {
        CAPTURE(i);
        CHECK(blended[i].rgba == opaque[i].rgba);
    }
}

TEST_CASE("slice corners and inputs are edited through the protocol, with undo")
{
    azl::EngineHarness harness(kSize, kStillFps, {}, stage({screen(1, "", {slice(1, {0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0})})}));
    Client client(harness.document());
    harness.step();
    const azl::Image full = harness.readScreen(1);

    apply(client, azl::SetSliceCorners{1, kKeystone});
    harness.step();
    const azl::Image keystone = harness.readScreen(1);
    CHECK(!same(keystone, full));
    const Message added = apply(client, azl::AddSlice{1, std::nullopt, slice(0, {0.0, 0.0, 0.3, 0.0, 0.3, 0.3, 0.0, 0.3}, {0.0, 0.0, 0.5, 0.5}, 1)});
    CHECK(added["slice"]["id"] == 2);
    CHECK(added["slice"]["name"] == "Slice 2");
    apply(client, azl::SetSliceInput{2, 10, InputRect{0.25, 0.25, 0.5, 0.5}});
    harness.step();
    CHECK(!same(harness.readScreen(1), keystone));

    for (int i = 0; i < 2; ++i) {
        client.send(msg::undo());
        CHECK(client.next()["type"] == "change");
    }
    harness.step();
    CHECK(same(harness.readScreen(1), keystone));
    client.send(msg::undo());
    client.next();
    harness.step();
    CHECK(same(harness.readScreen(1), full));

    CHECK(command(client, azl::SetSliceCorners{1, {0.0, 0.0, 3.0, 0.0, 1.0, 1.0, 0.0, 1.0}})["type"] == "error");
    CHECK(command(client, azl::SetSliceInput{1, 99, std::nullopt})["type"] == "error");
    CHECK(command(client, azl::SetSliceInput{1, std::nullopt, InputRect{0.5, 0.0, 0.6, 1.0}})["type"] == "error");
    CHECK(command(client, azl::SetSliceCorners{9, kKeystone})["type"] == "error");
    CHECK(command(client, azl::AddScreen{std::nullopt, {.display = "proj-a"}})["type"] == "change");
    CHECK(command(client, azl::SetScreenDisplay{1, "proj-a"})["type"] == "error");
    CHECK(command(client, azl::SetScreenDisplay{1, "proj-b"})["type"] == "change");
}

TEST_CASE("slices take a group or a layer; an output-only group is on its screen but not in the composition")
{
    azl::EngineHarness harness(kSize, kStillFps, {},
                               stage({screen(1, "", {slice(1, {0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0}, kFull, 10)}),
                                      screen(2, "", {slice(2, {0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0}, kFull, 1)})}));
    Client client(harness.document());
    harness.step();
    const azl::Image group = harness.readScreen(1);
    const azl::Image layer = harness.readScreen(2);
    const uint64_t passes = harness.stats().passes;
    CHECK(!same(group, layer));
    CHECK(same(harness.readScreen(), group));

    apply(client, azl::SetGroupOutputOnly{10, true});
    harness.step();
    CHECK(same(harness.readScreen(), layer));
    CHECK(same(harness.readScreen(1), group));
    CHECK(same(harness.readScreen(2), layer));
    // The group no longer blends into the composition.
    CHECK(harness.stats().passes == passes - 1);

    // A picture renders only while a slice reads it, and an output-only group only then at all.
    apply(client, azl::RemoveScreen{2});
    harness.step();
    CHECK(harness.stats().passes == passes - 2);
    apply(client, azl::RemoveScreen{1});
    harness.step();
    CHECK(harness.stats().passes == passes - 4);
    CHECK(same(harness.readScreen(), layer));

    CHECK(command(client, azl::SetGroupOutputOnly{1, true})["type"] == "error");
}

TEST_CASE("slices of layers and groups with Trails show what the composition shows, history advanced once")
{
    constexpr azl::EngineConfig kWide{.width = 960, .height = 540};
    const azl::Slice full{.id = 1, .input = 1};
    // At 60 frames per second, as Trails fades by elapsed time.
    azl::EngineHarness harness(
        kWide, 60.0, {},
        {.layers = {{.id = 1, .name = "Trails", .effects = {{1, "transform", {}}, {2, "trails", {{"feedback", 0.5}}}}},
                    {.id = 10, .name = "Kept", .group = true, .effects = {{3, "classicTrails", {{"feedback", 0.5}}}},
                     .layers = {{.id = 11, .name = "Card", .opacity = 0.5}}}},
         .screens = {{.id = 1, .slices = {full}}, {.id = 2, .slices = {{.id = 2, .input = 10}}}}});
    Client client(harness.document());
    apply(client, azl::SetGroupOutputOnly{10, true});

    // As in the Trails history test: the card's white left edge stays behind and halves once per frame, on the
    // slice exactly as in the composition, which the layer alone makes.
    harness.step();
    CHECK(pixel(harness.readScreen(1), 0, 270)[0] >= 250);
    apply(client, azl::SetEffectParam{1, "positionX", 0.5});
    for (int expected : {128, 64}) {
        harness.step();
        const azl::Image composition = harness.readScreen();
        const azl::Image slice = harness.readScreen(1);
        CHECK(std::abs(int(pixel(slice, 0, 270)[0]) - expected) <= 3);
        CHECK(same(slice, composition));
    }

    // The group with its Classic Trails on its own screen is what the composition shows once the group is in it alone.
    apply(client, azl::SetGroupOutputOnly{10, false});
    apply(client, azl::SetLayerBypass{1, true});
    harness.step();
    CHECK(same(harness.readScreen(), harness.readScreen(2)));
}

TEST_CASE("a test card per screen, toggled by command")
{
    azl::EngineHarness harness(kSize, kStillFps, {}, stage({screen(1, "", {slice(1, kKeystone)})}));
    Client client(harness.document());
    harness.step();
    const azl::Image mapped = harness.readScreen(1);
    apply(client, azl::SetScreenTestCard{1, true});
    harness.step();
    checkGolden(harness.readScreen(1), "screen_test_card");
    client.send(msg::undo());
    client.next();
    harness.step();
    CHECK(same(harness.readScreen(1), mapped));
}

TEST_CASE("a disconnected display parks its output; its screen re-attaches by identity, others are untouched")
{
    azl::EngineHarness harness(kSize, 60.0, {},
                               stage({screen(1, "proj-a", {slice(1, kKeystone)}),
                                      screen(2, "proj-b", {slice(2, {0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0}, kFull, 10)}),
                                      screen(3, "proj-c", {slice(3, {0.1, 0.1, 0.9, 0.0, 1.0, 1.0, 0.0, 0.9}, kFull, 1)})}));
    for (const char* id : {"proj-a", "proj-b", "proj-c"})
        harness.displays().attach(display(id));
    auto made = presentsWhileRendering(harness, 0.3);
    for (const char* id : {"proj-a", "proj-b", "proj-c"})
        CHECK(made[id] > 5);
    CHECK(mainOutput(harness) == "proj-a");

    harness.displays().detach("proj-a");
    CHECK(!harness.outputs().find("proj-a")->attached());
    CHECK(mainOutput(harness) == "proj-b");
    const uint64_t rendered = harness.stats().rendered;
    made = presentsWhileRendering(harness, 0.3);
    CHECK(made["proj-a"] == 0);
    CHECK(made["proj-b"] > 5);
    CHECK(made["proj-c"] > 5);
    CHECK(harness.stats().rendered > rendered + 10);

    // It comes back after another display, so its index differs; the screen still finds it by its id.
    harness.displays().attach(display("proj-d"));
    harness.displays().attach(display("proj-a"));
    CHECK(harness.outputs().find("proj-d") == nullptr);
    made = presentsWhileRendering(harness, 0.3);
    CHECK(made["proj-a"] > 5);
    // Outputs show frames begun at least half a refresh ago; this lets the last one age past that.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    for (auto [id, screenId] : {std::pair{"proj-a", 1}, std::pair{"proj-b", 2}, std::pair{"proj-c", 3}})
        CHECK_MESSAGE(same(harness.displays().capture(id), harness.readScreen(screenId)), std::string(id));

    Client client(harness.document());
    client.send({{"type", "outputs"}, {"op", "list"}});
    const Message list = client.next();
    CHECK(list["displays"].size() == 4);
    REQUIRE(list["outputs"].size() == 3);
    CHECK(list["outputs"][0] == Message({{"display", "proj-a"}, {"screen", 1}, {"attached", true}, {"main", true}}));
    client.send({{"type", "outputs"}, {"op", "main"}, {"display", "proj-c"}});
    CHECK(client.next()["outputs"][2]["main"] == true);
}

TEST_CASE("after a restart, screens find their displays by identity whatever order the projectors power on in")
{
    TempDir dir;
    const fs::path state = dir.path / "composition.json";
    {
        azl::EngineHarness before(kSize, 60.0, {},
                                  stage({screen(1, "proj-a", {slice(1, kKeystone)}),
                                         screen(2, "proj-b", {slice(2, {0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0}, kFull, 10)}),
                                         screen(3, "proj-c", {slice(3, {0.1, 0.1, 0.9, 0.0, 1.0, 1.0, 0.0, 0.9}, kFull, 1)})}));
        for (const char* id : {"proj-a", "proj-b", "proj-c"})
            before.displays().attach(display(id));
        before.step();
        azl::saveCompositionFile(state, {before.document().document()});
    }

    const std::optional<azl::CompositionFile> saved = azl::loadCompositionFile(state);
    REQUIRE(saved);
    azl::EngineHarness harness(kSize, 60.0, {}, saved->composition);
    for (const char* id : {"proj-a", "proj-b", "proj-c"}) {
        REQUIRE(harness.outputs().find(id));
        CHECK(!harness.outputs().find(id)->attached());
    }

    // The engine is up before any projector; they power on in another order, with an unrelated display among them.
    for (const char* id : {"proj-c", "monitor", "proj-a"})
        harness.displays().attach(display(id));
    auto made = presentsWhileRendering(harness, 0.3);
    CHECK(made["proj-a"] > 5);
    CHECK(made["proj-b"] == 0);
    CHECK(made["proj-c"] > 5);
    CHECK(harness.outputs().find("monitor") == nullptr);

    harness.displays().attach(display("proj-b"));
    made = presentsWhileRendering(harness, 0.3);
    for (const char* id : {"proj-a", "proj-b", "proj-c"})
        CHECK(made[id] > 5);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    for (auto [id, screenId] : {std::pair{"proj-a", 1}, std::pair{"proj-b", 2}, std::pair{"proj-c", 3}})
        CHECK_MESSAGE(same(harness.displays().capture(id), harness.readScreen(screenId)), std::string(id));
    const azl::Composition after = harness.document().document();
    for (size_t i = 0; i < 3; ++i)
        CHECK(after.screens[i].display == saved->composition.screens[i].display);
}

TEST_CASE("once locked, every output shows each rendered frame, even when display callbacks run late")
{
    azl::EngineHarness harness(kSize, 60.0, {},
                               stage({screen(1, "proj-a", {slice(1, kKeystone)}),
                                      screen(2, "proj-b", {slice(2, kKeystone, kFull, 10)}),
                                      screen(3, "proj-c", {slice(3, kKeystone, kFull, 1)})}));
    // Compiles every chain first, so the live run below measures rendering only.
    harness.step();
    harness.displays().setLateness(std::chrono::milliseconds(2), std::chrono::milliseconds(8));
    // The displays refresh at the same rate, each at its own phase.
    for (const char* id : {"proj-a", "proj-b", "proj-c"}) {
        harness.displays().attach(display(id));
        std::this_thread::sleep_for(std::chrono::microseconds(5300));
    }

    // Dropped frames are gaps in the frame index an output shows, so they count exactly whenever the window starts and
    // ends; new frames lag rendering by up to two frames, so their count only shows that every output kept up.
    std::map<std::string, uint64_t> fresh;
    std::map<std::string, uint64_t> dropped;
    uint64_t rendered = 0;
    constexpr double kLocking = 1.0;
    constexpr double kMeasured = 2.0;
    std::thread measure([&] {
        std::this_thread::sleep_for(std::chrono::duration<double>(kLocking));
        // The outputs' window holds the render's, so a late wake between the reads only adds new frames.
        for (azl::DisplayOutput* output : harness.outputs().all()) {
            fresh[output->display()] = output->stats().newFrames;
            dropped[output->display()] = output->stats().dropped;
        }
        rendered = harness.stats().rendered;
        std::this_thread::sleep_for(std::chrono::duration<double>(kMeasured));
        rendered = harness.stats().rendered - rendered;
        for (azl::DisplayOutput* output : harness.outputs().all()) {
            fresh[output->display()] = output->stats().newFrames - fresh[output->display()];
            dropped[output->display()] = output->stats().dropped - dropped[output->display()];
        }
    });
    harness.runLive(kLocking + kMeasured + 0.2);
    measure.join();

    CHECK(rendered > 100);
    for (const auto& [id, frames] : fresh) {
        INFO(id << ": " << frames << " new frames of " << rendered << " rendered, " << dropped[id] << " dropped");
        CHECK(dropped[id] == 0);
        CHECK(frames + 3 >= rendered);
    }
}

TEST_CASE("the frame guard still halves a previewed layer inside a group a second output shows; both outputs keep up")
{
    const azl::Composition show{
        .layers = {{.id = 10, .name = "Stage", .group = true, .layers = {gradientLayer(1), {.id = 11, .name = "Card"}}}},
        .screens = {screen(1, "proj-a", {slice(1, kKeystone)}), screen(2, "proj-b", {slice(2, kKeystone, kFull, 10)})}};
    azl::EngineHarness harness(kSize, 60.0, {}, show);
    harness.step();
    harness.engine().setPreviewPictures({1});
    harness.step();
    const azl::Image full = harness.readPicture(1);

    // A budget no frame fits: the guard halves every layer it may.
    harness.engine().setFrameBudget(1e-6);
    harness.step(90);
    CHECK(harness.stats().budgetLevel == 2);
    // The gradient's red and green are its position, which a half-resolution picture scaled up keeps in place.
    const azl::Image halved = harness.readPicture(1);
    REQUIRE(halved.rgba.size() == full.rgba.size());
    size_t moved = 0;
    for (size_t i = 0; i < full.rgba.size(); i += 4)
        moved += std::abs(int(halved.rgba[i]) - int(full.rgba[i])) > 3 ||
                 std::abs(int(halved.rgba[i + 1]) - int(full.rgba[i + 1])) > 3;
    CHECK(moved <= full.width * full.height / 2000);
    CHECK(halved.rgba != full.rgba);

    for (const char* id : {"proj-a", "proj-b"}) {
        harness.displays().attach(display(id));
        std::this_thread::sleep_for(std::chrono::microseconds(8300));
    }
    std::map<std::string, uint64_t> fresh;
    uint64_t rendered = 0;
    std::thread measure([&] {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        for (azl::DisplayOutput* output : harness.outputs().all())
            fresh[output->display()] = output->stats().newFrames;
        rendered = harness.stats().rendered;
        std::this_thread::sleep_for(std::chrono::seconds(2));
        rendered = harness.stats().rendered - rendered;
        for (azl::DisplayOutput* output : harness.outputs().all())
            fresh[output->display()] = output->stats().newFrames - fresh[output->display()];
    });
    harness.runLive(3.2);
    measure.join();

    CHECK(harness.stats().budgetLevel == 2);
    CHECK(rendered > 110);
    REQUIRE(fresh.size() == 2);
    for (const auto& [id, frames] : fresh) {
        INFO(id << ": " << frames << " new frames of " << rendered << " rendered");
        CHECK(frames + 3 >= rendered);
    }
}

TEST_CASE("with --realtime-frames, render and present threads wake on time from their timed waits")
{
    // Late wakes were most of the laptop's hitches: at user-interactive QoS a timed wait woke about 3 ms late.
    std::vector<double> lateMs;
    azl::realtimeFrames() = true;
    std::thread frames([&] {
        azl::setThreadRole(azl::ThreadRole::Frames);
        auto next = std::chrono::steady_clock::now();
        for (int i = 0; i < 60; ++i) {
            next += std::chrono::microseconds(16667);
            std::this_thread::sleep_until(next);
            lateMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - next).count());
        }
    });
    frames.join();
    azl::realtimeFrames() = false;
    std::sort(lateMs.begin(), lateMs.end());
    INFO("late by " << lateMs[30] << " ms (median), " << lateMs[54] << " ms (90th percentile)");
    CHECK(lateMs[30] < 0.5);
    CHECK(lateMs[54] < 1.5);
}

TEST_CASE("a slowed output does not affect the other outputs or the render rate")
{
    azl::EngineHarness harness(kSize, 60.0, {},
                               stage({screen(1, "proj-a", {slice(1, kKeystone)}),
                                      screen(2, "proj-b", {slice(2, {0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0}, kFull, 10)}),
                                      screen(3, "proj-c", {slice(3, kKeystone, kFull, 1)})}));
    for (const char* id : {"proj-a", "proj-b", "proj-c"})
        harness.displays().attach(display(id));
    harness.outputs().setDelay("proj-b", 50);
    harness.step();

    std::map<std::string, uint64_t> before;
    for (const char* id : {"proj-a", "proj-b", "proj-c"})
        before[id] = presents(harness, id);
    const uint64_t rendered = harness.stats().rendered;
    constexpr double kSeconds = 1.5;
    harness.runLive(kSeconds);

    const double renderRate = double(harness.stats().rendered - rendered) / kSeconds;
    auto rate = [&](const char* id) { return double(presents(harness, id) - before[id]) / kSeconds; };
    INFO("render " << renderRate << " fps, outputs " << rate("proj-a") << ", " << rate("proj-b") << ", "
                   << rate("proj-c") << " presents/s");
    CHECK(renderRate > 50.0);
    CHECK(rate("proj-a") > 50.0);
    CHECK(rate("proj-c") > 50.0);
    CHECK(rate("proj-b") < 25.0);
}

TEST_CASE("only a borderless fullscreen output hides the cursor")
{
    CHECK(azl::hidesCursor(false, 0));
    CHECK_FALSE(azl::hidesCursor(true, 0));
    CHECK_FALSE(azl::hidesCursor(false, 2));
    CHECK_FALSE(azl::hidesCursor(true, 2));
}

TEST_CASE("the cursor hides over an output, again when it comes back, and shows off all outputs")
{
    using azl::CursorStep;
    CHECK(azl::cursorStep(true, false, true) == CursorStep::hide);
    CHECK(azl::cursorStep(true, true, true) == CursorStep::hide);
    CHECK(azl::cursorStep(true, true, false) == CursorStep::none);
    CHECK(azl::cursorStep(false, true, false) == CursorStep::show);
    CHECK(azl::cursorStep(false, true, true) == CursorStep::show);
    CHECK(azl::cursorStep(false, false, true) == CursorStep::none);
}

TEST_CASE("Escape in an output window releases its display and Cmd+Q quits; other keys pass")
{
    using Key = azl::Displays::OutputKey;
    constexpr unsigned short kEscape = 53;
    constexpr unsigned short kQ = 12;
    CHECK(azl::outputKeyFor(kEscape, false, "\x1b") == Key::release);
    CHECK(azl::outputKeyFor(kQ, true, "q") == Key::quit);
    CHECK(azl::outputKeyFor(kQ, true, "Q") == Key::quit);
    CHECK_FALSE(azl::outputKeyFor(kQ, false, "q").has_value());
    CHECK_FALSE(azl::outputKeyFor(kEscape, true, "\x1b").has_value());
    CHECK_FALSE(azl::outputKeyFor(49, false, " ").has_value());
}

// Escape on an output goes through the document, like `nocturne-ctl screendisplay SCREEN none`: the screen keeps its
// slices and masks, its output parks, and the inverse edit gives the display back.
TEST_CASE("a screen released from its display keeps its mapping and takes the display back on the inverse edit")
{
    azl::Composition composition;
    composition.screens.push_back({.id = 7, .display = "window-1", .slices = {{}}});
    azl::Edit release = azl::SetScreenDisplay{7, std::string()};
    azl::Edit inverse = azl::applyEdit(composition, release);
    CHECK(composition.screens[0].display.empty());
    CHECK(composition.screens[0].slices.size() == 1);
    REQUIRE(std::holds_alternative<azl::SetScreenDisplay>(inverse));
    azl::applyEdit(composition, inverse);
    CHECK(composition.screens[0].display == "window-1");
}

namespace {

constexpr uint32_t kEpson = 0x4ca3;

// A projector without a serial number, so its id names the port it is on.
azl::DisplayInfo projector(const std::string& id, uint32_t model, uint32_t unit, uint32_t serial = 0)
{
    return {.id = id,
            .name = "EPSON PJ",
            .width = kSize.width,
            .height = kSize.height,
            .vendor = kEpson,
            .model = model,
            .serial = serial,
            .unit = unit};
}

// Reads messages until a change (op "change") or an outputs message with that op.
Message nextOutputs(Client& client, const std::string& op)
{
    for (;;) {
        const Message m = client.next();
        if (op == "change" ? m["type"] == "change" : m["type"] == "outputs" && m.value("op", "") == op)
            return m;
    }
}

Message listening(Client& client)
{
    client.send({{"type", "outputs"}, {"op", "list"}});
    return nextOutputs(client, "list");
}

Message displayIn(const Message& list, const std::string& id)
{
    for (const Message& d : list["displays"])
        if (d["id"] == id)
            return d;
    FAIL("no display " << id);
    return {};
}

} // namespace

TEST_CASE("displays: macOS mirroring displays that carry different screens is turned off, and clients are told")
{
    azl::EngineHarness harness(kSize, 60.0, {},
                               stage({screen(1, "proj-a", {slice(1, kKeystone)}),
                                      screen(2, "proj-b", {slice(2, kKeystone, kFull, 10)})}));
    for (const char* id : {"laptop", "proj-a", "proj-b"})
        harness.displays().attach(display(id));
    Client client(harness.document());
    CHECK(listening(client)["unmirror"] == true);

    harness.displays().mirror("proj-b", "proj-a");
    REQUIRE(harness.displays().unmirrored().size() == 1);
    CHECK(harness.displays().unmirrored()[0] == std::vector<std::string>{"proj-a", "proj-b"});
    const Message notice = nextOutputs(client, "notice");
    CHECK(notice["kind"] == "unmirrored");
    CHECK(notice["text"] == "Display mirroring turned off: proj-a, proj-b carry different screens");
    CHECK_FALSE(displayIn(nextOutputs(client, "list"), "proj-b").contains("mirrorOf"));
    CHECK(harness.outputs().find("proj-b")->attached());
    auto made = presentsWhileRendering(harness, 0.3);
    CHECK(made["proj-a"] > 5);
    CHECK(made["proj-b"] > 5);

    // The laptop carries no screen, so it may mirror a projector.
    harness.displays().mirror("laptop", "proj-a");
    CHECK(harness.displays().unmirrored().size() == 1);
    CHECK(displayIn(nextOutputs(client, "list"), "laptop")["mirrorOf"] == "proj-a");
}

TEST_CASE("displays: mirroring is left alone with one screen on a display, and while the setting is off")
{
    SUBCASE("one screen")
    {
        azl::EngineHarness harness(kSize, 60.0, {},
                                   stage({screen(1, "proj-a", {slice(1, kKeystone)}), screen(2, "", {slice(2, kKeystone)})}));
        for (const char* id : {"proj-a", "proj-b"})
            harness.displays().attach(display(id));
        harness.displays().mirror("proj-b", "proj-a");
        CHECK(harness.displays().unmirrored().empty());
    }

    SUBCASE("setting off, then on again")
    {
        azl::EngineHarness harness(kSize, 60.0, {},
                                   stage({screen(1, "proj-a", {slice(1, kKeystone)}),
                                          screen(2, "proj-b", {slice(2, kKeystone, kFull, 10)})}));
        for (const char* id : {"proj-a", "proj-b"})
            harness.displays().attach(display(id));
        Client client(harness.document());
        client.send({{"type", "outputs"}, {"op", "unmirror"}, {"on", false}});
        CHECK(nextOutputs(client, "list")["unmirror"] == false);
        CHECK_FALSE(harness.knownDisplays().unmirror());

        harness.displays().mirror("proj-b", "proj-a");
        CHECK(harness.displays().unmirrored().empty());
        CHECK_FALSE(harness.outputs().find("proj-b")->attached());
        CHECK(displayIn(nextOutputs(client, "list"), "proj-b")["mirrorOf"] == "proj-a");

        client.send({{"type", "outputs"}, {"op", "unmirror"}, {"on", true}});
        CHECK(nextOutputs(client, "notice")["kind"] == "unmirrored");
        CHECK(harness.displays().unmirrored().size() == 1);
        CHECK(harness.outputs().find("proj-b")->attached());
    }
}

TEST_CASE("displays: a projector back under a new id takes its screen back; the clients are told")
{
    azl::EngineHarness harness(kSize, 60.0, {},
                               stage({screen(1, "dell", {slice(1, kKeystone)}),
                                      screen(2, "4ca3-a11a-u0", {slice(2, kKeystone, kFull, 10)})}));
    harness.displays().attach(display("dell"));
    harness.displays().attach(projector("4ca3-a11a-u0", 0xa11a, 0));
    Client client(harness.document());
    listening(client);

    SUBCASE("on another port of the splitter")
    {
        harness.displays().replace("4ca3-a11a-u0", projector("4ca3-a11a-u2", 0xa11a, 2));
    }
    SUBCASE("on another port, and mirrored by macOS")
    {
        azl::DisplayInfo mirrored = projector("4ca3-a11a-u2", 0xa11a, 2);
        mirrored.name.clear();
        mirrored.mirrorOf = "dell";
        harness.displays().replace("4ca3-a11a-u0", mirrored);
        REQUIRE(harness.displays().unmirrored().size() == 1);
        CHECK(harness.displays().unmirrored()[0] == std::vector<std::string>{"4ca3-a11a-u2", "dell"});
    }

    const Message change = nextOutputs(client, "change");
    CHECK(change["cause"] == "displayReturned");
    CHECK(harness.document().document().screens[1].display == "4ca3-a11a-u2");
    const Message notice = nextOutputs(client, "notice");
    CHECK(notice["kind"] == "rebound");
    CHECK(notice["screen"] == 2);
    CHECK(notice["from"] == "4ca3-a11a-u0");
    CHECK(notice["to"] == "4ca3-a11a-u2");
    CHECK(notice["text"] == "Screen 2 took back its display EPSON PJ, which came back as 4ca3-a11a-u2 (was 4ca3-a11a-u0)");

    REQUIRE(harness.outputs().find("4ca3-a11a-u2"));
    CHECK(harness.outputs().find("4ca3-a11a-u2")->attached());
    auto made = presentsWhileRendering(harness, 0.3);
    CHECK(made["4ca3-a11a-u2"] > 5);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(same(harness.displays().capture("4ca3-a11a-u2"), harness.readScreen(2)));
}

TEST_CASE("displays: another projector under a new id does not take a screen whose display is gone")
{
    azl::EngineHarness harness(kSize, 60.0, {},
                               stage({screen(1, "dell", {slice(1, kKeystone)}),
                                      screen(2, "4ca3-a11a-u0", {slice(2, kKeystone, kFull, 10)}),
                                      screen(3, "4ca3-a11a-u1", {slice(3, kKeystone, kFull, 1)})}));
    harness.displays().attach(display("dell"));
    harness.displays().attach(projector("4ca3-a11a-u0", 0xa11a, 0));
    std::string gone = "4ca3-a11a-u0";

    SUBCASE("another model")
    {
        harness.displays().replace("4ca3-a11a-u0", projector("4ca3-b22b-u2", 0xb22b, 2));
    }
    SUBCASE("the same model with another serial number")
    {
        harness.document().perform(azl::SetScreenDisplay{2, "4ca3-a11a-1234"}, "test");
        harness.displays().replace("4ca3-a11a-u0", projector("4ca3-a11a-1234", 0xa11a, 0, 0x1234));
        harness.displays().replace("4ca3-a11a-1234", projector("4ca3-a11a-5678", 0xa11a, 0, 0x5678));
        gone = "4ca3-a11a-1234";
    }
    SUBCASE("one of two identical projectors that both went away")
    {
        harness.displays().attach(projector("4ca3-a11a-u1", 0xa11a, 1));
        harness.displays().detach("4ca3-a11a-u0");
        harness.displays().detach("4ca3-a11a-u1");
        harness.displays().attach(projector("4ca3-a11a-u2", 0xa11a, 2));
        CHECK(harness.document().document().screens[2].display == "4ca3-a11a-u1");
    }

    CHECK(harness.document().document().screens[1].display == gone);
    REQUIRE(harness.outputs().find(gone));
    CHECK_FALSE(harness.outputs().find(gone)->attached());
}

TEST_CASE("displays: the machine remembers each display's details and the unmirror setting across restarts")
{
    TempDir dir;
    const fs::path file = dir.path / "displays.json";
    {
        std::ofstream(file) << R"({"displays": ["old-id"]})";
        azl::KnownDisplays known(file);
        CHECK(known.ids() == std::set<std::string>{"old-id"});
        CHECK(known.unmirror());
        known.add({projector("4ca3-a11a-u0", 0xa11a, 0)});
        known.setUnmirror(false);
    }
    azl::KnownDisplays known(file);
    CHECK(known.ids() == std::set<std::string>{"4ca3-a11a-u0", "old-id"});
    CHECK_FALSE(known.unmirror());
    const std::optional<azl::DisplayInfo> epson = known.find("4ca3-a11a-u0");
    REQUIRE(epson);
    CHECK(epson->name == "EPSON PJ");
    CHECK(epson->vendor == kEpson);
    CHECK(epson->model == 0xa11a);

    // Mirroring, it reports no name of its own; the one it had stays.
    azl::DisplayInfo mirrored = projector("4ca3-a11a-u0", 0xa11a, 0);
    mirrored.name.clear();
    mirrored.mirrorOf = "dell";
    known.add({mirrored});
    CHECK(known.find("4ca3-a11a-u0")->name == "EPSON PJ");
}

TEST_CASE("map arrangement: position, hidden flag and order round-trip through the protocol and the file, one undo "
          "step each")
{
    azl::EngineHarness harness(kSize, kStillFps, {},
                               stage({screen(1, "", {slice(1, kKeystone)}), screen(2, "", {slice(2, kKeystone)}),
                                      screen(3, "", {slice(3, kKeystone)})}));
    Client client(harness.document());
    const azl::Composition before = harness.document().document();

    for (const azl::Edit& edit : {azl::Edit{azl::SetScreenPosition{2, std::array{120.5, -40.0}}},
                                  azl::Edit{azl::SetScreenHidden{3, true}}, azl::Edit{azl::MoveScreen{3, 0}}}) {
        const Message change = apply(client, edit);
        CHECK(azl::editToJson(azl::editFromJson(change)) == azl::editToJson(edit));
    }
    const azl::Composition arranged = harness.document().document();
    CHECK(arranged.screens[2].mapPosition == std::array{120.5, -40.0});
    CHECK(arranged.screens[0].id == 3);
    CHECK(arranged.screens[0].hidden);
    CHECK(!arranged.screens[1].mapPosition);

    TempDir dir;
    azl::saveCompositionFile(dir.path / "show.json", {.composition = arranged});
    const std::optional<azl::CompositionFile> loaded = azl::loadCompositionFile(dir.path / "show.json");
    REQUIRE(loaded);
    CHECK(loaded->composition.screens == arranged.screens);

    std::vector<azl::Composition> steps;
    for (int i = 0; i < 3; ++i) {
        client.send(msg::undo());
        CHECK(client.next()["type"] == "change");
        steps.push_back(harness.document().document());
    }
    CHECK(steps[0].screens[0].id == 1);
    CHECK(steps[0].screens[2].hidden);
    CHECK(!steps[1].screens[2].hidden);
    CHECK(steps[1].screens[1].mapPosition);
    CHECK(steps[2].screens == before.screens);
    for (int i = 0; i < 3; ++i) {
        client.send(msg::redo());
        CHECK(client.next()["type"] == "change");
    }
    CHECK(harness.document().document().screens == arranged.screens);
    apply(client, azl::SetScreenPosition{2, std::nullopt});
    CHECK(!harness.document().document().screens[2].mapPosition);

    CHECK(command(client, azl::SetScreenPosition{9, std::array{0.0, 0.0}})["type"] == "error");
    CHECK(command(client, azl::SetScreenPosition{1, std::array{std::nan(""), 0.0}})["type"] == "error");
    CHECK(command(client, azl::SetScreenHidden{9, true})["type"] == "error");
    CHECK(command(client, azl::MoveScreen{1, 3})["type"] == "error");
    CHECK(command(client, azl::MoveScreen{9, 0})["type"] == "error");
}

TEST_CASE("map arrangement: an old file has no positions and saves back without them")
{
    const nlohmann::json old = {{"id", 4}, {"name", "TV"}, {"display", "tv"}, {"slices", nlohmann::json::array()}};
    const azl::Screen loaded = old.get<azl::Screen>();
    CHECK(!loaded.mapPosition);
    CHECK(!loaded.hidden);
    const nlohmann::json saved = loaded;
    CHECK(!saved.contains("mapPosition"));
    CHECK(!saved.contains("hidden"));

    azl::Screen moved = loaded;
    moved.mapPosition = std::array{10.0, 20.0};
    moved.hidden = true;
    const nlohmann::json movedJson = moved;
    CHECK(movedJson["mapPosition"] == nlohmann::json::array({10.0, 20.0}));
    CHECK(movedJson["hidden"] == true);
    CHECK(movedJson.get<azl::Screen>() == moved);
}

TEST_CASE("map arrangement: a hidden screen's output keeps presenting the same picture; reordering keeps the main "
          "output")
{
    azl::EngineHarness harness(kSize, 60.0, {},
                               stage({screen(1, "proj-a", {slice(1, kKeystone)}),
                                      screen(2, "proj-b", {slice(2, {0.1, 0.1, 0.9, 0.0, 1.0, 1.0, 0.0, 0.9}, kFull, 1)})}));
    for (const char* id : {"proj-a", "proj-b"})
        harness.displays().attach(display(id));
    Client client(harness.document());
    auto made = presentsWhileRendering(harness, 0.3);
    CHECK(made["proj-b"] > 5);
    const azl::Image shown = harness.readScreen(2);

    apply(client, azl::SetScreenHidden{2, true});
    made = presentsWhileRendering(harness, 0.3);
    CHECK(made["proj-a"] > 5);
    CHECK(made["proj-b"] > 5);
    CHECK(same(harness.readScreen(2), shown));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(same(harness.displays().capture("proj-b"), shown));

    client.send(msg::undo());
    CHECK(client.next()["type"] == "change");
    CHECK(!harness.document().document().screens[1].hidden);
    CHECK(presentsWhileRendering(harness, 0.2)["proj-b"] > 3);

    CHECK(mainOutput(harness) == "proj-a");
    apply(client, azl::MoveScreen{2, 0});
    harness.step();
    CHECK(harness.outputs().all().front()->display() == "proj-b");
    CHECK(mainOutput(harness) == "proj-a");
    CHECK(presentsWhileRendering(harness, 0.2)["proj-a"] > 3);
}

TEST_CASE("map arrangement: nocturne-ctl screen move, hide and order")
{
    azl::EngineHarness harness(kSize, kStillFps, {},
                               stage({screen(1, "", {slice(1, kKeystone)}), screen(2, "", {slice(2, kKeystone)})}));
    azl::WebSocketServer server(harness.document(), 0);
    auto ctl = [&](const std::string& args) {
        const fs::path binary = ctlPath();
        const std::string command =
            "'" + binary.string() + "' --port " + std::to_string(server.port()) + " " + args + " >/dev/null 2>&1";
        const int status = std::system(command.c_str());
        return WEXITSTATUS(status);
    };
    CHECK(ctl("screen move 2 300 -12.5") == 0);
    CHECK(ctl("screen hide 1 on") == 0);
    CHECK(ctl("screen order 2 0") == 0);
    azl::Composition c = harness.document().document();
    REQUIRE(c.screens.size() == 2);
    CHECK(c.screens[0].id == 2);
    CHECK(c.screens[0].mapPosition == std::array{300.0, -12.5});
    CHECK(c.screens[1].hidden);
    CHECK(ctl("screen move 2 grid") == 0);
    CHECK(ctl("screen hide 1 off") == 0);
    c = harness.document().document();
    CHECK(!c.screens[0].mapPosition);
    CHECK(!c.screens[1].hidden);
    CHECK(ctl("screen hide 1 maybe") == 2);
    CHECK(ctl("screen spin 1 2") != 0);
}

namespace {

azl::Screen named(uint64_t id, std::string name, std::string display)
{
    return {.id = id, .name = std::move(name), .display = std::move(display), .slices = {{.id = id}}};
}

using Displays = std::vector<std::string>;

Displays displaysOf(const azl::Composition& composition)
{
    Displays displays;
    for (const azl::Screen& s : composition.screens)
        displays.push_back(s.display);
    return displays;
}

Message loadShow(azl::EngineHarness& harness, const fs::path& show)
{
    Client client(harness.document());
    client.send({{"type", "composition"}, {"op", "load"}, {"path", show.string()}});
    const Message reply = client.next();
    REQUIRE(reply["type"] == "composition");
    REQUIRE(client.next()["op"] == "loadComposition");
    return reply;
}

} // namespace

TEST_CASE("displays per machine: a mapped role takes its display; the others keep theirs unless it is taken")
{
    azl::Composition c{.screens = {named(1, "Main", "show-a"), named(2, "Side", "show-b"), named(3, "Main", "show-c"),
                                   named(4, "Spare", "m1")}};
    std::vector<std::string> warnings;
    azl::placeByRole(c, {{"Main", {"m1", "m2"}}, {"Side", {}}}, warnings);
    CHECK(displaysOf(c) == Displays{"m1", "show-b", "m2", ""});
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("Spare") != std::string::npos);

    // A display two roles name goes to the first screen; an empty one means no output.
    c = {.screens = {named(1, "Main", "show-a"), named(2, "Stage", "show-b"), named(3, "Off", "show-c")}};
    warnings.clear();
    azl::placeByRole(c, {{"Main", {"d1"}}, {"Stage", {"d1"}}, {"Off", {""}}}, warnings);
    CHECK(displaysOf(c) == Displays{"d1", "show-b", ""});
    CHECK(warnings.empty());
    CHECK(azl::screenRole(azl::Screen{.id = 7}) == "Screen 7");
}

TEST_CASE("displays per machine: the map records where the screens are and keeps every other role")
{
    azl::RoleDisplays roles{{"Other", {"x"}}, {"Main", {"m1", "m2"}}};
    const azl::Composition c{.screens = {named(1, "Main", "p1"), named(2, "Fresh", ""), named(3, "New", "n1")}};
    CHECK(azl::recordRoles(roles, c));
    CHECK(roles == azl::RoleDisplays{{"Other", {"x"}}, {"Main", {"p1", "m2"}}, {"New", {"n1"}}});
    CHECK(!azl::recordRoles(roles, c));
    // A mapped role keeps "no output".
    CHECK(azl::recordRoles(roles, azl::Composition{.screens = {named(1, "New", "")}}));
    CHECK(roles["New"] == Displays{""});
}

TEST_CASE("displays per machine: an old show's display ids enter the machine's map on load, nothing moves")
{
    TempDir dir;
    const fs::path show = dir.path / "old.json";
    azl::saveCompositionFile(show, {stage({named(1, "Left", "proj-a"), named(2, "Right", "proj-b")})});
    azl::EngineHarness harness(kSize, kStillFps, dir.path / "machine");
    for (const char* id : {"proj-a", "proj-b"})
        harness.displays().attach(display(id));
    CHECK(loadShow(harness, show)["warnings"].empty());
    CHECK(displaysOf(harness.document().document()) == Displays{"proj-a", "proj-b"});
    CHECK(harness.screenDisplays().roles() == azl::RoleDisplays{{"Left", {"proj-a"}}, {"Right", {"proj-b"}}});
    CHECK(harness.outputs().find("proj-a")->attached());
    std::ifstream in(dir.path / "machine/screen-displays.json");
    CHECK(Message::parse(in)["roles"]["Left"] == Message::array({"proj-a"}));
}

TEST_CASE("displays per machine: the same show on two machines lands on each machine's own displays")
{
    TempDir dir;
    const fs::path show = dir.path / "show.json";
    azl::saveCompositionFile(show, {stage({named(1, "Left", "laptop-1"), named(2, "Right", "laptop-2")})});
    const auto machine = [&](const std::string& name, const std::string& left) {
        auto harness =
            std::make_unique<azl::EngineHarness>(kSize, kStillFps, dir.path / name, stage({named(1, "Left", left)}));
        for (const std::string& id : {left, name + "-spare"})
            harness->displays().attach(display(id));
        return harness;
    };
    auto a = machine("a", "a-dell");
    auto b = machine("b", "b-projector");
    loadShow(*a, show);
    loadShow(*b, show);
    // Left is mapped on each machine. Right is not: it moves to the free display as before, and is mapped from now on.
    CHECK(displaysOf(a->document().document()) == Displays{"a-dell", "a-spare"});
    CHECK(displaysOf(b->document().document()) == Displays{"b-projector", "b-spare"});
    CHECK(a->screenDisplays().roles() == azl::RoleDisplays{{"Left", {"a-dell"}}, {"Right", {"a-spare"}}});
    CHECK(b->screenDisplays().roles() == azl::RoleDisplays{{"Left", {"b-projector"}}, {"Right", {"b-spare"}}});
}

TEST_CASE("displays per machine: a move writes the machine's map, not the show, and a restart keeps it")
{
    TempDir dir;
    const fs::path show = dir.path / "show.json";
    const azl::Composition authored = stage({named(1, "Screen 1", "proj-a")});
    azl::saveCompositionFile(show, {authored});
    {
        azl::EngineHarness harness(kSize, kStillFps, dir.path / "machine", authored);
        harness.document().markSaved(show.string());
        Client client(harness.document());
        client.send({{"type", "outputs"}, {"op", "list"}});
        CHECK(client.next()["roles"] == Message({{"Screen 1", {"proj-a"}}}));
        const Message change = apply(client, azl::SetScreenDisplay{1, "proj-b"});
        CHECK(change["history"]["undo"] == 0);
        CHECK(harness.document().file() == azl::ShowFile{show.string(), false});
        CHECK(harness.screenDisplays().roles() == azl::RoleDisplays{{"Screen 1", {"proj-b"}}});
    }
    azl::EngineHarness restarted(kSize, kStillFps, dir.path / "machine", authored);
    CHECK(restarted.document().document().screens[0].display == "proj-b");
    loadShow(restarted, show);
    CHECK(restarted.document().document().screens[0].display == "proj-b");
}

TEST_CASE("displays per machine: nocturne-ctl screendisplay moves the screen on this machine only")
{
    azl::EngineHarness harness(kSize, kStillFps, {}, stage({named(1, "Main", "proj-a")}));
    azl::WebSocketServer server(harness.document(), 0);
    const fs::path binary = ctlPath();
    const std::string command = "'" + binary.string() + "' --port " + std::to_string(server.port()) +
                                " screendisplay 1 proj-b >/dev/null 2>&1";
    const int status = std::system(command.c_str());
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(harness.document().document().screens[0].display == "proj-b");
    CHECK(harness.screenDisplays().roles() == azl::RoleDisplays{{"Main", {"proj-b"}}});
    Client client(harness.document());
    CHECK(client.snapshot["history"]["undo"] == 0);
}

TEST_CASE("displays per machine: the gig show's screen stays on the Dell after the upgrade and after a reload")
{
    TempDir dir;
    const std::string dell = "10ac-43b0-42393442";
    const fs::path state = dir.path / "composition.json";
    const fs::path show = dir.path / "gig.json";
    // The state and the show file as the build before the map saved them.
    const azl::Composition gig = stage({named(1, "Screen 1", dell)});
    azl::saveCompositionFile(state, {gig});
    azl::saveCompositionFile(show, {gig});
    for (int start = 0; start < 2; ++start) {
        CAPTURE(start);
        azl::EngineHarness harness(kSize, 60.0, dir.path / "mini", azl::loadCompositionFile(state)->composition);
        harness.displays().attach(display(dell));
        harness.displays().attach(display("projector"));
        CHECK(displaysOf(harness.document().document()) == Displays{dell});
        CHECK(presentsWhileRendering(harness, 0.2)[dell] > 3);
        CHECK(harness.screenDisplays().roles() == azl::RoleDisplays{{"Screen 1", {dell}}});
        loadShow(harness, show);
        CHECK(displaysOf(harness.document().document()) == Displays{dell});
        azl::saveCompositionFile(state, {harness.document().document()});
    }
}
