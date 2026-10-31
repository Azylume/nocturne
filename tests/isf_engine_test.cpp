#include "golden.hpp"
#include "test_client.hpp"

#include "effects/chain_planner.hpp"
#include "effects/isf.hpp"
#include "engine/chain_compiler.hpp"
#include "engine/isf_compiler.hpp"

#include <Metal/Metal.hpp>
#include <doctest.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <utility>
#include <map>
#include <string>

namespace {

using Params = std::map<std::string, double>;

constexpr azl::EngineConfig kGolden{.width = 480, .height = 270};

struct Look {
    std::string type;
    Params params;
};

const std::vector<Look> kEffects{
    {"isfKaleidoscope", {{"sides", 5.0}, {"angle", 0.1}, {"centerX", 0.5}, {"centerY", 0.5}}},
    {"isfTwirl", {{"radius", 0.4}, {"amount", 2.0}}},
    {"isfPixellate", {{"cell_size", 0.05}}},
    {"isfChromaticAberration", {{"amount", 0.08}}},
    {"isfRgbEq", {{"red", 1.5}, {"green", 0.6}}},
    {"isfPosterize", {{"numColors", 4.0}}},
    {"isfMirror", {{"horizontal", 1.0}, {"vertical", 1.0}}},
    {"isfQuadTile", {{"size", 0.8}, {"rotation", 0.1}, {"shiftX", 0.2}}},
    {"isfSolarize", {{"centerBrightness", 0.4}, {"colorize", 0.6}}},
    {"isfTriangleMosaic", {{"cellSize", 0.06}, {"outlineWidth", 1.5}, {"outlineColorRed", 1.0}}},
    {"isfRadialReplicate", {{"numberOfDivisions", 8.0}, {"preRotateAngle", 30.0}}},
    {"isfKaleidoscopeTile", {{"size", 0.4}, {"sides", 8.0}, {"rotation", 0.2}}},
    {"isfMultiHueShift", {{"shiftLow", 0.3}, {"shiftMid", 0.6}, {"shiftHigh", 0.15}}},
};

const std::vector<Look> kGenerators{
    {"isfSpiral", {{"count", 5.0}, {"color2Green", 0.3}, {"color2Blue", 0.1}}},
    {"isfStripes", {{"width", 0.1}, {"vertical", 1.0}}},
    {"isfSineWarpGradient", {{"size", 0.7}, {"angle", 0.2}}},
    {"isfGraphPaper", {{"majorDivisions", 6.0}}},
    {"isfWorleyCells", {{"density", 8.0}}},
};

Message addEffect(Client& client, const std::string& type, Params params = {})
{
    client.send(msg::command(azl::AddEffect{1, std::nullopt, {0, type, std::move(params)}}));
    return client.next();
}

azl::ClipRef generatorClip(const std::string& type, Params params = {})
{
    return {.name = type, .effects = {azl::Effect{0, type, std::move(params)}}};
}

void playGenerator(Client& client, const std::string& type, Params params = {})
{
    apply(client, azl::SetQuantise{0});
    apply(client, azl::SetSlot{1, 1, generatorClip(type, std::move(params))});
    client.send({{"type", "trigger"}, {"op", "clip"}, {"layer", 1}, {"column", 1}});
    REQUIRE(client.next()["op"] == "triggerClip");
}

size_t differing(const azl::Image& a, const azl::Image& b)
{
    size_t count = 0;
    for (size_t i = 0; i < a.rgba.size(); i += 4)
        count += std::abs(a.rgba[i] - b.rgba[i]) + std::abs(a.rgba[i + 1] - b.rgba[i + 1]) +
                     std::abs(a.rgba[i + 2] - b.rgba[i + 2]) >
                 24;
    return count;
}

size_t lit(const azl::Image& image)
{
    size_t count = 0;
    for (size_t i = 0; i < image.rgba.size(); i += 4)
        count += image.rgba[i] + image.rgba[i + 1] + image.rgba[i + 2] > 30;
    return count;
}

azl::IsfShader parsed(const azl::IsfSource& source)
{
    auto result = azl::parseIsf(source);
    REQUIRE(std::holds_alternative<azl::IsfShader>(result));
    return std::get<azl::IsfShader>(result);
}

} // namespace

TEST_CASE("every built-in ISF shader loads: effects and generators register under ISF, transitions as modes")
{
    std::vector<std::string> types;
    for (const azl::IsfSource& source : azl::isfSources()) {
        CAPTURE(source.name);
        const azl::IsfShader shader = parsed(source);
        const azl::IsfGlsl glsl = azl::translateIsf(shader, {});
        CHECK_MESSAGE(glsl.error.empty(), glsl.error);
        if (shader.kind != azl::IsfKind::Transition)
            types.push_back(azl::isfType(source.name));
    }
    std::vector<std::string> expected;
    for (const Look& look : kEffects)
        expected.push_back(look.type);
    for (const Look& look : kGenerators)
        expected.push_back(look.type);
    std::sort(types.begin(), types.end());
    std::sort(expected.begin(), expected.end());
    CHECK(types == expected);
    for (const Look& look : kGenerators)
        CHECK(azl::isGenerator(look.type));
    for (const Look& look : kEffects)
        CHECK(azl::findEffect(look.type)->category == "ISF");
    CHECK(azl::effectLabel("isfChromaticAberration") == "Chromatic Aberration");

    azl::prepareIsf({});
    for (const std::string& type : types)
        CHECK(azl::findEffect(type)->glsl.find("#error") == std::string::npos);
    std::vector<azl::TransitionMode> modes;
    for (const azl::IsfTransition& t : *azl::isfTransitions())
        if (!t.function.empty())
            modes.push_back(t.mode);
    CHECK(modes == std::vector<azl::TransitionMode>{azl::TransitionMode::Radial, azl::TransitionMode::RandomSquares,
                                                    azl::TransitionMode::WindowSlice});
}

TEST_CASE("golden frames of every built-in ISF effect and generator at fixed parameters and time")
{
    azl::Image card;
    {
        azl::EngineHarness harness(kGolden);
        harness.step(30);
        card = harness.readScreen();
    }
    for (const Look& look : kEffects) {
        CAPTURE(look.type);
        azl::EngineHarness harness(kGolden);
        Client client(harness.document());
        REQUIRE(addEffect(client, look.type, look.params)["type"] == "change");
        harness.step(30);
        const azl::Image image = harness.readScreen();
        CHECK(differing(image, card) > image.width * image.height / 50);
        checkGolden(image, look.type);
    }
    for (const Look& look : kGenerators) {
        CAPTURE(look.type);
        azl::EngineHarness harness(kGolden);
        Client client(harness.document());
        playGenerator(client, look.type, look.params);
        harness.step(30);
        const azl::Image image = harness.readScreen();
        CHECK(lit(image) > image.width * image.height / 10);
        checkGolden(image, look.type);
    }
}

TEST_CASE("macros, modulators and presets drive ISF parameters without recompiling")
{
    TempDir root;
    azl::EngineHarness harness({.width = 160, .height = 90}, 60.0, root.path);
    Client client(harness.document());
    const uint64_t effect = addEffect(client, "isfRgbEq", {{"red", 0.0}, {"green", 0.0}})["effect"]["id"];
    harness.step(2);
    CHECK(pixel(harness.readScreen(), 1, 1)[0] < 3);
    const uint64_t pipelines = harness.compileStats().pipelines;

    // The macro adds 0..2 to red's base of 0.
    const Message macro = apply(client, azl::AddMacro{std::nullopt, {.name = "Red", .value = 0.25}});
    const uint64_t id = macro["macro"]["id"];
    apply(client, azl::AddMacroTarget{id, std::nullopt, {{1, effect, "red"}, {}, 0.0, 2.0}});
    harness.step(4);
    CHECK(std::abs(pixel(harness.readScreen(), 1, 1)[0] - 128) < 8);
    apply(client, azl::SetMacroValue{id, 0.5});
    harness.step(4);
    CHECK(pixel(harness.readScreen(), 1, 1)[0] > 250);

    azl::Modulator square;
    square.shape = azl::LfoShape::Square;
    square.beats = 1.0;
    square.target = {0, {1, effect, "green"}};
    apply(client, azl::AddModulator{std::nullopt, square});
    int on = 0, off = 0;
    for (int i = 0; i < 60; ++i) {
        harness.step();
        (pixel(harness.readScreen(), 1, 1)[1] > 128 ? on : off)++;
    }
    CHECK(on > 10);
    CHECK(off > 10);

    apply(client, azl::SetEffectParam{effect, "blue", 0.5});
    client.send({{"type", "preset"}, {"op", "save"}, {"effect", effect}, {"name", "Half blue"}});
    CHECK(client.next()["params"]["blue"] == 0.5);
    apply(client, azl::SetEffectParam{effect, "blue", 2.0});
    client.send({{"type", "preset"}, {"op", "apply"}, {"effect", effect}, {"name", "Half blue"}});
    const Message applied = client.next();
    CHECK(applied["op"] == "setEffectParams");
    CHECK(applied["params"]["blue"] == 0.5);
    harness.step(2);
    CHECK(std::abs(pixel(harness.readScreen(), 1, 1)[2] - 128) < 8);
    CHECK(harness.compileStats().pipelines == pipelines);
}

TEST_CASE("an ISF shader that does not translate reports its error when used, and the engine keeps rendering")
{
    azl::IsfShader shader = parsed({"Test Broken", R"(/*{"INPUTS": [{"NAME": "inputImage", "TYPE": "image"}]}*/
void main() { gl_FragColor = IMG_THIS_PIXEL(inputImage) * notDeclared; })"});
    const azl::IsfGlsl glsl = azl::translateIsf(shader, {});
    REQUIRE_FALSE(glsl.error.empty());
    azl::EffectDef def = azl::isfEffect(shader);
    def.type = "testIsfBroken";
    if (!azl::findEffect(def.type))
        azl::registerEffect(def);

    azl::EngineHarness harness({.width = 160, .height = 90});
    Client client(harness.document());
    azl::applyIsf(shader, glsl, "testIsfBroken");
    harness.step();
    REQUIRE(addEffect(client, "testIsfBroken")["type"] == "change");
    const uint64_t rendered = harness.stats().rendered;
    harness.step(3);
    const Message failure = client.next();
    CHECK(failure["type"] == "error");
    CHECK(failure["chain"] == std::vector<std::string>{"testIsfBroken"});
    CHECK(failure.dump().find("notDeclared") != std::string::npos);
    CHECK(harness.stats().rendered == rendered + 3);
    CHECK(pixel(harness.readScreen(), 1, 1)[0] > 250);
}

// Information: every last pass carries the ISF transitions, so switching a transition never compiles; this is what
// they add to compiling one.
TEST_CASE("compile time the ISF transitions add to a last pass")
{
    azl::prepareIsf({});
    auto device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    const std::vector<const azl::EffectDef*> chain{azl::findEffect("colorize")};
    int run = 0;
    auto compileMs = [&] {
        const std::string source = azl::passMsl(azl::planChain(azl::SourceKind::Yuv, chain).back());
        double best = 1e9;
        for (int i = 0; i < 5; ++i) {
            // A comment of its own each time, so no shader cache answers.
            const std::string text = source + "\n// " + std::to_string(getpid()) + "-" + std::to_string(run++) + "\n";
            const auto start = std::chrono::steady_clock::now();
            NS::Error* error = nullptr;
            auto library = NS::TransferPtr(
                device->newLibrary(NS::String::string(text.c_str(), NS::UTF8StringEncoding), nullptr, &error));
            REQUIRE(library);
            best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        return best;
    };
    const double with = compileMs();
    const azl::IsfTransitions shaders = *azl::isfTransitions();
    azl::setIsfTransitions({});
    const double without = compileMs();
    azl::setIsfTransitions(shaders);
    char line[128];
    std::snprintf(line, sizeof line, "last pass library compile: %.1f ms with the ISF transitions, %.1f ms without", with,
                  without);
    MESSAGE(line);
    std::printf("%s\n", line);
}

// The compiler inlines a last pass's chain wherever the fade code evaluates a picture, so a transition case that
// evaluated its own would make every last pass slower to compile.
TEST_CASE("the ISF transitions add no evaluation of a last pass's pictures")
{
    azl::prepareIsf({});
    const std::vector<const azl::EffectDef*> chain{azl::findEffect("colorize")};
    auto evaluations = [&] {
        const std::string source = azl::planChain(azl::SourceKind::Yuv, chain).back().glsl;
        size_t count = 0;
        for (const std::string call : {"fadeIncoming(", "fadeOutgoing(", "INCOMING(", "OUTGOING("})
            for (size_t at = source.find(call); at != std::string::npos; at = source.find(call, at + 1))
                ++count;
        return count;
    };
    const azl::IsfTransitions shaders = *azl::isfTransitions();
    REQUIRE_FALSE(shaders.empty());
    const size_t with = evaluations();
    azl::setIsfTransitions({});
    const size_t without = evaluations();
    azl::setIsfTransitions(shaders);
    CHECK(with == without);
}

TEST_CASE("GPU time per built-in ISF shader at 1080p")
{
    auto measure = [](const std::function<void(Client&)>& setup) {
        azl::EngineHarness harness({.width = 1920, .height = 1080});
        Client client(harness.document());
        setup(client);
        harness.step(30);
        const uint64_t before = harness.stats().gpuMicros;
        harness.step(120);
        return double(harness.stats().gpuMicros - before) / 120 / 1000.0;
    };
    char text[64];
    std::snprintf(text, sizeof text, "%.3f", measure([](Client&) {}));
    std::string line = std::string("GPU ms/frame at 1080p: test card ") + text;
    for (const Look& look : kEffects) {
        std::snprintf(text, sizeof text, "%.3f", measure([&](Client& c) { addEffect(c, look.type, look.params); }));
        line += ", " + look.type + " " + text;
    }
    for (const Look& look : kGenerators) {
        std::snprintf(text, sizeof text, "%.3f",
                      measure([&](Client& c) { playGenerator(c, look.type, look.params); }));
        line += ", " + look.type + " " + text;
    }
    MESSAGE(line);
    std::printf("%s\n", line.c_str());
}
