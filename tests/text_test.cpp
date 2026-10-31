#include "clips.hpp"
#include "document/composition_file.hpp"
#include "document/text_edits.hpp"
#include "golden.hpp"
#include "media/composition_media.hpp"
#include "media/text_renderer.hpp"

#include <doctest.h>

#include <string>

namespace {

using azl::ClipRef;
using azl::SetClipText;
using azl::TextAlign;
using azl::TextClip;
using azl::TextParam;

// Helvetica ships with every macOS since 10.0, on Apple silicon and Intel alike.
constexpr azl::EngineConfig kSize{.width = 960, .height = 540};

ClipRef textClip(TextClip text)
{
    return {.name = "Text", .text = std::move(text)};
}

TextClip title()
{
    return {.text = "Nocturne\nText clip", .size = 0.16, .color = {1.0, 0.85, 0.2, 1.0}, .outline = 0.06,
            .outlineColor = {0.6, 0.0, 0.3, 1.0}};
}

void setSlot(Client& client, uint64_t layer, uint64_t column, ClipRef clip)
{
    apply(client, azl::SetSlot{layer, column, std::move(clip)});
}

void trigger(Client& client, uint64_t layer, uint64_t column)
{
    client.send({{"type", "trigger"}, {"op", "clip"}, {"layer", layer}, {"column", column}});
    REQUIRE(client.next()["op"] == "triggerClip");
}

// Pixels brighter than the black background.
size_t lit(const azl::Image& image)
{
    size_t count = 0;
    for (size_t i = 0; i < image.rgba.size(); i += 4)
        count += image.rgba[i] + image.rgba[i + 1] + image.rgba[i + 2] > 30;
    return count;
}

// Pixels of red text over black at `level` (0..255) of its full strength.
size_t red(const azl::Image& image, int level)
{
    size_t count = 0;
    for (size_t i = 0; i < image.rgba.size(); i += 4)
        count += std::abs(image.rgba[i] - level) < 20 && image.rgba[i + 1] < 15 && image.rgba[i + 2] < 15;
    return count;
}

bool same(const azl::Image& a, const azl::Image& b)
{
    return a.rgba == b.rgba;
}

azl::Composition twoLayers()
{
    return {.layers = {{.id = 1, .name = "Layer 1"}, {.id = 2, .name = "Layer 2"}}};
}

} // namespace

TEST_CASE("a text clip validates, serialises and is edited field by field with undo")
{
    azl::Composition composition = azl::defaultComposition();
    azl::Edit bad = azl::SetSlot{1, 1, ClipRef{.hash = kUnknownHash, .name = "x", .text = TextClip{}}};
    CHECK_THROWS_AS(azl::applyEdit(composition, bad), azl::ProtocolError);
    azl::Edit tooBig = azl::SetSlot{1, 1, textClip({.size = 2.0})};
    CHECK_THROWS_AS(azl::applyEdit(composition, tooBig), azl::ProtocolError);

    const ClipRef clip = textClip(title());
    const nlohmann::json j = clip;
    CHECK(j["text"]["align"] == "center");
    CHECK(j.get<ClipRef>() == clip);
    CHECK(azl::editFromJson(azl::editToJson(azl::SetSlot{1, 1, clip})).index() == azl::Edit(azl::SetSlot{}).index());

    azl::Edit set = azl::SetSlot{1, 1, clip};
    azl::applyEdit(composition, set);
    azl::Edit change = azl::editFromJson(
        {{"op", "setClipText"}, {"layer", 1}, {"column", 1}, {"text", "Hello"}, {"align", "right"}, {"x", 0.9}});
    azl::Edit inverse = azl::applyEdit(composition, change);
    const TextClip& text = *composition.layers[0].clips.at(1).text;
    CHECK(text.text == "Hello");
    CHECK(text.align == TextAlign::Right);
    CHECK(text.x == 0.9);
    CHECK(text.size == title().size);
    azl::applyEdit(composition, inverse);
    CHECK(*composition.layers[0].clips.at(1).text == title());

    azl::Edit outOfRange = SetClipText{.layer = 1, .column = 1, .color = std::array{1.0, 2.0, 0.0, 1.0}};
    CHECK_THROWS_AS(azl::applyEdit(composition, outOfRange), azl::ProtocolError);
    CHECK(*composition.layers[0].clips.at(1).text == title());
    azl::Edit noText = SetClipText{.layer = 1, .column = 2, .text = "x"};
    CHECK_THROWS_AS(azl::applyEdit(composition, noText), azl::ProtocolError);
}

TEST_CASE("a composition file keeps text clips without looking for media")
{
    TempDir dir;
    azl::MediaLibrary library(dir.path / "library");
    azl::Composition composition = azl::defaultComposition();
    composition.layers[0].clips[1] = textClip(title());
    const auto warnings = azl::saveComposition(dir.path / "show.json", composition, library, {});
    CHECK(warnings.empty());
    const azl::LoadedComposition loaded = azl::loadComposition(dir.path / "show.json", library, {}, {});
    CHECK(loaded.warnings.empty());
    REQUIRE(loaded.composition.layers[0].clips.contains(1));
    CHECK(loaded.composition.layers[0].clips.at(1) == textClip(title()));
}

TEST_CASE("text clips render on any layer with font, size, colour, alignment and outline")
{
    azl::EngineHarness harness(kSize, 60.0, {}, twoLayers());
    Client client(harness.document());
    setSlot(client, 1, 1, textClip(title()));
    setSlot(client, 2, 1,
            textClip({.text = "right aligned", .font = "Helvetica-Bold", .size = 0.06, .color = {0.3, 0.8, 1.0, 1.0},
                      .align = TextAlign::Right, .x = 0.95, .y = 0.9}));
    setSlot(client, 2, 2, textClip({.text = "left", .size = 0.06, .align = TextAlign::Left, .x = 0.05, .y = 0.1}));
    trigger(client, 1, 1);
    trigger(client, 2, 1);
    harness.step(2);
    checkGolden(harness.readScreen(), "text");
}

TEST_CASE("italic text does not smear its last glyph to the edge of the composition")
{
    azl::EngineHarness harness(kSize, 60.0, {}, {.layers = {{.id = 1, .name = "Layer 1"}}});
    Client client(harness.document());
    setSlot(client, 1, 1, textClip({.text = "VORTEX", .font = "Helvetica Neue Medium Italic", .size = 0.16}));
    trigger(client, 1, 1);
    harness.step(2);
    const azl::Image image = harness.readScreen();
    size_t edge = 0;
    for (uint32_t y = 0; y < image.height; ++y) {
        const size_t i = (size_t(y) * image.width + image.width - 1) * 4;
        edge += image.rgba[i] + image.rgba[i + 1] + image.rgba[i + 2];
    }
    CHECK(edge == 0);
    checkGolden(image, "text_italic");
}

TEST_CASE("text and its parameters update live and undo through the protocol")
{
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    setSlot(client, 1, 1, textClip({.text = "one"}));
    trigger(client, 1, 1);
    harness.step();
    const azl::Image before = harness.readScreen();
    REQUIRE(lit(before) > 500);

    apply(client, SetClipText{.layer = 1, .column = 1, .text = "one two three"});
    harness.step();
    const azl::Image longer = harness.readScreen();
    CHECK(lit(longer) > lit(before) * 2);

    apply(client, SetClipText{.layer = 1, .column = 1, .color = std::array{1.0, 0.0, 0.0, 1.0}});
    harness.step();
    const azl::Image red = harness.readScreen();
    CHECK(lit(red) == doctest::Approx(lit(longer)).epsilon(0.05));
    const uint8_t* p = pixel(red, 0, 0);
    CHECK(p[1] == 0);

    client.send(msg::undo());
    client.next();
    client.send(msg::undo());
    client.next();
    harness.step();
    CHECK(same(harness.readScreen(), before));
}

TEST_CASE("a text clip plays through transitions, effects and Autopilot like a video clip")
{
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    TempDir media;
    const Message added = addMedia(client, copyMedia("count_240.mp4", media.path / "a.mp4"));
    setSlot(client, 1, 1, ClipRef{added["entry"]["hash"], "clip", {}});
    setSlot(client, 1, 2, textClip({.text = "TEXT", .size = 0.4, .color = {1.0, 0.0, 0.0, 1.0}}));
    trigger(client, 1, 1);
    harness.step();
    checkPatches(harness.readScreen());

    apply(client, azl::SetQuantise{0});
    apply(client, azl::SetLayerTransition{1, azl::TransitionMode::Alpha, 2.0});
    trigger(client, 1, 2);
    harness.step(30);
    CHECK(red(harness.readScreen(), 128) > 5000);
    harness.step(40);
    const azl::Image after = harness.readScreen();
    CHECK(red(after, 255) > 20000);
    CHECK(pixel(after, 240, 405)[0] < 10);

    apply(client, SetClipText{.layer = 1, .column = 2, .color = std::array{1.0, 1.0, 1.0, 1.0}});
    harness.step();
    const size_t white = lit(harness.readScreen());
    apply(client, azl::AddEffect{1, std::nullopt, {0, "transform", {{"scale", 1.2}}}});
    harness.step();
    CHECK(lit(harness.readScreen()) > white);
    client.send(msg::undo());
    client.next();
    apply(client, SetClipText{.layer = 1, .column = 2, .color = std::array{1.0, 0.0, 0.0, 1.0}});

    apply(client, azl::SetLayerTransition{1, azl::TransitionMode::Alpha, 0.0});
    apply(client, azl::SetLayerAutopilot{1, azl::AutopilotMode::Next, 1});
    bool sawText = false;
    bool sawVideo = false;
    for (int i = 0; i < 16; ++i) {
        harness.step(30);
        const azl::Image screen = harness.readScreen();
        const uint8_t* q = pixel(screen, 240, 405);
        sawVideo = sawVideo || (q[0] > 150 && q[1] < 100);
        sawText = sawText || red(harness.readScreen(), 255) > 20000;
    }
    CHECK(sawText);
    CHECK(sawVideo);
    CHECK(harness.stats().videoMisses == 0);
}

TEST_CASE("text size and colour are modulated without re-rasterising or recompiling")
{
    azl::EngineHarness harness({.width = 320, .height = 180});
    Client client(harness.document());
    setSlot(client, 1, 1, textClip({.text = "pulse", .size = 0.2}));
    trigger(client, 1, 1);
    harness.step();
    const size_t base = lit(harness.readScreen());
    const uint64_t pipelines = harness.compileStats().pipelines;

    azl::Modulator size;
    size.shape = azl::LfoShape::Square;
    size.beats = 2.0;
    size.target = {0, {1, 0, "textSize"}};
    size.min = 0.0;
    size.max = 0.2;
    apply(client, azl::AddModulator{std::nullopt, size});
    azl::Modulator red;
    red.shape = azl::LfoShape::Square;
    red.beats = 2.0;
    red.combine = azl::Combine::Multiply;
    red.target = {0, {1, 0, "textGreen"}};
    red.min = 0.0;
    red.max = 0.0;
    apply(client, azl::AddModulator{std::nullopt, red});
    harness.step();
    const azl::Image pulsed = harness.readScreen();
    CHECK(lit(pulsed) > base * 3);
    for (size_t i = 0; i < pulsed.rgba.size(); i += 4)
        REQUIRE(pulsed.rgba[i + 1] == 0);
    CHECK(harness.compileStats().pipelines == pipelines);

    azl::Edit wrong = azl::AddModulator{std::nullopt, {.target = {0, {1, 0, "textFont"}}}};
    CHECK(command(client, wrong)["type"] == "error");
}

TEST_CASE("a text clip runs its own clip chain fused with its layer's chain")
{
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    setSlot(client, 1, 1, textClip({.text = "TEXT", .size = 0.3}));
    trigger(client, 1, 1);
    harness.step();
    const size_t base = lit(harness.readScreen());
    const uint64_t passes = harness.stats().passes;
    REQUIRE(base > 5000);

    apply(client, azl::AddEffect{1, std::nullopt, {0, "colorize", {{"hue", 0.0}, {"saturation", 1.0}}}, 1});
    apply(client, azl::AddEffect{1, std::nullopt, {0, "transform", {{"scale", 0.5}}}});
    harness.step();
    const azl::Image shown = harness.readScreen();
    CHECK(lit(shown) == doctest::Approx(base / 4.0).epsilon(0.15));
    CHECK(red(shown, 255) > base / 6);
    for (size_t i = 0; i < shown.rgba.size(); i += 4)
        REQUIRE(shown.rgba[i + 1] < 40);
    CHECK(harness.stats().passes == passes);
}

TEST_CASE("a text layer adding no colour where its picture is transparent draws only over its text, looking the same")
{
    // Passes its input through; it says nothing about transparent black, so a chain holding it draws the whole frame.
    if (!azl::findEffect("testPassThrough"))
        azl::registerEffect({"testPassThrough", azl::StageKind::Pointwise, {}, false, "\n    return c;\n", {}});
    struct Case {
        azl::BlendMode blend;
        double red;
    };
    for (const Case& c : {Case{azl::BlendMode::Add, 0.0}, Case{azl::BlendMode::Screen, 0.0},
                          Case{azl::BlendMode::Add, 1.0}, Case{azl::BlendMode::Normal, 0.0}}) {
        CAPTURE(int(c.blend));
        CAPTURE(c.red);
        auto render = [&](bool whole) {
            azl::Composition composition;
            composition.layers.push_back({.id = 1, .name = "card"});
            std::vector<azl::Effect> chain{
                {1, "classicStripper",
                 {{"number", 0.8}, {"mode", 0.2}, {"movementType", 0.4}, {"globalWidth", 0.07}, {"red", c.red}}},
                {2, "classicAutoMask", {{"contrast", 0.8}}}};
            if (whole)
                chain.push_back({3, "testPassThrough", {}});
            composition.layers.push_back({.id = 2, .name = "text", .effects = chain, .blend = c.blend});
            azl::EngineHarness harness(kSize, 60.0, {}, composition);
            Client client(harness.document());
            setSlot(client, 2, 1, textClip({.text = "TEXT", .size = 0.2, .x = 0.3, .y = 0.6}));
            trigger(client, 2, 1);
            harness.step(20);
            return harness.readScreen();
        };
        const azl::Image clipped = render(false);
        CHECK(lit(clipped) > 5000);
        CHECK(clipped.rgba == render(true).rgba);
    }
}

TEST_CASE("a layer switches between two text clips directly, through a transition and by Autopilot")
{
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    apply(client, azl::SetQuantise{0});
    setSlot(client, 1, 1, textClip({.text = "RED", .size = 0.3, .color = {1.0, 0.0, 0.0, 1.0}}));
    setSlot(client, 1, 2, textClip({.text = "GREEN", .size = 0.3, .color = {0.0, 1.0, 0.0, 1.0}}));
    auto green = [](const azl::Image& image) {
        size_t count = 0;
        for (size_t i = 0; i < image.rgba.size(); i += 4)
            count += image.rgba[i] < 15 && image.rgba[i + 1] > 235 && image.rgba[i + 2] < 15;
        return count;
    };
    trigger(client, 1, 1);
    harness.step(2);
    const azl::Image first = harness.readScreen();
    CHECK(red(first, 255) > 5000);
    CHECK(green(first) == 0);

    trigger(client, 1, 2);
    harness.step(2);
    const azl::Image second = harness.readScreen();
    CHECK(red(second, 255) == 0);
    CHECK(green(second) > 5000);

    apply(client, azl::SetLayerTransition{1, azl::TransitionMode::Alpha, 2.0});
    trigger(client, 1, 1);
    harness.step(30);
    const azl::Image mixed = harness.readScreen();
    CHECK(red(mixed, 128) > 1000);
    CHECK(lit(mixed) <= lit(first) + lit(second));
    harness.step(40);
    CHECK(same(harness.readScreen(), first));

    apply(client, azl::SetLayerTransition{1, azl::TransitionMode::Alpha, 0.0});
    apply(client, azl::SetLayerAutopilot{1, azl::AutopilotMode::Next, 1});
    bool sawFirst = false;
    bool sawSecond = false;
    for (int i = 0; i < 16; ++i) {
        harness.step(30);
        const azl::Image image = harness.readScreen();
        sawFirst = sawFirst || same(image, first);
        sawSecond = sawSecond || same(image, second);
    }
    CHECK(sawFirst);
    CHECK(sawSecond);
}

TEST_CASE("a text cell of another deck takes over from a playing text cell, with Autopilot on one cell per deck")
{
    azl::Composition composition{.layers = {{.id = 5, .name = "Text"}},
                                 .decks = {{1, "Warm-up", {{13, "13"}}}, {2, "Peak", {{26, "26"}}}}};
    composition.deck = 1;
    azl::EngineHarness harness(kSize, 60.0, {}, composition);
    Client client(harness.document());
    setSlot(client, 5, 13,
            textClip({.text = "VORTEX", .font = "Helvetica Neue Medium Italic", .size = 0.2,
                      .color = {1.0, 0.0, 0.0, 1.0}}));
    setSlot(client, 5, 26, textClip({.text = "EVENT TITLE", .size = 0.1, .color = {0.0, 1.0, 0.0, 1.0}}));
    apply(client, azl::SetLayerAutopilot{5, azl::AutopilotMode::Random, 2});
    auto green = [](const azl::Image& image) {
        size_t count = 0;
        for (size_t i = 0; i < image.rgba.size(); i += 4)
            count += image.rgba[i] < 15 && image.rgba[i + 1] > 235 && image.rgba[i + 2] < 15;
        return count;
    };
    auto shows = [&](bool warmup) {
        const azl::Image image = harness.readScreen();
        const std::vector<azl::TransportState::Clip> clips = harness.transport().clips;
        const bool listed = clips.size() == 1 && clips[0].layer == 5 && clips[0].column == (warmup ? 13 : 26) &&
                            clips[0].frame == 0 && clips[0].beats == 0;
        const bool picture = warmup ? red(image, 255) > 2000 && green(image) == 0
                                     : green(image) > 1000 && red(image, 255) == 0;
        return listed && picture;
    };
    trigger(client, 5, 26);
    harness.step(150);
    CHECK(shows(false));
    trigger(client, 5, 13);
    harness.step(150);
    CHECK(shows(true));
    harness.step(300);
    CHECK(shows(true));

    client.send({{"type", "trigger"}, {"op", "deck"}, {"deck", 2}});
    REQUIRE(client.next()["op"] == "selectDeck");
    trigger(client, 5, 26);
    harness.step(150);
    CHECK(shows(false));
    harness.step(300);
    CHECK(shows(false));
    trigger(client, 5, 13);
    harness.step(150);
    CHECK(shows(true));
}

TEST_CASE("a moving text glides by fractions of a pixel with a steady brightness at every render scale")
{
    // A card too faint to show sits below the text, so the render scale has a layer to scale.
    azl::Composition c{.layers = {{.id = 1, .name = "card", .opacity = 0.001}, {.id = 2, .name = "text"}}};
    azl::EngineHarness harness(kSize, 60.0, {}, c);
    Client client(harness.document());
    setSlot(client, 2, 1, textClip({.text = "glide", .size = 0.12}));
    trigger(client, 2, 1);
    azl::Modulator move;
    move.shape = azl::LfoShape::Ramp;
    move.beats = 64.0;
    move.target = {0, {2, 0, "textX"}};
    move.min = 0.0;
    move.max = 0.2;
    apply(client, azl::AddModulator{std::nullopt, move});
    harness.step(2);

    for (int percent : {100, 83, 67}) {
        CAPTURE(percent);
        harness.engine().setRenderScale(percent);
        harness.step();
        std::vector<double> centres, totals;
        for (int frame = 0; frame < 24; ++frame) {
            harness.step();
            const azl::Image image = harness.readScreen();
            double total = 0.0, moment = 0.0;
            for (size_t i = 0; i < image.rgba.size(); i += 4) {
                const double v = image.rgba[i];
                total += v;
                moment += v * double((i / 4) % image.width);
            }
            REQUIRE(total > 0.0);
            centres.push_back(moment / total);
            totals.push_back(total);
        }
        const double step = (centres.back() - centres.front()) / double(centres.size() - 1);
        REQUIRE(std::abs(step) > 0.05);
        REQUIRE(std::abs(step) < 0.9);
        for (size_t i = 1; i < centres.size(); ++i) {
            CAPTURE(i);
            CHECK((centres[i] - centres[i - 1]) / step == doctest::Approx(1.0).epsilon(0.5));
            CHECK(totals[i] == doctest::Approx(totals[0]).epsilon(0.01));
        }
    }
}
