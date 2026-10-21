#include "golden.hpp"
#include "protocol/document_mirror.hpp"
#include "test_client.hpp"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

using azl::BlendMode;

constexpr azl::EngineConfig kSize{.width = 480, .height = 270};
// Keeps the clock near zero, so the test card's moving marker stays put and goldens do not depend on frame count.
constexpr double kStillFps = 1e6;

// The test card's yellow bar at 480x270, and the colours Colorize gives it at hue 0, 1/3 and 2/3.
constexpr uint32_t kYellowX = 127;
constexpr uint32_t kYellowY = 45;
constexpr int kTinted = 177;

// Flips the picture upside down onto exact pixel centres, so the test card's colour bars land on its grey ramp.
azl::EffectDef flip()
{
    return {"testFlip", azl::StageKind::Remap, {}, false, "\n    return float2(px.x, h.size.y - px.y);\n", {}};
}

void registerFlip()
{
    if (!azl::findEffect("testFlip"))
        azl::registerEffect(flip());
}

azl::Effect colorize(uint64_t id, double hue, double saturation = 1.0)
{
    return {id, "colorize", {{"hue", hue}, {"saturation", saturation}}};
}

azl::Layer flippedLayer(uint64_t id, double opacity = 1.0)
{
    return {.id = id, .name = "Flipped", .opacity = opacity, .effects = {{id * 10, "testFlip", {}}, colorize(id * 10 + 1, 0.6, 0.6)}};
}

// The plain test card below a flipped, tinted one at 80% opacity.
azl::Composition blendStack()
{
    registerFlip();
    return {{{.id = 1, .name = "Below"}, flippedLayer(2, 0.8)}};
}

azl::Image render(azl::EngineHarness& harness)
{
    harness.step();
    return harness.readScreen();
}

void checkRgb(const azl::Image& image, uint32_t x, uint32_t y, int r, int g, int b, int tolerance = 3)
{
    const uint8_t* p = pixel(image, x, y);
    INFO("pixel " << x << "," << y << " is " << int(p[0]) << "," << int(p[1]) << "," << int(p[2]));
    CHECK(std::abs(p[0] - r) <= tolerance);
    CHECK(std::abs(p[1] - g) <= tolerance);
    CHECK(std::abs(p[2] - b) <= tolerance);
}

float hardLight(float b, float s)
{
    return s <= 0.5f ? b * 2.0f * s : b + (2.0f * s - 1.0f) - b * (2.0f * s - 1.0f);
}

float softLight(float b, float s)
{
    if (s <= 0.5f)
        return b - (1.0f - 2.0f * s) * b * (1.0f - b);
    const float d = b <= 0.25f ? ((16.0f * b - 12.0f) * b + 4.0f) * b : std::sqrt(b);
    return b + (2.0f * s - 1.0f) * (d - b);
}

// The separable blend functions of the W3C Compositing spec, and Resolume's add and subtract.
float blendChannel(BlendMode mode, float b, float s)
{
    switch (mode) {
    case BlendMode::Normal: return s;
    case BlendMode::Add: return b + s;
    case BlendMode::Screen: return b + s - b * s;
    case BlendMode::Multiply: return b * s;
    case BlendMode::Difference: return std::abs(b - s);
    case BlendMode::Subtract: return b - s;
    case BlendMode::Exclusion: return b + s - 2.0f * b * s;
    case BlendMode::Lighten: return std::max(b, s);
    case BlendMode::Darken: return std::min(b, s);
    case BlendMode::Overlay: return hardLight(s, b);
    case BlendMode::HardLight: return hardLight(b, s);
    case BlendMode::SoftLight: return softLight(b, s);
    case BlendMode::Dodge: return b <= 0.0f ? 0.0f : s >= 1.0f ? 1.0f : b / (1.0f - s);
    case BlendMode::Burn: return b >= 1.0f ? 1.0f : s <= 0.0f ? 0.0f : 1.0f - (1.0f - b) / s;
    }
    return s;
}

// Over an opaque picture a layer of opacity o gives mix(below, blend, o); add brightens by o times the layer.
float expected(BlendMode mode, float b, float s, float opacity)
{
    if (mode == BlendMode::Add)
        return std::min(b + s * opacity, 1.0f);
    return b + (std::clamp(blendChannel(mode, b, s), 0.0f, 1.0f) - b) * opacity;
}

// True when `actual` is what the blend gives for some below and layer values that round to the given 8-bit ones.
bool matches(BlendMode mode, int below, int layer, float opacity, int actual)
{
    float lo = 1.0f, hi = 0.0f;
    for (float db : {-0.5f, 0.5f})
        for (float ds : {-0.5f, 0.5f}) {
            const float b = std::clamp((below + db) / 255.0f, 0.0f, 1.0f);
            const float s = std::clamp((layer + ds) / 255.0f, 0.0f, 1.0f);
            const float v = expected(mode, b, s, opacity);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    return actual >= lo * 255.0f - 2.5f && actual <= hi * 255.0f + 2.5f;
}

void checkBlend(BlendMode mode, const azl::Image& below, const azl::Image& layer, float opacity,
                const azl::Image& actual)
{
    int failures = 0;
    for (uint32_t y = 1; y < actual.height; y += 7)
        for (uint32_t x = 1; x < actual.width; x += 7)
            for (int c = 0; c < 3; ++c) {
                const int b = pixel(below, x, y)[c], s = pixel(layer, x, y)[c], a = pixel(actual, x, y)[c];
                if (matches(mode, b, s, opacity, a))
                    continue;
                if (++failures <= 3)
                    MESSAGE(std::string(azl::blendModeName(mode)) << " at " << x << "," << y << " channel " << c << ": below " << b
                                                     << ", layer " << s << ", got " << a);
            }
    CHECK_MESSAGE(failures == 0, std::string(azl::blendModeName(mode)) << ": " << failures << " channels off");
}

std::vector<BlendMode> allBlendModes()
{
    std::vector<BlendMode> modes;
    for (int i = 0; i < azl::kBlendModeCount; ++i)
        modes.push_back(BlendMode(i));
    return modes;
}

std::string goldenName(BlendMode mode)
{
    return std::string("blend_") + azl::blendModeName(mode);
}

// Bottom to top: red, green and blue tints of the test card, so the yellow bar tells which layer shows.
azl::Composition rgbStack()
{
    return {{{.id = 1, .name = "Red", .effects = {colorize(1, 0.0)}},
             {.id = 2, .name = "Green", .effects = {colorize(2, 1.0 / 3.0)}},
             {.id = 3, .name = "Blue", .effects = {colorize(3, 2.0 / 3.0)}}}};
}

void checkShows(azl::EngineHarness& harness, int r, int g, int b)
{
    checkRgb(render(harness), kYellowX, kYellowY, r, g, b);
}

} // namespace

TEST_CASE("each blend mode composites as its formula and matches its golden")
{
    azl::EngineHarness harness(kSize, kStillFps, {}, blendStack());
    Client client(harness.document());
    apply(client, azl::SetLayerBypass{2, true});
    const azl::Image below = render(harness);
    apply(client, azl::SetLayerBypass{2, false});
    apply(client, azl::SetLayerBypass{1, true});
    apply(client, azl::SetLayerOpacity{2, 1.0});
    const azl::Image layer = render(harness);
    apply(client, azl::SetLayerBypass{1, false});
    apply(client, azl::SetLayerOpacity{2, 0.8});

    for (BlendMode mode : allBlendModes()) {
        apply(client, azl::SetLayerBlend{2, mode});
        const azl::Image image = render(harness);
        checkBlend(mode, below, layer, 0.8f, image);
        checkGolden(image, goldenName(mode));
    }
}

TEST_CASE("switching blend modes never compiles, never waits and never blanks the layer")
{
    azl::EngineHarness harness(kSize, kStillFps, {}, blendStack());
    Client client(harness.document());
    harness.step();
    const uint64_t pipelines = harness.compileStats().pipelines;

    harness.holdCompiles(true);
    for (BlendMode mode : allBlendModes()) {
        apply(client, azl::SetLayerBlend{2, mode});
        const uint64_t rendered = harness.stats().rendered;
        const azl::Image image = render(harness);
        CHECK(harness.stats().rendered == rendered + 1);
        checkGolden(image, goldenName(mode));
    }
    CHECK(harness.stats().skipped == 0);
    CHECK(harness.compileStats().pipelines == pipelines);
    harness.holdCompiles(false);
}

TEST_CASE("a group renders its layers as one picture and blends it as one layer")
{
    registerFlip();
    azl::Layer multiplied{.id = 12, .name = "Multiplied", .opacity = 0.9, .effects = {colorize(120, 0.3)},
                          .blend = BlendMode::Multiply};
    azl::Layer group{.id = 10, .name = "Group", .opacity = 0.8, .blend = BlendMode::Difference, .group = true,
                     .layers = {flippedLayer(11), multiplied}};
    azl::EngineHarness harness(kSize, kStillFps, {}, azl::Composition{{{.id = 1, .name = "Below"}, group}});
    harness.step();
    checkGolden(harness.readScreen(), "group");
    // The layer below, two layers into the group's picture, a copy for the difference and the group's pass.
    CHECK(harness.stats().passes == 5);
}

TEST_CASE("a group's opacity applies to its whole picture, and its effects to everything in it")
{
    azl::Layer group{.id = 10, .name = "Group", .group = true,
                     .layers = {{.id = 11, .name = "Red", .effects = {colorize(110, 0.0)}},
                                {.id = 12, .name = "Blue", .effects = {colorize(120, 2.0 / 3.0)}}}};
    azl::EngineHarness harness(kSize, kStillFps, {},
                               azl::Composition{{{.id = 1, .name = "Green", .effects = {colorize(1, 1.0 / 3.0)}}, group}});
    Client client(harness.document());
    checkShows(harness, 0, 0, kTinted);

    // Blue covers red inside the group, so half the group is half blue over green, with no red in it.
    apply(client, azl::SetLayerOpacity{10, 0.5});
    checkShows(harness, 0, 89, 89);

    apply(client, azl::SetLayerOpacity{10, 1.0});
    apply(client, azl::AddEffect{10, std::nullopt, colorize(0, 0.0, 0.0)});
    harness.step();
    // Grey from the group's blue: Colorize at zero saturation keeps luminance.
    checkShows(harness, 13, 13, 13);
    CHECK(harness.stats().passes == 4);
}

TEST_CASE("opacity, bypass and solo choose what shows")
{
    azl::EngineHarness harness(kSize, kStillFps, {}, rgbStack());
    Client client(harness.document());
    checkShows(harness, 0, 0, kTinted);

    apply(client, azl::SetLayerOpacity{3, 0.5});
    checkShows(harness, 0, 89, 89);
    apply(client, azl::SetLayerOpacity{3, 1.0});

    apply(client, azl::SetLayerBypass{3, true});
    checkShows(harness, 0, kTinted, 0);
    apply(client, azl::SetLayerBypass{3, false});

    // A soloed layer shows alone, whatever is above it; several soloed layers stack as usual.
    apply(client, azl::SetLayerSolo{1, true});
    checkShows(harness, kTinted, 0, 0);
    apply(client, azl::SetLayerSolo{2, true});
    checkShows(harness, 0, kTinted, 0);
    // Bypass wins over solo.
    apply(client, azl::SetLayerBypass{2, true});
    checkShows(harness, kTinted, 0, 0);
    apply(client, azl::SetLayerBypass{2, false});
    apply(client, azl::SetLayerSolo{1, false});
    apply(client, azl::SetLayerSolo{2, false});

    // Green and blue in a group above red.
    apply(client, azl::AddLayer{0, std::nullopt, {.id = 10, .group = true}});
    apply(client, azl::MoveLayer{2, 10, 0});
    apply(client, azl::MoveLayer{3, 10, 1});
    checkShows(harness, 0, 0, kTinted);

    // A soloed layer in a group shows its group with only that layer in it.
    apply(client, azl::SetLayerSolo{2, true});
    checkShows(harness, 0, kTinted, 0);
    // A soloed group shows all its layers, unless some of them are soloed too.
    apply(client, azl::SetLayerSolo{10, true});
    checkShows(harness, 0, kTinted, 0);
    apply(client, azl::SetLayerSolo{2, false});
    checkShows(harness, 0, 0, kTinted);
    apply(client, azl::SetLayerSolo{10, false});

    apply(client, azl::SetLayerBypass{10, true});
    checkShows(harness, kTinted, 0, 0);
}

TEST_CASE("fixed-function blend modes add no pass; shader modes add one copy")
{
    registerFlip();
    azl::Composition stack;
    const BlendMode modes[] = {BlendMode::Normal, BlendMode::Add, BlendMode::Screen, BlendMode::Multiply,
                               BlendMode::Normal};
    for (uint64_t i = 1; i <= 5; ++i) {
        azl::Layer layer{.id = i, .name = "Layer", .opacity = 0.8, .blend = modes[i - 1]};
        if (i % 2)
            layer.effects = {{i * 10, "transform", {{"scale", 0.9}, {"rotation", 10.0}}}, colorize(i * 10 + 1, 0.2)};
        stack.layers.push_back(layer);
    }
    azl::EngineHarness harness(kSize, kStillFps, {}, stack);
    Client client(harness.document());
    harness.step();
    CHECK(harness.stats().passes == 5);

    apply(client, azl::SetLayerBlend{4, BlendMode::Difference});
    harness.step();
    CHECK(harness.stats().passes == 6);
}

TEST_CASE("layers and groups are added, moved and removed through the protocol, with undo")
{
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    const Message initial = client.snapshot["composition"];

    const Message added = command(client, azl::AddLayer{0, std::nullopt, {}});
    REQUIRE(added["op"] == "addLayer");
    CHECK(added["index"] == 1);
    CHECK(added["layer"]["id"] == 2);
    CHECK(added["layer"]["name"] == "Layer 2");
    CHECK(added["layer"]["blend"] == "normal");

    const Message group = command(client, azl::AddLayer{0, 0, {.group = true}});
    CHECK(group["layer"]["id"] == 3);
    CHECK(group["layer"]["name"] == "Group 3");
    CHECK(group["layer"]["layers"].empty());

    apply(client, azl::MoveLayer{1, 3, 0});
    apply(client, azl::MoveLayer{2, 3, 1});
    apply(client, azl::SetLayerBlend{3, BlendMode::Screen});
    apply(client, azl::SetLayerBypass{2, true});
    apply(client, azl::SetLayerSolo{1, true});
    apply(client, azl::AddEffect{1, std::nullopt, colorize(0, 0.5)});

    Client late(harness.document());
    const Message& layers = late.snapshot["composition"]["layers"];
    REQUIRE(layers.size() == 1);
    CHECK(layers[0]["group"] == true);
    CHECK(layers[0]["blend"] == "screen");
    REQUIRE(layers[0]["layers"].size() == 2);
    CHECK(layers[0]["layers"][0]["id"] == 1);
    CHECK(layers[0]["layers"][0]["solo"] == true);
    CHECK(layers[0]["layers"][1]["bypass"] == true);

    CHECK(command(client, azl::MoveLayer{3, 3, 0})["type"] == "error");
    CHECK(command(client, azl::AddLayer{3, std::nullopt, {.group = true}})["type"] == "error");
    CHECK(command(client, azl::AddLayer{1, std::nullopt, {}})["type"] == "error");
    CHECK(command(client, azl::MoveLayer{1, 0, 5})["type"] == "error");
    CHECK(command(client, azl::SetLayerClip{3, azl::ClipRef{.hash = std::string(64, 'a'), .name = "clip"}})["type"] == "error");
    client.send({{"type", "command"}, {"op", "setLayerBlend"}, {"layer", 1}, {"blend", "glow"}});
    CHECK(client.next()["type"] == "error");

    // Removing the group takes its layers and their effects along; undo brings them all back.
    apply(client, azl::RemoveLayer{3});
    harness.step();
    Client empty(harness.document());
    CHECK(empty.snapshot["composition"]["layers"].empty());
    client.send(msg::undo());
    const Message restored = client.next();
    CHECK(restored["op"] == "addLayer");
    CHECK(restored["layer"]["layers"][0]["effects"][0]["type"] == "colorize");
    Client back(harness.document());
    CHECK(back.snapshot["composition"] == late.snapshot["composition"]);

    for (int i = 0; i < 8; ++i) {
        client.send(msg::undo());
        REQUIRE(client.next()["type"] == "change");
    }
    Client start(harness.document());
    CHECK(start.snapshot["composition"] == initial);

    // A mirror replaying the redo changes ends with the server's document.
    azl::DocumentMirror mirror;
    mirror.receive(start.snapshot);
    for (int i = 0; i < 9; ++i) {
        client.send(msg::redo());
        const Message change = client.next();
        REQUIRE(change["type"] == "change");
        CHECK_FALSE(mirror.receive(change));
    }
    Client end(harness.document());
    CHECK(end.snapshot["composition"] == empty.snapshot["composition"]);
    CHECK(Message(mirror.composition()) == end.snapshot["composition"]);
}

TEST_CASE("a clip in a group keeps playing while its group is bypassed")
{
    TempDir media;
    const fs::path clip = copyMedia("count_709_tv.mp4", media.path / "count.mp4");
    const azl::Composition grouped{{{.id = 10, .name = "Group", .group = true, .layers = {{.id = 1, .name = "Clip"}}}}};
    auto play = [&](bool bypassMidway) {
        azl::EngineHarness harness(kSize, 60.0, {}, grouped);
        Client client(harness.document());
        playMedia(client, clip);
        harness.step(10);
        const azl::Image early = harness.readScreen();
        if (bypassMidway)
            apply(client, azl::SetLayerBypass{10, true});
        harness.step(30);
        apply(client, azl::SetLayerBypass{10, false});
        const azl::Image late = render(harness);
        CHECK(late.rgba != early.rgba);
        return late;
    };
    const azl::Image watched = play(false);
    const azl::Image hidden = play(true);
    CHECK(watched.rgba == hidden.rgba);
}
