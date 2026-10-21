#include "clips.hpp"
#include "golden.hpp"
#include "media/composition_media.hpp"

#include <doctest.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <string>

namespace {

using azl::ClipRef;
using azl::Effect;

constexpr azl::EngineConfig kSize{.width = 960, .height = 540};
constexpr azl::EngineConfig kGolden{.width = 480, .height = 270};

ClipRef generatorClip(const std::string& type, std::map<std::string, double> params = {}, uint64_t id = 0)
{
    return {.name = type, .effects = {Effect{id, type, std::move(params)}}};
}

Message setSlot(Client& client, uint64_t layer, uint64_t column, ClipRef clip)
{
    return apply(client, azl::SetSlot{layer, column, std::move(clip)});
}

uint64_t generatorId(const Message& reply)
{
    return reply["clip"]["effects"][0]["id"];
}

void trigger(Client& client, uint64_t layer, uint64_t column)
{
    client.send({{"type", "trigger"}, {"op", "clip"}, {"layer", layer}, {"column", column}});
    REQUIRE(client.next()["op"] == "triggerClip");
}

double meanRed(const azl::Image& image)
{
    double sum = 0.0;
    for (size_t i = 0; i < image.rgba.size(); i += 4)
        sum += image.rgba[i];
    return sum / double(image.rgba.size() / 4);
}

size_t lit(const azl::Image& image)
{
    size_t count = 0;
    for (size_t i = 0; i < image.rgba.size(); i += 4)
        count += image.rgba[i] + image.rgba[i + 1] + image.rgba[i + 2] > 30;
    return count;
}

// Halfway through an alpha transition between the green patch (40, 160, 80) and solid red.
bool halfway(const azl::Image& image)
{
    const uint8_t* p = pixel(image, 720, 405);
    return std::abs(p[0] - 148) < 12 && std::abs(p[1] - 80) < 12 && std::abs(p[2] - 40) < 12;
}

bool expectError(const azl::Edit& edit, azl::Composition composition)
{
    azl::Edit e = edit;
    try {
        azl::applyEdit(composition, e);
    } catch (const azl::ProtocolError&) {
        return true;
    }
    return false;
}

} // namespace

TEST_CASE("generator clips validate, keep their generator first and round-trip; files without them are unchanged")
{
    CHECK(azl::generatorTypes() ==
          std::vector<std::string>{"blank", "solid", "strobe", "lines", "checker", "gradient", "noise", "isfGraphPaper",
                                   "isfSineWarpGradient", "isfSpiral", "isfStripes", "isfWorleyCells"});
    const std::vector<std::string> effects = azl::effectTypes();
    for (const std::string& type : azl::generatorTypes())
        CHECK(std::find(effects.begin(), effects.end(), type) == effects.end());

    azl::Composition composition = azl::defaultComposition();
    azl::Edit set = azl::SetSlot{1, 1, generatorClip("solid", {{"red", 0.5}})};
    azl::applyEdit(composition, set);
    const ClipRef& clip = composition.layers[0].clips.at(1);
    REQUIRE(azl::isGeneratorClip(clip));
    CHECK(clip.hash.empty());
    CHECK(clip.effects[0].id != 0);
    CHECK(clip.effects[0].params.at("red") == 0.5);
    CHECK(clip.effects[0].params.at("mix") == 1.0);
    const nlohmann::json j = clip;
    CHECK(j.get<ClipRef>() == clip);
    const uint64_t generator = clip.effects[0].id;

    CHECK(expectError(azl::SetSlot{1, 2, ClipRef{.name = "nothing"}}, composition));
    CHECK(expectError(azl::SetSlot{1, 2, ClipRef{.hash = kUnknownHash, .name = "x", .effects = {{0, "solid", {}}}}},
                      composition));
    CHECK(expectError(azl::SetSlot{1, 2, ClipRef{.name = "x", .effects = {{0, "colorize", {}}, {0, "solid", {}}}}},
                      composition));
    CHECK(expectError(azl::AddEffect{1, std::nullopt, {0, "solid", {}}}, composition));
    CHECK(expectError(azl::AddEffect{1, 0, {0, "colorize", {}}, 1}, composition));
    CHECK(expectError(azl::AddEffect{1, std::nullopt, {0, "noise", {}}, 1}, composition));
    CHECK(expectError(azl::RemoveEffect{generator}, composition));
    CHECK(expectError(azl::MoveEffect{generator, 0, 1}, composition));

    azl::Edit add = azl::AddEffect{1, std::nullopt, {0, "colorize", {}}, 1};
    azl::Edit undo = azl::applyEdit(composition, add);
    REQUIRE(composition.layers[0].clips.at(1).effects.size() == 2);
    const uint64_t colorize = composition.layers[0].clips.at(1).effects[1].id;
    CHECK(expectError(azl::MoveEffect{colorize, 0}, composition));
    azl::Edit param = azl::SetEffectParam{generator, "green", 0.25};
    azl::applyEdit(composition, param);
    CHECK(composition.layers[0].clips.at(1).effects[0].params.at("green") == 0.25);
    azl::applyEdit(composition, undo);
    CHECK(composition.layers[0].clips.at(1).effects.size() == 1);

    // A composition of media and text clips reads and writes exactly as before.
    azl::Composition old = azl::defaultComposition();
    old.layers[0].clips[1] = ClipRef{.hash = std::string(64, 'b'), .name = "clip", .effects = {{7, "colorize", {}}}};
    old.layers[0].clips[2] = ClipRef{.name = "Text", .text = azl::TextClip{.text = "hi"}};
    const nlohmann::json before = azl::toJson(azl::CompositionFile{old});
    CHECK(azl::toJson(azl::compositionFileFromJson(before)) == before);
    CHECK(before.dump().find("generator") == std::string::npos);

    TempDir dir;
    azl::MediaLibrary library(dir.path / "library");
    const auto warnings = azl::saveComposition(dir.path / "show.json", composition, library, {});
    CHECK(warnings.empty());
    const azl::LoadedComposition loaded = azl::loadComposition(dir.path / "show.json", library, {}, {});
    CHECK(loaded.warnings.empty());
    REQUIRE(loaded.composition.layers[0].clips.contains(1));
    CHECK(loaded.composition.layers[0].clips.at(1) == composition.layers[0].clips.at(1));
}

TEST_CASE("golden frames of every generator")
{
    const std::vector<std::pair<std::string, std::map<std::string, double>>> looks = {
        {"blank", {}},
        {"solid", {{"red", 0.9}, {"green", 0.3}, {"blue", 0.1}}},
        {"strobe", {{"red", 0.2}, {"green", 0.8}, {"blue", 1.0}}},
        {"lines", {{"count", 6.0}, {"width", 0.3}, {"angle", 30.0}, {"softness", 0.1}, {"blue", 0.2}}},
        {"checker", {{"count", 5.0}, {"angle", 15.0}, {"green", 0.6}}},
        {"gradient", {{"angle", 45.0}, {"red", 0.8}, {"toBlue", 1.0}, {"toRed", 0.1}, {"toGreen", 0.2}}},
        {"noise", {{"scale", 5.0}, {"speed", 0.0}, {"contrast", 1.5}, {"blue", 0.6}}},
    };
    for (const auto& [type, params] : looks) {
        CAPTURE(type);
        azl::EngineHarness harness(kGolden);
        Client client(harness.document());
        apply(client, azl::SetQuantise{0});
        setSlot(client, 1, 1, generatorClip(type, params));
        trigger(client, 1, 1);
        harness.step(2);
        const azl::Image image = harness.readScreen();
        if (type == "blank")
            CHECK(lit(image) == 0);
        else
            CHECK(lit(image) > image.width * image.height / 4);
        checkGolden(image, "generator_" + type);
    }
}

TEST_CASE("the Strobe is on for its duty of every period on the beat clock")
{
    azl::EngineHarness harness({.width = 160, .height = 90});
    Client client(harness.document());
    apply(client, azl::SetQuantise{0});
    setSlot(client, 1, 1, generatorClip("strobe", {{"rate", 0.5}, {"duty", 0.25}}));
    trigger(client, 1, 1);
    harness.step(2);
    int on = 0, off = 0;
    for (int i = 0; i < 240; ++i) {
        harness.step();
        const double phase = std::fmod(harness.transport().beat / 0.5, 1.0);
        if (std::abs(phase - 0.25) < 0.05 || phase > 0.95 || phase < 0.05)
            continue;
        const bool lit = pixel(harness.readScreen(), 80, 45)[0] > 128;
        CHECK_MESSAGE(lit == (phase < 0.25), "beat " << harness.transport().beat);
        (lit ? on : off)++;
    }
    CHECK(on > 20);
    CHECK(off > 60);
}

TEST_CASE("Autopilot lands on Blank as a step that shows nothing; an empty cell is skipped")
{
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    TempDir media;
    const Message added = addMedia(client, copyMedia("count_240.mp4", media.path / "a.mp4"));
    apply(client, azl::SetQuantise{0});
    setSlot(client, 1, 1, ClipRef{added["entry"]["hash"], "clip", {}});
    setSlot(client, 1, 3, generatorClip("blank"));
    trigger(client, 1, 1);
    harness.step();
    apply(client, azl::SetLayerAutopilot{1, azl::AutopilotMode::Next, 1});
    std::map<uint64_t, int> columns;
    int blankFrames = 0, videoFrames = 0;
    uint64_t last = 0;
    for (int i = 0; i < 16 * 30; ++i) {
        harness.step();
        const azl::TransportState transport = harness.transport();
        REQUIRE(transport.playing.size() == 1);
        REQUIRE(transport.playing[0].play);
        const uint64_t column = transport.playing[0].play->column;
        columns[column]++;
        if (i % 10 != 0 || std::exchange(last, column) != column)
            continue;
        const size_t shown = lit(harness.readScreen());
        if (column == 3) {
            CHECK(shown == 0);
            blankFrames++;
        } else {
            CHECK(shown > 1000);
            videoFrames++;
        }
    }
    CHECK(columns.size() == 2);
    CHECK(columns[3] > 150);
    CHECK(blankFrames > 10);
    CHECK(videoFrames > 10);
    CHECK(harness.stats().videoMisses == 0);
}

TEST_CASE("transitions run from a video clip to a generator and back")
{
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    TempDir media;
    const Message added = addMedia(client, copyMedia("count_240.mp4", media.path / "a.mp4"));
    apply(client, azl::SetQuantise{0});
    setSlot(client, 1, 1, ClipRef{added["entry"]["hash"], "clip", {}});
    setSlot(client, 1, 2, generatorClip("solid", {{"red", 1.0}, {"green", 0.0}, {"blue", 0.0}}));
    trigger(client, 1, 1);
    harness.step(2);
    checkPatches(harness.readScreen());
    apply(client, azl::SetLayerTransition{1, azl::TransitionMode::Alpha, 2.0});
    const uint64_t pipelines = harness.compileStats().pipelines;

    trigger(client, 1, 2);
    harness.step(30);
    CHECK(halfway(harness.readScreen()));
    harness.step(40);
    const azl::Image red = harness.readScreen();
    CHECK(meanRed(red) > 250.0);
    CHECK(pixel(red, 480, 270)[1] < 5);

    trigger(client, 1, 1);
    harness.step(30);
    CHECK(halfway(harness.readScreen()));
    harness.step(40);
    checkPatches(harness.readScreen());

    apply(client, azl::SetLayerTransition{1, azl::TransitionMode::Wipe, 2.0});
    trigger(client, 1, 2);
    harness.step(30);
    const azl::Image wipe = harness.readScreen();
    CHECK(pixel(wipe, 100, 270)[1] < 5);
    CHECK(pixel(wipe, 100, 270)[0] > 250);
    CHECK(harness.stats().videoMisses == 0);
    CHECK(harness.compileStats().pipelines == pipelines);
}

TEST_CASE("a layer switches between two generator clips directly, through a transition and by Autopilot")
{
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    apply(client, azl::SetQuantise{0});
    setSlot(client, 1, 1, generatorClip("solid", {{"red", 1.0}, {"green", 0.0}, {"blue", 0.0}}));
    setSlot(client, 1, 2, generatorClip("checker"));
    trigger(client, 1, 1);
    harness.step(2);
    const azl::Image solid = harness.readScreen();
    CHECK(meanRed(solid) > 250.0);

    trigger(client, 1, 2);
    harness.step(2);
    const azl::Image checker = harness.readScreen();
    CHECK(checker.rgba != solid.rgba);
    CHECK(lit(checker) > 1000);
    const std::vector<azl::TransportState::Clip> clips = harness.transport().clips;
    REQUIRE(clips.size() == 1);
    CHECK(clips[0].column == 2);
    CHECK(clips[0].beats == 0);

    apply(client, azl::SetLayerTransition{1, azl::TransitionMode::Wipe, 2.0});
    trigger(client, 1, 1);
    harness.step(30);
    const azl::Image wipe = harness.readScreen();
    CHECK(pixel(wipe, 100, 270)[0] > 250);
    CHECK(pixel(wipe, 100, 270)[1] < 5);
    CHECK(pixel(wipe, 900, 270)[1] == pixel(checker, 900, 270)[1]);
    harness.step(40);
    CHECK(harness.readScreen().rgba == solid.rgba);

    apply(client, azl::SetLayerTransition{1, azl::TransitionMode::Alpha, 0.0});
    apply(client, azl::SetLayerAutopilot{1, azl::AutopilotMode::Next, 1});
    bool sawSolid = false;
    bool sawChecker = false;
    for (int i = 0; i < 16; ++i) {
        harness.step(30);
        const azl::Image image = harness.readScreen();
        sawSolid = sawSolid || image.rgba == solid.rgba;
        sawChecker = sawChecker || image.rgba == checker.rgba;
    }
    CHECK(sawSolid);
    CHECK(sawChecker);
}

TEST_CASE("macros and modulators drive generator parameters without recompiling")
{
    azl::EngineHarness harness({.width = 160, .height = 90});
    Client client(harness.document());
    apply(client, azl::SetQuantise{0});
    const uint64_t solid = generatorId(setSlot(client, 1, 1, generatorClip("solid", {{"red", 0.0}, {"green", 0.0}})));
    trigger(client, 1, 1);
    harness.step(2);
    CHECK(pixel(harness.readScreen(), 80, 45)[0] < 3);
    const uint64_t pipelines = harness.compileStats().pipelines;

    const Message macro = apply(client, azl::AddMacro{std::nullopt, {.name = "Red", .value = 0.5}});
    const uint64_t id = macro["macro"]["id"];
    apply(client, azl::AddMacroTarget{id, std::nullopt, {{1, solid, "red"}, {}, 0.0, 1.0}});
    harness.step(2);
    CHECK(std::abs(pixel(harness.readScreen(), 80, 45)[0] - 128) < 6);

    apply(client, azl::SetMacroValue{id, 1.0});
    harness.step(4);
    CHECK(pixel(harness.readScreen(), 80, 45)[0] > 250);

    azl::Modulator square;
    square.shape = azl::LfoShape::Square;
    square.beats = 1.0;
    square.target = {0, {1, solid, "green"}};
    apply(client, azl::AddModulator{std::nullopt, square});
    int greenOn = 0, greenOff = 0;
    for (int i = 0; i < 60; ++i) {
        harness.step();
        (pixel(harness.readScreen(), 80, 45)[1] > 128 ? greenOn : greenOff)++;
    }
    CHECK(greenOn > 10);
    CHECK(greenOff > 10);
    CHECK(harness.compileStats().pipelines == pipelines);
}

TEST_CASE("GPU time per generator at 1080p")
{
    auto measure = [](const std::optional<ClipRef>& clip) {
        azl::EngineHarness harness({.width = 1920, .height = 1080});
        Client client(harness.document());
        apply(client, azl::SetQuantise{0});
        if (clip) {
            setSlot(client, 1, 1, *clip);
            trigger(client, 1, 1);
        }
        harness.step(30);
        const uint64_t before = harness.stats().gpuMicros;
        harness.step(120);
        return double(harness.stats().gpuMicros - before) / 120 / 1000.0;
    };
    std::string line = "GPU ms/frame at 1080p: test card " + std::to_string(measure(std::nullopt));
    for (const std::string& type : azl::generatorTypes()) {
        const auto& params = azl::findEffect(type)->params;
        const bool moves = std::any_of(params.begin(), params.end(), [](const auto& p) { return p.name == "speed"; });
        line += ", " + type + " " +
                std::to_string(measure(generatorClip(type, moves ? std::map<std::string, double>{{"speed", 0.5}}
                                                                 : std::map<std::string, double>{})));
    }
    MESSAGE(line);
    std::printf("%s\n", line.c_str());
}
