#include "clips.hpp"
#include "document/composition_file.hpp"
#include "effects/effect_registry.hpp"
#include "golden.hpp"
#include "media/composition_media.hpp"

#include <doctest.h>

#include <set>
#include <string>
#include <vector>

namespace {

using azl::AddEffect;
using azl::ClipRef;
using azl::Effect;

constexpr azl::EngineConfig kSize{.width = 960, .height = 540};
constexpr double kStillFps = 1e6;
// The test card's yellow colour bar (0.75, 0.75, 0).
constexpr uint32_t kYellowX = 255;
constexpr uint32_t kYellowY = 90;

void registerHalf()
{
    if (!azl::findEffect("testHalf"))
        azl::registerEffect({"testHalf", azl::StageKind::Pointwise, {}, false, "\n    return float4(c.rgb * 0.5, c.a);\n", {}});
}

Effect colorize(double hue, double saturation = 1.0)
{
    return {0, "colorize", {{"hue", hue}, {"saturation", saturation}}};
}

std::string add(Client& client, const fs::path& path)
{
    const Message added = addMedia(client, path);
    REQUIRE(added["type"] == "library");
    return added["entry"]["hash"];
}

void setSlot(Client& client, uint64_t column, const std::string& hash, std::vector<Effect> effects = {})
{
    apply(client, azl::SetSlot{1, column, ClipRef{.hash = hash, .name = "clip", .effects = std::move(effects)}});
}

void triggerColumn(Client& client, uint64_t column)
{
    client.send({{"type", "trigger"}, {"op", "column"}, {"column", column}});
    REQUIRE(client.next()["op"] == "triggerColumn");
}

Message snapshot(Client& client)
{
    client.send(msg::hello());
    return client.next()["composition"];
}

void checkNear(const azl::Image& image, uint32_t x, uint32_t y, int r, int g, int b, int tolerance = 4)
{
    const uint8_t* p = pixel(image, x, y);
    INFO("pixel " << x << "," << y << " is " << int(p[0]) << "," << int(p[1]) << "," << int(p[2]));
    CHECK(std::abs(p[0] - r) <= tolerance);
    CHECK(std::abs(p[1] - g) <= tolerance);
    CHECK(std::abs(p[2] - b) <= tolerance);
}

} // namespace

TEST_CASE("clip and composition chains are undoable edits; effect edits find any effect by its id")
{
    azl::Composition start = azl::defaultComposition();
    start.layers[0].clips[1] = ClipRef{.hash = kUnknownHash, .name = "clip"};
    azl::EngineHarness harness(kSize, kStillFps, {}, start);
    Client client(harness.document());

    const Message added = apply(client, AddEffect{1, std::nullopt, colorize(0.5), 1});
    const uint64_t onClip = added["effect"]["id"];
    CHECK(added["column"] == 1);
    const uint64_t onComposition = apply(client, AddEffect{0, std::nullopt, colorize(0.25)})["effect"]["id"];
    const uint64_t second = apply(client, AddEffect{1, std::nullopt, {0, "transform", {}}, 1})["effect"]["id"];

    Message composition = snapshot(client);
    CHECK(composition["effects"][0]["id"] == onComposition);
    CHECK(composition["layers"][0]["effects"].empty());
    const Message clipEffects = composition["layers"][0]["clips"]["1"]["effects"];
    REQUIRE(clipEffects.size() == 2);
    CHECK(clipEffects[0]["params"]["saturation"] == 1.0);

    apply(client, azl::SetEffectParam{onClip, "hue", 0.75});
    apply(client, azl::MoveEffect{second, 0});
    apply(client, azl::SetEffectParam{onComposition, "saturation", 0.5});
    composition = snapshot(client);
    CHECK(composition["layers"][0]["clips"]["1"]["effects"][0]["id"] == second);
    CHECK(composition["layers"][0]["clips"]["1"]["effects"][1]["params"]["hue"] == 0.75);
    CHECK(composition["effects"][0]["params"]["saturation"] == 0.5);

    apply(client, azl::RemoveEffect{onClip});
    CHECK(snapshot(client)["layers"][0]["clips"]["1"]["effects"].size() == 1);
    client.send(msg::undo());
    REQUIRE(client.next()["cause"] == "undo");
    composition = snapshot(client);
    CHECK(composition["layers"][0]["clips"]["1"]["effects"][1]["id"] == onClip);
    CHECK(composition["layers"][0]["effects"].empty());

    CHECK(command(client, AddEffect{1, std::nullopt, colorize(0.5), 9})["type"] == "error");
    CHECK(command(client, AddEffect{0, std::nullopt, colorize(0.5), 1})["type"] == "error");
    CHECK(command(client, azl::SetEffectParam{999, "hue", 0.1})["type"] == "error");

    // A clip entering a cell has its effects completed, with fresh ids.
    apply(client, azl::SetSlot{1, 2, ClipRef{.hash = kUnknownHash, .name = "b", .effects = {colorize(0.1)}}});
    const Message entered = snapshot(client)["layers"][0]["clips"]["2"]["effects"][0];
    CHECK(entered["id"].get<uint64_t>() > second);
    CHECK(entered["params"].contains("mix"));
    CHECK(command(client, azl::SetSlot{1, 3, ClipRef{.hash = kUnknownHash, .name = "c", .effects = {{onClip, "colorize", {}}}}})
              ["type"] == "error");
}

TEST_CASE("clip and composition chains are saved in composition files; unknown effects there are skipped")
{
    azl::Composition c = azl::defaultComposition();
    c.layers[0].clips[1] = ClipRef{.hash = kUnknownHash, .name = "clip", .effects = {{2, "colorize", {{"hue", 0.3}}}}};
    c.effects = {{3, "transform", {{"scale", 0.5}}}};
    const azl::CompositionFile loaded = azl::compositionFileFromJson(azl::toJson(azl::CompositionFile{c}));
    CHECK(loaded.composition.layers[0].clips.at(1).effects == c.layers[0].clips.at(1).effects);
    CHECK(loaded.composition.effects == c.effects);

    azl::Composition unknown = c;
    unknown.layers[0].clips[1].effects.push_back({4, "wobble", {}});
    unknown.effects.push_back({5, "wobble", {}});
    std::vector<std::string> warnings;
    azl::dropUnknownEffects(unknown, warnings);
    CHECK(unknown.layers[0].clips.at(1).effects.size() == 1);
    CHECK(unknown.effects.size() == 1);
    REQUIRE(warnings.size() == 2);
    CHECK(warnings[0] == "composition: unknown effect wobble; skipped");
    CHECK(warnings[1] == "Layer 1 / clip clip: unknown effect wobble; skipped");
}

TEST_CASE("an older file keeps its values' meaning: angles wrap into the range, other values clamp with a warning")
{
    azl::Composition c = azl::defaultComposition();
    c.layers[0].effects = {{1, "stripper", {{"direction", 270.0}, {"width", 1.5}}},
                           {2, "transform", {{"rotation", -360.0}}}};
    c.modulators.push_back({.id = 1, .target = {0, {0, 1, "direction"}}, .min = -720.0, .max = 720.0});
    std::vector<std::string> warnings;
    azl::dropUnknownEffects(c, warnings);
    const std::vector<Effect>& effects = c.layers[0].effects;
    CHECK(effects[0].params.at("direction") == doctest::Approx(-90.0));
    CHECK(effects[0].params.at("width") == 1.0);
    CHECK(effects[1].params.at("rotation") == doctest::Approx(0.0));
    CHECK(c.modulators[0].min == -360.0);
    CHECK(c.modulators[0].max == 360.0);
    REQUIRE(warnings.size() == 2);
    CHECK(warnings[0].find("stripper.width 1.5") != std::string::npos);
    CHECK(warnings[1].find("modulator") != std::string::npos);
}

TEST_CASE("an older file with repeated effect ids loads with unique ones; its targets follow the effect they named")
{
    azl::Composition c = azl::defaultComposition();
    c.effects = {{1, "colorize", {}}};
    c.layers[0].effects = {{1, "transform", {}}};
    c.layers[0].clips[1] = ClipRef{.hash = kUnknownHash, .name = "clip", .effects = {{1, "colorize", {}}}};
    c.layers.push_back({.id = 2, .name = "Layer 2", .effects = {{1, "edgeDetection", {}}, {3, "colorize", {}}}});
    c.macros = {{.id = 1, .targets = {{}, {}, {}, {}}}};
    c.modulators = {{.id = 1}};
    Message file = azl::toJson(azl::CompositionFile{c});
    Message& targets = file["composition"]["macros"][0]["targets"];
    targets[0].update({{"layer", 0}, {"effect", 1}, {"param", "hue"}});
    targets[1].update({{"layer", 1}, {"effect", 1}, {"param", "scale"}});
    targets[2].update({{"layer", 2}, {"effect", 1}, {"param", "multiplier"}});
    targets[3].update({{"layer", 2}, {"effect", 3}, {"param", "hue"}});
    file["composition"]["modulators"][0]["target"] = {{"layer", 2}, {"effect", 1}, {"param", "mix"}};

    const azl::Composition loaded = azl::compositionFileFromJson(file).composition;
    const azl::Layer& base = loaded.layers[0];
    const azl::Layer& top = loaded.layers[1];
    std::set<uint64_t> ids;
    azl::forEachChain(loaded, [&](const std::vector<Effect>& effects, uint64_t, std::optional<uint64_t>) {
        for (const Effect& e : effects)
            CHECK(ids.insert(e.id).second);
    });
    CHECK(ids.size() == 5);
    CHECK(loaded.effects[0].id == 1);
    CHECK(top.effects[1].id == 3);
    const std::vector<azl::MacroTarget>& driven = loaded.macros[0].targets;
    CHECK(driven[0].param == azl::ParamRef{0, 1, "hue"});
    CHECK(driven[1].param == azl::ParamRef{0, base.effects[0].id, "scale"});
    CHECK(driven[2].param == azl::ParamRef{0, top.effects[0].id, "multiplier"});
    CHECK(driven[3].param == azl::ParamRef{0, 3, "hue"});
    CHECK(loaded.modulators[0].target.param == azl::ParamRef{0, top.effects[0].id, "mix"});
    CHECK(base.clips.at(1).effects[0].id != base.effects[0].id);

    const Message saved = azl::toJson(azl::CompositionFile{loaded})["composition"];
    CHECK(saved["macros"][0]["targets"][1] ==
          Message{{"effect", base.effects[0].id}, {"param", "scale"}, {"curve", {0.0, 0.0, 1.0, 1.0}}, {"min", 0.0},
                  {"max", 1.0}});
    CHECK(saved["modulators"][0]["target"] == Message{{"effect", top.effects[0].id}, {"param", "mix"}});

    // Edits keep the ids unique; an older message naming a layer next to the effect still finds the effect.
    azl::Composition document = loaded;
    azl::Edit taken = AddEffect{2, std::nullopt, {1, "colorize", {}}};
    CHECK_THROWS_AS(azl::applyEdit(document, taken), azl::ProtocolError);
    azl::Edit repeated = azl::LoadComposition{c};
    CHECK_THROWS_AS(azl::applyEdit(document, repeated), azl::ProtocolError);
    azl::Edit old = azl::editFromJson({{"op", "setEffectParam"}, {"layer", 9}, {"effect", 3}, {"param", "hue"},
                                       {"value", 0.5}});
    azl::applyEdit(document, old);
    CHECK(document.layers[1].effects[1].params.at("hue") == 0.5);
    azl::Edit target = azl::editFromJson(
        {{"op", "addMacroTarget"}, {"macro", 1}, {"target", {{"layer", 9}, {"effect", 3}, {"param", "saturation"}}}});
    azl::applyEdit(document, target);
    CHECK(document.macros[0].targets.back().param == azl::ParamRef{0, 3, "saturation"});
}

TEST_CASE("a clip chain runs before its layer's chain, fused into one pass with it")
{
    TempDir media;
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    const std::string hash = add(client, copyMedia("count_709_tv.mp4", media.path / "count.mp4"));
    apply(client, AddEffect{1, std::nullopt, {0, "colorize", {{"hue", 0.55}, {"saturation", 0.8}}}});
    apply(client, azl::SetLayerClip{1, ClipRef{.hash = hash,
                                               .name = "count",
                                               .effects = {{0, "transform", {{"positionX", 0.1}, {"scale", 0.8},
                                                                             {"rotation", 12.0}}}}}});
    harness.step(85);
    checkGolden(harness.readScreen(), "clip_chain");
    CHECK(harness.stats().passes == 1);
    CHECK(harness.stats().intermediates == 0);
}

TEST_CASE("a group's chain runs on the group's picture")
{
    azl::Layer group{.id = 10, .name = "Group", .effects = {{20, "transform", {{"scale", 0.6}, {"rotation", -20.0}}},
                                                            {21, "colorize", {{"hue", 0.8}, {"saturation", 0.7}}}},
                     .group = true,
                     .layers = {{.id = 11, .name = "Card"},
                                {.id = 12, .name = "Small", .opacity = 0.7,
                                 .effects = {{22, "transform", {{"positionX", 0.25}, {"scale", 0.4}}}}}}};
    azl::EngineHarness harness(kSize, kStillFps, {}, azl::Composition{{group}});
    harness.step();
    checkGolden(harness.readScreen(), "group_chain");
    // Two layers into the group's picture, then the group's fused chain.
    CHECK(harness.stats().passes == 3);
}

TEST_CASE("the composition chain runs on the final picture, in one more pass")
{
    azl::Composition c{{{.id = 1, .name = "Card", .effects = {{1, "colorize", {{"hue", 0.6}, {"saturation", 0.5}}}}},
                        {.id = 2, .name = "Shifted", .effects = {{2, "transform", {{"positionY", 0.2}}}},
                         .blend = azl::BlendMode::Add}}};
    c.effects = {{3, "transform", {{"scale", 0.75}, {"rotation", 8.0}}}, {4, "edgeDetection", {{"multiplier", 0.6}}}};
    azl::EngineHarness harness(kSize, kStillFps, {}, c);
    harness.step();
    checkGolden(harness.readScreen(), "composition_chain");
    // Two layers, then the composition's transform and its edge detection over the materialised transform.
    CHECK(harness.stats().passes == 4);

    Client client(harness.document());
    apply(client, azl::RemoveEffect{4});
    apply(client, azl::RemoveEffect{3});
    harness.step();
    CHECK(harness.stats().passes == 2);
}

TEST_CASE("modulators drive clip and composition effect parameters")
{
    azl::Composition c = azl::defaultComposition();
    c.effects = {{1, "colorize", {{"hue", 0.0}}}};
    azl::EngineHarness harness(kSize, kStillFps, {}, c);
    Client client(harness.document());
    harness.step();
    const uint8_t* tinted = pixel(harness.readScreen(), kYellowX, kYellowY);
    CHECK(tinted[1] < 10);

    azl::Modulator off;
    off.target = {0, {0, 1, "mix"}};
    off.combine = azl::Combine::Multiply;
    off.min = 0.0;
    off.max = 0.0;
    apply(client, azl::AddModulator{std::nullopt, off});
    harness.step();
    checkNear(harness.readScreen(), kYellowX, kYellowY, 191, 191, 0);
}

TEST_CASE("a trigger to a clip with its own chain compiles nothing and misses nothing")
{
    TempDir media;
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    setSlot(client, 1, add(client, copyMedia("count_240.mp4", media.path / "a.mp4")));
    setSlot(client, 2, add(client, copyMedia("count_709_tv.mp4", media.path / "b.mp4")), {colorize(0.0)});
    triggerColumn(client, 1);
    harness.step(300);
    checkPatches(harness.readScreen());

    harness.holdCompiles(true);
    const uint64_t pipelines = harness.compileStats().pipelines;
    triggerColumn(client, 2);
    harness.step(70);
    const azl::Image shown = harness.readScreen();
    // The green patch through a red Colorize: its luminance, in red only.
    checkNear(shown, 720, 405, 129, 0, 0, 6);
    CHECK(harness.compileStats().pipelines == pipelines);
    CHECK(harness.stats().videoMisses == 0);
    harness.holdCompiles(false);
}

TEST_CASE("through a transition each clip keeps its own chain, then both run the layer's")
{
    registerHalf();
    TempDir media;
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    apply(client, AddEffect{1, std::nullopt, {0, "testHalf", {}}});
    setSlot(client, 1, add(client, copyMedia("count_240.mp4", media.path / "a.mp4")), {colorize(0.0)});
    setSlot(client, 2, add(client, copyMedia("count_709_tv.mp4", media.path / "b.mp4")));
    apply(client, azl::SetLayerTransition{1, azl::TransitionMode::Wipe, 2.0});
    triggerColumn(client, 1);
    harness.step(300);
    const uint64_t pipelines = harness.compileStats().pipelines;
    triggerColumn(client, 2);
    // Halfway: the incoming clip left of the wipe, the outgoing one right of it.
    harness.step(91);
    const azl::Image halfway = harness.readScreen();
    checkNear(halfway, 240, 405, 100, 20, 20);
    checkNear(halfway, 720, 405, 64, 0, 0);
    CHECK(harness.compileStats().pipelines == pipelines);
    CHECK(harness.stats().videoMisses == 0);
}

TEST_CASE("clip changes reuse side-pass textures: a layer's targets stay bounded however many clips it plays")
{
    TempDir media;
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    const std::string a = add(client, copyMedia("count_240.mp4", media.path / "a.mp4"));
    const std::string b = add(client, copyMedia("count_709_tv.mp4", media.path / "b.mp4"));
    constexpr uint64_t kColumns = 4;
    for (uint64_t column = 1; column <= kColumns; ++column)
        setSlot(client, column, column % 2 ? a : b, {{0, "trails", {}}, {0, "classicTrails", {}}});
    apply(client, azl::SetLayerTransition{1, azl::TransitionMode::Alpha, 1.0});
    auto held = [&] { return harness.stats().sideTargets + harness.stats().sideSpares; };

    triggerColumn(client, 1);
    harness.step(150);
    triggerColumn(client, 2);
    harness.step(150);
    const uint64_t bound = held();
    CHECK(bound > 0);
    for (uint64_t column = 3; column <= kColumns; ++column) {
        triggerColumn(client, column);
        for (int frame = 0; frame < 150; frame += 10) {
            harness.step(10);
            REQUIRE(held() <= bound);
        }
    }
    CHECK(harness.stats().videoMisses == 0);
}
