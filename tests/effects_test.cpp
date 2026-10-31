#include "audio/audio_analysis.hpp"
#include "engine/chain_compiler.hpp"
#include "engine/frame_ring.hpp"
#include "engine/shader_cross.hpp"
#include "golden.hpp"
#include "gpu/glsl.hpp"
#include "platform/process.hpp"
#include "test_client.hpp"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <stdexcept>
#include <vector>

namespace {

namespace gpu = azl::gpu;

// Builds the pass's pipeline as the chain compiler does, into `format`; throws when it does not build.
void buildPass(const gpu::Device& device, const azl::PlannedPass& pass, gpu::Format format)
{
    std::string error;
    const gpu::ShaderModule module =
        device.makeShaderModule(gpu::compileGlsl(azl::fullscreenEntries(pass.glsl, {"passMain"})), &error);
    if (!module || !device.makeRenderPipeline(
                       {module, gpu::entryPoint("fullscreen"), gpu::entryPoint("passMain"), format}, {}, nullptr, &error))
        throw std::runtime_error(error);
}

using azl::AddEffect;
using azl::SetEffectParam;

// The test card's yellow colour bar (0.75, 0.75, 0) at 960x540.
constexpr uint32_t kYellowX = 255;
constexpr uint32_t kYellowY = 90;

Message addEffect(Client& client, const std::string& type, std::map<std::string, double> params = {},
                  uint64_t layer = 1)
{
    client.send(msg::command(AddEffect{layer, std::nullopt, {0, type, std::move(params)}}));
    return client.next();
}

Message setParam(Client& client, uint64_t effect, const std::string& param, double value)
{
    client.send(msg::command(SetEffectParam{effect, param, value}));
    return client.next();
}

void checkRgb(const azl::Image& image, uint32_t x, uint32_t y, int r, int g, int b, int tolerance = 3)
{
    const uint8_t* p = pixel(image, x, y);
    INFO("pixel " << x << "," << y << " is " << int(p[0]) << "," << int(p[1]) << "," << int(p[2]));
    CHECK(std::abs(p[0] - r) <= tolerance);
    CHECK(std::abs(p[1] - g) <= tolerance);
    CHECK(std::abs(p[2] - b) <= tolerance);
}

// Reads a 3x3 neighbourhood but inverts only the centre, so its output is easy to predict.
azl::EffectDef fakeNeighbourhood()
{
    return {"testInvert", azl::StageKind::Neighbourhood, {}, false, R"(
    vec4 sum = vec4(0.0);
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i)
            sum += samplePixel(t, px + vec2(i, j)) * (i == 0 && j == 0 ? 1.0 : 0.0);
    return vec4(1.0 - sum.rgb, sum.a);
)", {}};
}

// A test-only effect whose GLSL does not compile.
azl::EffectDef brokenEffect()
{
    return {"testBroken", azl::StageKind::Pointwise, {}, false, "\n    return c * undefinedValue;\n", {}};
}

void registerOnce(azl::EffectDef effect)
{
    if (!azl::findEffect(effect.type))
        azl::registerEffect(std::move(effect));
}

// Layer 1 under a red Colorize, layer 2 above it shrunk into the left half by a Transform.
azl::Composition twoLayers()
{
    azl::Composition composition;
    composition.layers.push_back({.id = 1, .name = "Layer 1", .effects = {{1, "colorize", {}}}});
    composition.layers.push_back(
        {.id = 2, .name = "Layer 2", .effects = {{2, "transform", {{"positionX", -0.25}, {"scale", 0.5}}}}});
    return composition;
}

// A background pixel of the test card as layer 2 shows it, inside its shrunk picture.
constexpr uint32_t kLayer2X = 130;
constexpr uint32_t kLayer2Y = 200;

std::vector<size_t> stageIndexes(const azl::PlannedPass& pass)
{
    std::vector<size_t> indexes;
    for (const azl::PlannedStage& stage : pass.stages)
        indexes.push_back(stage.chainIndex);
    return indexes;
}

} // namespace

TEST_CASE("a Transform + Colorize chain on a clip renders in one fused pass")
{
    TempDir media;
    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    playMedia(client, copyMedia("count_709_tv.mp4", media.path / "count.mp4"));
    REQUIRE(addEffect(client, "transform", {{"positionX", 0.1}, {"positionY", -0.05}, {"scale", 0.8}, {"rotation", 12}})
                ["type"] == "change");
    REQUIRE(addEffect(client, "colorize", {{"hue", 0.55}, {"saturation", 0.8}})["type"] == "change");

    // Clip frame 42 (0b00101010), so the bar code shows black and white bits.
    harness.step(85);
    checkGolden(harness.readScreen(), "transform_colorize");
    CHECK(harness.stats().passes == 1);
}

TEST_CASE("animated effect parameters never recompile")
{
    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    const uint64_t transform = addEffect(client, "transform")["effect"]["id"];
    const uint64_t colorize = addEffect(client, "colorize", {{"mix", 0.0}})["effect"]["id"];
    harness.step();
    checkRgb(harness.readScreen(), kYellowX, kYellowY, 191, 191, 0);
    const uint64_t pipelines = harness.compileStats().pipelines;

    azl::Image previous = harness.readScreen();
    for (int i = 1; i <= 5; ++i) {
        REQUIRE(setParam(client, transform, "rotation", 3.0 * i)["type"] == "change");
        REQUIRE(setParam(client, colorize, "mix", i / 5.0)["type"] == "change");
        harness.step();
        const azl::Image image = harness.readScreen();
        CHECK(image.rgba != previous.rgba);
        previous = image;
    }
    REQUIRE(setParam(client, transform, "rotation", 0.0)["type"] == "change");
    harness.step();
    checkRgb(harness.readScreen(), kYellowX, kYellowY, 177, 0, 0);
    CHECK(harness.compileStats().pipelines == pipelines);
    CHECK(harness.stats().passes == 1);
}

TEST_CASE("while an edited chain compiles the layer keeps its previous chain and frames never wait")
{
    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    harness.step();
    const uint64_t pipelines = harness.compileStats().pipelines;
    const uint64_t rendered = harness.stats().rendered;

    harness.holdCompiles(true);
    REQUIRE(addEffect(client, "colorize")["type"] == "change");
    harness.step(3);
    CHECK(harness.stats().rendered == rendered + 3);
    CHECK(harness.stats().skipped == 0);
    CHECK(harness.compileStats().pipelines == pipelines);
    checkRgb(harness.readScreen(), kYellowX, kYellowY, 191, 191, 0);

    harness.holdCompiles(false);
    harness.step();
    CHECK(harness.compileStats().pipelines > pipelines);
    checkRgb(harness.readScreen(), kYellowX, kYellowY, 177, 0, 0);

    // Undo returns to a chain that is still compiled: no compile, and the very next frame shows it.
    const uint64_t compiled = harness.compileStats().pipelines;
    harness.holdCompiles(true);
    client.send(msg::undo());
    REQUIRE(client.next()["op"] == "removeEffect");
    harness.step();
    checkRgb(harness.readScreen(), kYellowX, kYellowY, 191, 191, 0);
    CHECK(harness.compileStats().pipelines == compiled);
    harness.holdCompiles(false);
}

TEST_CASE("a layer whose chain is still compiling shows nothing, never its source bare")
{
    azl::EngineHarness harness({.width = 960, .height = 540}, 60.0, {}, azl::Composition{});
    Client client(harness.document());
    harness.step();

    harness.holdCompiles(true);
    client.send(msg::command(azl::LoadComposition{twoLayers()}));
    REQUIRE(client.next()["type"] == "change");
    harness.step(3);
    checkRgb(harness.readScreen(), kYellowX, kYellowY, 0, 0, 0);
    checkRgb(harness.readScreen(), kLayer2X, kLayer2Y, 0, 0, 0);

    harness.holdCompiles(false);
    harness.step();
    checkRgb(harness.readScreen(), kYellowX, kYellowY, 177, 0, 0);
}

TEST_CASE("a second start on the same pipeline cache compiles nothing; a Paravirtual device keeps none")
{
    CHECK(azl::archivesWork("Apple M2 Pro"));
    CHECK(azl::archivesWork("Intel(R) UHD Graphics 630"));
    CHECK_FALSE(azl::archivesWork("Apple Paravirtual device"));
    const bool archives = azl::archivesWork(gpu::Device::systemDefault().name());
    const std::filesystem::path cache = std::filesystem::temp_directory_path() /
                                        ("nocturne-pipelines-" + std::to_string(azl::currentProcessId()) +
                                         gpu::kPipelineCacheExtension);
    std::filesystem::remove(cache);
    const std::string cachePath = cache.string();
    const azl::EngineConfig config{.width = 960, .height = 540, .pipelineCache = cachePath.c_str()};
    {
        azl::EngineHarness harness(config, 60.0, {}, twoLayers());
        harness.step();
        CHECK(harness.compileStats().pipelines > 0);
    }
    if (!archives) {
        // Every start compiles, as with no cache at all.
        CHECK_FALSE(std::filesystem::exists(cache));
        azl::EngineHarness harness(config, 60.0, {}, twoLayers());
        harness.step();
        CHECK(harness.compileStats().pipelines > 0);
        CHECK(harness.compileStats().archived == 0);
        return;
    }
    REQUIRE(std::filesystem::exists(cache));
    {
        azl::EngineHarness harness(config, 60.0, {}, twoLayers());
        harness.step();
        CHECK(harness.compileStats().pipelines == 0);
        CHECK(harness.compileStats().archived > 0);
        checkRgb(harness.readScreen(), kYellowX, kYellowY, 177, 0, 0);
    }
    std::filesystem::remove(cache);
}

TEST_CASE("effect edits are undoable and seen by every client")
{
    azl::EngineHarness harness({.width = 960, .height = 540});
    Client a(harness.document());
    Client b(harness.document());
    CHECK(a.snapshot["composition"]["layers"][0]["effects"].empty());

    const Message added = addEffect(a, "transform", {{"scale", 2.0}});
    CHECK(added["op"] == "addEffect");
    CHECK(added["index"] == 0);
    CHECK(added["effect"]["params"]["scale"] == 2.0);
    CHECK(added["effect"]["params"]["mix"] == 1.0);
    CHECK(b.next() == added);
    const uint64_t transform = added["effect"]["id"];
    const uint64_t colorize = addEffect(a, "colorize")["effect"]["id"];
    CHECK(colorize != transform);
    b.next();

    a.send(msg::command(azl::MoveEffect{colorize, 0}));
    CHECK(a.next()["op"] == "moveEffect");
    CHECK(setParam(a, transform, "rotation", 45.0)["value"] == 45.0);
    a.send(msg::command(azl::RemoveEffect{colorize}));
    CHECK(a.next()["op"] == "removeEffect");

    for (int i = 0; i < 3; ++i) {
        a.send(msg::undo());
        a.next();
    }
    Client late(harness.document());
    const Message& effects = late.snapshot["composition"]["layers"][0]["effects"];
    REQUIRE(effects.size() == 2);
    CHECK(effects[0]["id"] == transform);
    CHECK(effects[0]["params"]["rotation"] == 0.0);
    CHECK(effects[1]["id"] == colorize);

    a.send(msg::redo());
    CHECK(a.next()["index"] == 0);

    CHECK(addEffect(a, "nope")["type"] == "error");
    CHECK(setParam(a, transform, "scale", 9.0)["type"] == "error");
    CHECK(setParam(a, transform, "volume", 1.0)["type"] == "error");
    a.send(msg::command(azl::RemoveEffect{999}));
    CHECK(a.next()["type"] == "error");
}

TEST_CASE("the planner fuses pointwise and remap stages and cuts before a neighbourhood stage")
{
    const azl::EffectDef* transform = azl::findEffect("transform");
    const azl::EffectDef* colorize = azl::findEffect("colorize");
    const azl::EffectDef fake = fakeNeighbourhood();

    const auto fused = azl::planChain(azl::SourceKind::Yuv, {transform, colorize, transform});
    REQUIRE(fused.size() == 1);
    CHECK(fused[0].source == azl::SourceKind::Yuv);
    CHECK(fused[0].intoComposition);
    CHECK(stageIndexes(fused[0]) == std::vector<size_t>{0, 1, 2});

    const auto cut = azl::planChain(azl::SourceKind::Yuv, {transform, colorize, &fake, colorize, transform});
    REQUIRE(cut.size() == 2);
    CHECK(cut[0].source == azl::SourceKind::Yuv);
    CHECK(stageIndexes(cut[0]) == std::vector<size_t>{0, 1});
    CHECK_FALSE(cut[0].intoComposition);
    CHECK_FALSE(cut[0].float16);
    CHECK(cut[1].source == azl::SourceKind::Texture);
    CHECK(stageIndexes(cut[1]) == std::vector<size_t>{2, 3, 4});
    CHECK(cut[1].intoComposition);

    // The source is materialised first: its conversion never runs once per neighbourhood tap.
    const auto first = azl::planChain(azl::SourceKind::TestCard, {&fake});
    REQUIRE(first.size() == 2);
    CHECK(first[0].stages.empty());
    CHECK(stageIndexes(first[1]) == std::vector<size_t>{0});
    CHECK(azl::planChain(azl::SourceKind::Texture, {&fake}).size() == 1);

    azl::EffectDef headroom = fake;
    headroom.needsFloat16 = true;
    CHECK(azl::planChain(azl::SourceKind::Yuv, {&headroom, &fake})[1].float16);

    const gpu::Device device = gpu::Device::systemDefault();
    for (const azl::PlannedPass& pass : cut)
        CHECK_NOTHROW(buildPass(device, pass, azl::kFrameFormat));
}

TEST_CASE("a neighbourhood effect reads a materialised 8-bit intermediate")
{
    registerOnce(fakeNeighbourhood());
    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    REQUIRE(addEffect(client, "testInvert")["type"] == "change");
    harness.step();
    CHECK(harness.stats().passes == 2);
    checkRgb(harness.readScreen(), kYellowX, kYellowY, 64, 64, 255);

    REQUIRE(addEffect(client, "colorize", {{"saturation", 0.0}})["type"] == "change");
    harness.step();
    CHECK(harness.stats().passes == 2);
    // Luminance of (64, 64, 255) once fused into the neighbourhood pass.
    checkRgb(harness.readScreen(), kYellowX, kYellowY, 78, 78, 78);
}

TEST_CASE("a chain that fails to compile is reported and its layer keeps its previous chain")
{
    registerOnce(brokenEffect());
    azl::EngineHarness harness({.width = 960, .height = 540}, 60.0, {}, twoLayers());
    Client client(harness.document());
    harness.step();
    checkRgb(harness.readScreen(), kYellowX, kYellowY, 177, 0, 0);
    checkRgb(harness.readScreen(), kLayer2X, kLayer2Y, 20, 20, 20);

    const Message added = addEffect(client, "testBroken", {}, 2);
    REQUIRE(added["type"] == "change");
    const uint64_t rendered = harness.stats().rendered;
    harness.step(3);
    const Message failure = client.next();
    CHECK(failure["type"] == "error");
    CHECK(failure["chain"] == std::vector<std::string>{"transform", "testBroken"});
    CHECK(harness.stats().rendered == rendered + 3);
    CHECK(harness.stats().passes == 2);
    checkRgb(harness.readScreen(), kLayer2X, kLayer2Y, 20, 20, 20);

    // The other layer still follows its parameters, and a failed chain is not compiled again on every edit.
    const uint64_t pipelines = harness.compileStats().pipelines;
    REQUIRE(setParam(client, 1, "hue", 1.0 / 3.0)["type"] == "change");
    harness.step();
    checkRgb(harness.readScreen(), kYellowX, kYellowY, 0, 177, 0);

    // Removing the broken effect returns to the chain that is still compiled.
    client.send(msg::command(azl::RemoveEffect{added["effect"]["id"]}));
    REQUIRE(client.next()["op"] == "removeEffect");
    harness.step();
    CHECK(harness.compileStats().pipelines == pipelines);
    checkRgb(harness.readScreen(), kLayer2X, kLayer2Y, 20, 20, 20);
}

TEST_CASE("a layer whose first chain fails to compile shows its source with no effects")
{
    registerOnce(brokenEffect());
    azl::Composition composition;
    composition.layers.push_back({.id = 1, .name = "Layer 1", .effects = {{1, "colorize", {}}, {2, "testBroken", {}}}});
    azl::EngineHarness harness({.width = 960, .height = 540}, 60.0, {}, composition);
    harness.step();
    CHECK(harness.stats().passes == 1);
    checkRgb(harness.readScreen(), kYellowX, kYellowY, 191, 191, 0);
}

namespace {

std::vector<const azl::EffectDef*> effects(std::initializer_list<const char*> types)
{
    std::vector<const azl::EffectDef*> defs;
    for (const char* type : types)
        defs.push_back(azl::findEffect(type));
    return defs;
}

uint64_t effectId(const Message& change)
{
    return change["effect"]["id"];
}

} // namespace

TEST_CASE("every v1 effect declares its stage kind and the planner fuses accordingly")
{
    using azl::SourceKind;
    const auto edge = azl::planChain(SourceKind::Yuv, effects({"transform", "colorize", "edgeDetection"}));
    REQUIRE(edge.size() == 2);
    CHECK(stageIndexes(edge[0]) == std::vector<size_t>{0, 1});
    CHECK(stageIndexes(edge[1]) == std::vector<size_t>{2});

    const auto edgeThenColour = azl::planChain(SourceKind::Yuv, effects({"edgeDetection", "colorize", "stripper"}));
    REQUIRE(edgeThenColour.size() == 2);
    CHECK(stageIndexes(edgeThenColour[1]) == std::vector<size_t>{0, 1, 2});

    CHECK(azl::planChain(SourceKind::Yuv, effects({"transform", "stripper", "colorize", "reveal"})).size() == 1);

    // Trails keeps one full-resolution 16F history and fuses the stages around it.
    const auto trails = azl::planChain(SourceKind::Yuv, effects({"colorize", "trails", "transform"}));
    REQUIRE(trails.size() == 2);
    CHECK(trails[0].side == 0u);
    CHECK(trails[0].divisor == 1);
    CHECK(trails[0].float16);
    CHECK(stageIndexes(trails[1]) == std::vector<size_t>{0, 1, 2});
    CHECK(trails[1].intoComposition);

    const gpu::Device device = gpu::Device::systemDefault();
    for (const auto& chain : {trails, azl::planChain(SourceKind::Texture, effects({"classicTrails", "trails"}))})
        for (const azl::PlannedPass& pass : chain)
            CHECK_NOTHROW(buildPass(device, pass, pass.float16 ? gpu::Format::RGBA16F : azl::kFrameFormat));
}

TEST_CASE("frame stats show the passes and full-resolution intermediates of the fused chains")
{
    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    addEffect(client, "transform");
    addEffect(client, "colorize");
    const uint64_t edge = effectId(addEffect(client, "edgeDetection"));
    harness.step();
    CHECK(harness.stats().passes == 2);
    CHECK(harness.stats().intermediates == 1);

    client.send(msg::command(azl::RemoveEffect{edge}));
    client.next();
    addEffect(client, "trails");
    harness.step();
    CHECK(harness.stats().passes == 2);
    CHECK(uint64_t(harness.stats().intermediates) == 1);

    addEffect(client, "edgeDetection");
    harness.step();
    CHECK(harness.stats().passes == 3);
    CHECK(uint64_t(harness.stats().intermediates) == 2);
}

TEST_CASE("golden frames of EdgeDetection, Stripper, Colorize, Reveal and Trails")
{
    SUBCASE("edge detection fused with colorize")
    {
        azl::EngineHarness harness({.width = 960, .height = 540});
        Client client(harness.document());
        // A shift that keeps the card's one-pixel lines on whole pixels: a rotated or scaled card is point-sampled at
        // arbitrary positions, where Intel and Apple GPUs pick different pixels of a line.
        addEffect(client, "transform", {{"positionX", 0.05}, {"positionY", -0.1}});
        addEffect(client, "edgeDetection", {{"multiplier", 0.5}});
        addEffect(client, "colorize", {{"hue", 0.5}, {"saturation", 0.6}});
        harness.step();
        checkGolden(harness.readScreen(), "edge_detection");
    }
    SUBCASE("stripper")
    {
        azl::EngineHarness harness({.width = 960, .height = 540});
        Client client(harness.document());
        addEffect(client, "stripper",
                  {{"number", 7.0}, {"direction", 30.0}, {"speed", 0.5}, {"spread", 0.6}, {"width", 0.4},
                   {"feather", 0.3}, {"red", 0.2}, {"blue", 0.4}});
        harness.step(30);
        checkGolden(harness.readScreen(), "stripper");
    }
    SUBCASE("stripper with random widths through the Add mixer")
    {
        azl::EngineHarness harness({.width = 960, .height = 540});
        Client client(harness.document());
        addEffect(client, "stripper",
                  {{"number", 9.0}, {"speed", 0.3}, {"width", 0.7}, {"feather", 0.2}, {"alpha", 0.0},
                   {"minWidth", 0.2}, {"maxWidth", 0.8}, {"add", 1.0}});
        harness.step(30);
        checkGolden(harness.readScreen(), "stripper_add");
    }
    SUBCASE("stripper with strips of their own direction and speed")
    {
        azl::EngineHarness harness({.width = 960, .height = 540});
        Client client(harness.document());
        addEffect(client, "stripper",
                  {{"number", 9.0}, {"direction", 20.0}, {"speed", 0.6}, {"width", 0.35}, {"feather", 0.3},
                   {"directionSpread", 0.25}, {"speedSpread", 0.8}});
        harness.step(30);
        checkGolden(harness.readScreen(), "stripper_spread");
    }
    SUBCASE("stripper swinging with jitter, reshuffled every quarter beat")
    {
        azl::EngineHarness harness({.width = 960, .height = 540});
        Client client(harness.document());
        addEffect(client, "stripper",
                  {{"number", 8.0}, {"speed", 0.7}, {"width", 0.6}, {"feather", 0.2}, {"minWidth", 0.3},
                   {"movement", 1.0}, {"jitter", 0.6}, {"reshuffle", 0.25}});
        harness.step(30);
        checkGolden(harness.readScreen(), "stripper_swing");
    }
    SUBCASE("stripper stepping")
    {
        azl::EngineHarness harness({.width = 960, .height = 540});
        Client client(harness.document());
        addEffect(client, "stripper", {{"number", 6.0}, {"direction", 90.0}, {"speed", 1.1}, {"movement", 2.0}});
        harness.step(30);
        checkGolden(harness.readScreen(), "stripper_step");
    }
    SUBCASE("colorize darkened, inverted and partly mixed")
    {
        azl::EngineHarness harness({.width = 960, .height = 540});
        Client client(harness.document());
        addEffect(client, "colorize",
                  {{"hue", 0.05}, {"saturation", 0.9}, {"brightness", 0.6}, {"invert", 1.0}, {"alpha", 0.8}});
        harness.step();
        checkGolden(harness.readScreen(), "colorize_brightness");
    }
    SUBCASE("reveal")
    {
        azl::EngineHarness harness({.width = 960, .height = 540});
        Client client(harness.document());
        addEffect(client, "reveal",
                  {{"progress", 0.6}, {"columns", 6.0}, {"rows", 4.0}, {"gap", 0.08}, {"seed", 3.5},
                   {"softness", 0.1}, {"black", 1.0}});
        harness.step();
        checkGolden(harness.readScreen(), "reveal");
    }
    SUBCASE("trails of a moving picture")
    {
        azl::EngineHarness harness({.width = 960, .height = 540});
        Client client(harness.document());
        const uint64_t transform = effectId(addEffect(client, "transform"));
        addEffect(client, "trails", {{"feedback", 0.85}});
        // Steps of 19.2 x 10.8 pixels never put a pixel centre on a pixel edge of the point-sampled card.
        for (int i = 0; i < 12; ++i) {
            setParam(client, transform, "positionX", 0.02 * i);
            setParam(client, transform, "positionY", -0.02 * i);
            harness.step();
        }
        checkGolden(harness.readScreen(), "trails");
    }
}

namespace {

// The pixels a Reveal gap hides: those that differ from the same frame with no gap. Over a non-black picture that is
// the gap exactly; a black patch of the card can only miss gap pixels, never add any, so a maximum over several
// lines across the gap reads its width.
std::vector<bool> hiddenByGap(const azl::Image& gapped, const azl::Image& plain)
{
    std::vector<bool> hidden(gapped.rgba.size() / 4);
    for (size_t i = 0; i < hidden.size(); ++i)
        for (int ch = 0; ch < 3; ++ch)
            if (std::abs(int(gapped.rgba[4 * i + ch]) - int(plain.rgba[4 * i + ch])) > 64)
                hidden[i] = true;
    return hidden;
}

// The width of the gap between two cells, measured as the longest run of hidden pixels on a line through the middle
// of a cell and across its edge into the next: half a cell either side of the edge, which never reaches another gap.
struct GapWidths {
    int across; // width of the gap between two cells side by side, in pixels along x
    int down;   // width of the gap between two cells one above the other, in pixels along y
};

GapWidths gapWidths(const azl::Image& gapped, const azl::Image& plain, int columns, int rows)
{
    const std::vector<bool> hidden = hiddenByGap(gapped, plain);
    const int w = int(gapped.width), h = int(gapped.height);
    const int cellW = w / columns, cellH = h / rows;
    auto run = [&](auto at, int from, int to) {
        int best = 0, current = 0;
        for (int i = from; i < to; ++i) {
            current = hidden[at(i)] ? current + 1 : 0;
            best = std::max(best, current);
        }
        return best;
    };
    GapWidths widths{0, 0};
    // Lines through the middle of each cell row, across the first interior vertical edge; and through the middle of
    // each cell column, down the first interior horizontal edge. Half a cell either side never reaches another gap.
    for (int r = 0; r < rows; ++r) {
        const int y = r * cellH + cellH / 2;
        widths.across = std::max(widths.across, run([&](int x) { return size_t(y) * w + x; }, cellW / 2, cellW + cellW / 2));
    }
    for (int c = 0; c < columns; ++c) {
        const int x = c * cellW + cellW / 2;
        widths.down = std::max(widths.down, run([&](int y) { return size_t(y) * w + x; }, cellH / 2, cellH + cellH / 2));
    }
    return widths;
}

} // namespace

TEST_CASE("Reveal's gap is as wide across as down whatever the cell's shape, and the same at every render scale")
{
    // 1920 x 1080 at 5 x 4 is 384 x 270 cells: a gap as a fraction of the cell would be 1.42 times thinner down.
    constexpr int kColumns = 5, kRows = 4;
    constexpr double kGap = 0.2;
    // The test card through a Reveal with everything shown but the gap: progress 1 with no softness, hidden parts
    // black. A gap below 0 shoots the card with no effect at all.
    auto shoot = [&](double gap, int renderScale) {
        azl::EngineHarness harness({.width = 1920, .height = 1080});
        Client client(harness.document());
        harness.engine().setRenderScale(renderScale);
        if (gap >= 0.0)
            addEffect(client, "reveal",
                      {{"progress", 1.0}, {"columns", kColumns}, {"rows", kRows}, {"gap", gap}, {"softness", 0.0},
                       {"black", 1.0}});
        harness.step(2);
        return harness.readScreen();
    };

    SUBCASE("gap 0 shows the whole picture")
    {
        CHECK(shoot(0.0, 100).rgba == shoot(-1.0, 100).rgba);
    }
    SUBCASE("at render scale 100 %")
    {
        const GapWidths widths = gapWidths(shoot(kGap, 100), shoot(0.0, 100), kColumns, kRows);
        INFO("across " << widths.across << " down " << widths.down);
        // A fifth of the shorter side, 270: 54 pixels.
        CHECK(std::abs(widths.across - widths.down) <= 1);
        CHECK(std::abs(widths.across - 54) <= 1);
    }
    SUBCASE("at render scale 67 %, in output pixels")
    {
        const GapWidths widths = gapWidths(shoot(kGap, 67), shoot(0.0, 67), kColumns, kRows);
        INFO("across " << widths.across << " down " << widths.down);
        CHECK(std::abs(widths.across - widths.down) <= 1);
        CHECK(std::abs(widths.across - 54) <= 2);
    }
}

TEST_CASE("Trails history ping-pongs across frames, survives parameter and chain edits and a clip change")
{
    TempDir media;
    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    // The test card's white left edge is at x = 0; then the card moves away and leaves its fading history there.
    const uint64_t transform = effectId(addEffect(client, "transform"));
    const uint64_t trails = effectId(addEffect(client, "trails", {{"feedback", 0.5}}));
    harness.step();
    checkRgb(harness.readScreen(), 0, 270, 255, 255, 255);

    setParam(client, transform, "positionX", 0.5);
    harness.step();
    checkRgb(harness.readScreen(), 0, 270, 128, 128, 128);
    harness.step();
    checkRgb(harness.readScreen(), 0, 270, 64, 64, 64);

    setParam(client, trails, "feedback", 1.0);
    harness.step(3);
    checkRgb(harness.readScreen(), 0, 270, 64, 64, 64);

    addEffect(client, "colorize", {{"saturation", 0.0}});
    harness.step();
    checkRgb(harness.readScreen(), 0, 270, 64, 64, 64);

    setParam(client, transform, "positionX", 0.0);
    harness.step();
    setParam(client, transform, "positionX", 0.5);
    harness.step();
    checkRgb(harness.readScreen(), 0, 270, 255, 255, 255);
    // A layer's Trails runs on through a clip change: the history stays where the new clip leaves it uncovered.
    playMedia(client, copyMedia("count_709_tv.mp4", media.path / "count.mp4"));
    harness.step();
    checkRgb(harness.readScreen(), 0, 270, 255, 255, 255);
}

TEST_CASE("dry/wet at 0 shows the unaffected picture for every effect")
{
    azl::EngineHarness plain({.width = 960, .height = 540});
    plain.step(4);
    const azl::Image expected = plain.readScreen();

    for (const char* type : {"transform", "colorize", "edgeDetection", "stripper", "trails", "reveal",
                             "classicStripper", "classicColorize", "classicBrightnessContrast", "classicLevels",
                             "classicThreshold", "classicHueRotate", "classicMirrorQuad", "classicPolarKaleido",
                             "classicTrails", "classicStrobe", "classicShiftGlitch", "classicEdgeDetection"}) {
        CAPTURE(type);
        azl::EngineHarness harness({.width = 960, .height = 540});
        Client client(harness.document());
        addEffect(client, "transform", {{"rotation", 20.0}, {"mix", 0.0}});
        addEffect(client, type, {{"mix", 0.0}});
        harness.step(4);
        CHECK(harness.readScreen().rgba == expected.rgba);
    }
}

TEST_CASE("an added layer with a black-fill Classic Stripper renders only inside its strips, looking the same")
{
    // Passes its input through; it says nothing about black, so a chain ending in it renders every pixel.
    registerOnce({"testPassThrough", azl::StageKind::Pointwise, {}, false, "\n    return c;\n", {}});
    struct Case {
        const char* name;
        azl::BlendMode blend = azl::BlendMode::Add;
        std::map<std::string, double> strips{};
        std::map<std::string, double> colorize{};
    };
    const Case cases[] = {
        {"add"},
        {"screen", azl::BlendMode::Screen},
        {"normal", azl::BlendMode::Normal},
        {"red fill", azl::BlendMode::Add, {{"red", 0.6}}},
        {"inverted strips", azl::BlendMode::Add, {{"mode", 0.34}}},
        {"stripper add mixer", azl::BlendMode::Add, {{"blendMode", 1.0}, {"mix", 0.7}}},
        {"stripper half wet", azl::BlendMode::Add, {{"mix", 0.5}}},
        {"inverting colorize", azl::BlendMode::Add, {}, {{"invert", 1.0}}},
    };
    for (const Case& c : cases) {
        const std::string name = c.name;
        CAPTURE(name);
        auto render = [&](bool whole) {
            std::map<std::string, double> strips{{"number", 0.8}, {"mode", 0.2}, {"movementType", 0.4},
                                                 {"globalWidth", 0.07}, {"speed", 0.03}};
            for (const auto& [name, value] : c.strips)
                strips[name] = value;
            std::map<std::string, double> colorize{{"colorGreen", 1.0}, {"colorBlue", 0.5}};
            for (const auto& [name, value] : c.colorize)
                colorize[name] = value;
            azl::Composition composition;
            composition.layers.push_back({.id = 1, .name = "below", .effects = {{1, "colorize", {{"hue", 0.6}}}}});
            std::vector<azl::Effect> chain{{2, "edgeDetection", {{"multiplier", 0.8}}},
                                           {3, "classicStripper", strips},
                                           {4, "classicColorize", colorize}};
            if (whole)
                chain.push_back({5, "testPassThrough", {}});
            composition.layers.push_back({.id = 2, .name = "strips", .effects = chain, .blend = c.blend});
            azl::EngineHarness harness({.width = 960, .height = 540}, 60.0, {}, composition);
            harness.step(20);
            return harness.readScreen();
        };
        CHECK(render(false).rgba == render(true).rgba);
    }
}

TEST_CASE("a stage at dry/wet 0 returns its input before evaluating its effect, so a bypassed effect costs nothing")
{
    // One effect of each stage kind; its dry/wet is the last of its parameters in the pass's buffer.
    for (const char* type : {"colorize", "transform", "edgeDetection", "trails"}) {
        CAPTURE(type);
        const auto passes = azl::planChain(azl::SourceKind::Texture, effects({type}));
        const std::string& metal = passes.back().glsl;
        const std::string wet = "pb[" + std::to_string(azl::findEffect(type)->params.size() - 1) + "]";
        const size_t guard = metal.find("if (" + wet + " == 0.0) return");
        REQUIRE(guard != std::string::npos);
        CHECK(guard < metal.find(std::string("fx_") + type + "(", metal.find("vec4 s1(")));
    }
}

namespace {

using Edits = std::vector<std::pair<std::string, std::string>>;

// The effect copied as `full` with each edit's text replaced in its shaders, every one of which must be found: its
// no-op skips taken out. The copy is a variant, so no list of effect or generator types shows it; a generator's copy
// is a plain effect, which on a layer draws the same picture as it ignores its input.
void registerFull(const std::string& type, const std::string& full, const Edits& edits)
{
    if (azl::findEffect(full))
        return;
    azl::EffectDef def = *azl::findEffect(type);
    def.type = full;
    def.params.pop_back();
    def.variants.clear();
    def.variant = true;
    def.generator = false;
    for (const auto& [from, to] : edits) {
        CAPTURE(from);
        size_t found = 0;
        auto edit = [&](std::string& metal) {
            for (size_t at = metal.find(from); at != std::string::npos; at = metal.find(from, at + to.size())) {
                metal.replace(at, from.size(), to);
                ++found;
            }
        };
        edit(def.glsl);
        for (azl::SidePass& side : def.sidePasses)
            edit(side.glsl);
        REQUIRE(found > 0);
    }
    azl::registerEffect(std::move(def));
}

// The largest difference of any channel between two pictures of the same size.
int levelsApart(const azl::Image& a, const azl::Image& b)
{
    REQUIRE(a.rgba.size() == b.rgba.size());
    int most = 0;
    for (size_t i = 0; i < a.rgba.size(); ++i)
        most = std::max(most, std::abs(int(a.rgba[i]) - int(b.rgba[i])));
    return most;
}

} // namespace

TEST_CASE("a parameter at its no-op value skips the work it would do for nothing, with the same picture")
{
    struct Case {
        const char* type;
        Edits edits;
        std::string param;
        double noop;
        double off;
        std::map<std::string, double> params{};
        bool clip = false;
    };
    const Case cases[] = {
        {"edgeDetection", {{"if (pb[p + 0] == 0.0)", "if (false)"}}, "multiplier", 0.0, 0.3},
        {"classicEdgeDetection", {{"if (pb[p + 1] != 0.0)", "if (true)"}}, "multiplier", 0.0, 0.3},
        {"classicEdgeDetection", {{"if (pb[p + 1] != 0.0)", "if (true)"}}, "multiplier", 0.0, 0.3, {{"mode", 0.0}}},
        {"trails", {{"if (pb[p + 0] == 0.0)", "if (false)"}}, "feedback", 0.0, 0.8, {}, true},
        {"classicShiftRgb", {{"i == 0 || a != last", "true"}, {"q == px ? c : ", ""}}, "g", 0.0, 0.02},
        {"classicShiftRgb", {{"i == 0 || a != last", "true"}, {"q == px ? c : ", ""}}, "g", 0.0, 0.02,
         {{"mode", 2.0}}},
        {"classicStripper_resample", {{"pb[p + 3] == 0.0 ? 1 : 4", "4"}}, "directionSpread", 0.0, 0.2,
         {{"number", 0.4}, {"mode", 0.5}, {"globalWidth", 0.5}}},
        {"reveal", {{"blend > 0.0 ? 2 : 1", "2"}}, "evolve", 0.0, 0.3},
        {"transform", {{"if (pb[p + 3] != 0.0)", "if (true)"}}, "rotation", 0.0, 3.0, {{"scale", 0.9}}},
        {"colorize", {{"if (pb[p + 4] == 0.0)", "if (false)"}}, "alpha", 0.0, 0.5},
        {"classicColorize", {{"if (pb[p + 3] != 0.0)", "if (true)"}}, "colorAlpha", 0.0, 0.5},
        {"classicColorize",
         {{"float keep = 0.0;\n        if (pb[p + 6] > 0.5)\n            keep = exp(-pow((hi - lo) / max(0.475 * pow(pb[p + 7], "
           "1.5), 1e-5), 1.5));",
           "float keep = pb[p + 6] > 0.5 ? exp(-pow((hi - lo) / max(0.475 * pow(pb[p + 7], 1.5), 1e-5), 1.5)) : 0.0;"}},
         "greysKeep", 0.0, 1.0, {{"colorGreen", 0.5}}},
        {"noise", {{"if (pb[p + 2] == 0.0)", "if (false)"}}, "contrast", 0.0, 0.5},
    };
    TempDir media;
    const fs::path clip = copyMedia("count_240.mp4", media.path / "clip.mp4");
    for (const Case& c : cases) {
        const std::string name = c.type;
        CAPTURE(name);
        CAPTURE(c.param);
        const std::string full = std::string(c.type) + "_" + c.param + "Full";
        registerFull(c.type, full, c.edits);
        auto render = [&](const std::string& type, double value) {
            std::map<std::string, double> params = c.params;
            params[c.param] = value;
            const bool generator = azl::isGenerator(type);
            azl::Composition composition = azl::defaultComposition();
            if (!generator)
                composition.layers[0].effects = {{1, type, params}};
            azl::EngineHarness harness({.width = 960, .height = 540}, 60.0, {}, composition);
            Client client(harness.document());
            if (c.clip)
                playMedia(client, clip);
            if (generator)
                apply(client, azl::SetLayerClip{1, azl::ClipRef{.name = type, .effects = {{1, type, params}}}});
            harness.step(40);
            return harness.readScreen();
        };
        const azl::Image skipped = render(c.type, c.noop);
        CHECK(skipped.rgba == render(full, c.noop).rgba);
        const azl::Image off = render(c.type, c.off);
        // Both do the same work here in programs compiled apart: the Intel Mac mini's compiler rounds an edge pixel
        // of Transform's rotation one level differently.
        CHECK(levelsApart(off, render(full, c.off)) <= 1);
        CHECK(off.rgba != skipped.rgba);
    }
}

namespace {

// Classic Shift Glitch's shader as it was when every pixel hashed the bands itself.
constexpr const char* kShiftGlitchHashing = R"(
    float period = 2.0 * (1.0 - pb[p + 0]);
    uint pattern = period > 0.0 ? uint(floor(max(h.beat, 0.0) / period)) : uint(max(h.time, 0.0) * 60.0);
    ivec2 count = pb[p + 3] > 0.0 ? ivec2(ceil(12.0 * vec2(pb[p + 2], pb[p + 1]) - 1e-4)) : ivec2(0);
    ivec2 top = ivec2(-1);
    vec2 from = px;
    for (int axis = 0; axis < 2; ++axis) {
        float dim = axis == 0 ? h.size.y : h.size.x, at = axis == 0 ? px.y : px.x;
        uint seed = pcg(pattern * 2u + uint(axis));
        for (int i = count[axis] - 1; i >= (axis == 0 ? 0 : max(top.x, 0)); --i) {
            uint n = pcg(seed ^ (uint(i + 1) * 0x9e3779b9u));
            float high = max(1.0, float(n & 0xffffu) / 65535.0 * 0.5 * pb[p + 3] * dim);
            float start = float(n >> 16u) / 65536.0 * dim;
            if (at < start || at >= start + high)
                continue;
            uint m = pcg(n);
            float run = float(m & 0xffffu) / 65535.0 * 0.11 * dim;
            from[1 - axis] = float(m >> 16u) / 65535.0 * (dim - run) + (at - start) * run / high;
            top[axis] = i;
            break;
        }
    }
    vec2 s = px;
    if (top.y >= 0 && top.y >= top.x)
        s.x = from.x;
    else if (top.x >= 0)
        s.y = from.y;
    return clamp(s, vec2(0.5), h.size - 0.5);
)";

// Classic Shift Glitch copied as `type`, with this shader and these derived values.
std::string registerShiftGlitch(const std::string& type, const char* metal,
                                void (*derive)(const float*, const float*, float, float, float*))
{
    if (azl::findEffect(type))
        return type;
    azl::EffectDef def = *azl::findEffect("classicShiftGlitch");
    def.type = type;
    def.params.pop_back();
    def.variant = true;
    if (metal) {
        def.glsl = metal;
        def.derived = 0;
    }
    def.derive = derive;
    azl::registerEffect(std::move(def));
    return type;
}

} // namespace

// Pictures at several frames, so several band patterns, on the reference show's base chain at 1080p and alone at 960x540.
// The copy whose CPU values never match the shader's pattern takes the shader's own hashing.
TEST_CASE("Classic Shift Glitch draws the same pictures from the CPU's band hashes as from its own")
{
    const std::string hashing = registerShiftGlitch("classicShiftGlitch_hashing", kShiftGlitchHashing, nullptr);
    const std::string unmatched = registerShiftGlitch("classicShiftGlitch_unmatched", nullptr,
                                                      [](const float*, const float*, float, float, float* out) { out[2] = 0.0f; });
    struct Case {
        const char* label;
        std::map<std::string, double> params;
        bool referenceShow = false;
        bool clip = false;
    };
    const Case cases[] = {
        {"Reference show base chain, defaults", {}, true, true},
        {"defaults", {}},
        {"all 1", {{"vertical", 1.0}, {"horizontal", 1.0}, {"size", 1.0}}, false, true},
        {"golden", {{"frequency", 0.5}, {"vertical", 0.6}, {"size", 0.7}}},
        {"slow, columns over rows", {{"frequency", 0.3}, {"vertical", 0.4}, {"horizontal", 1.0}, {"size", 0.2}}, false,
         true},
        {"size 0", {{"vertical", 1.0}, {"size", 0.0}}},
    };
    TempDir media;
    const fs::path clip = copyMedia("count_240.mp4", media.path / "clip.mp4");
    for (const Case& c : cases) {
        CAPTURE(c.label);
        auto render = [&](const std::string& type) {
            azl::Composition composition = azl::defaultComposition();
            if (c.referenceShow)
                composition.layers[0].effects = {{1, "colorize", {{"hue", 0.0074}, {"saturation", 0.8809}}}};
            composition.layers[0].effects.push_back({3, type, c.params});
            const uint32_t width = c.referenceShow ? 1920 : 960;
            azl::EngineHarness harness({.width = width, .height = width * 9 / 16}, 60.0, {}, composition);
            Client client(harness.document());
            if (c.clip)
                playMedia(client, clip);
            std::vector<azl::Image> frames;
            harness.step(30);
            for (int i = 0; i < 4; ++i) {
                harness.step(i == 0 ? 1 : 23);
                frames.push_back(harness.readScreen());
            }
            return frames;
        };
        const std::vector<azl::Image> reference = render(hashing);
        const std::vector<azl::Image> cached = render("classicShiftGlitch");
        const std::vector<azl::Image> fallback = render(unmatched);
        for (size_t i = 0; i < reference.size(); ++i) {
            CAPTURE(i);
            CHECK(cached[i].rgba == reference[i].rgba);
            CHECK(fallback[i].rgba == reference[i].rgba);
        }
        CHECK(reference.front().rgba != reference.back().rgba);
    }
}

namespace {

Message preset(Client& client, const std::string& op, const std::string& name = "", uint64_t effect = 0)
{
    Message m{{"type", "preset"}, {"op", op}};
    if (!name.empty())
        m.update({{"effect", effect}, {"name", name}});
    client.send(m);
    return client.next();
}

} // namespace

TEST_CASE("an effect's parameters save as a named preset, list, survive a restart and apply as one undoable edit")
{
    TempDir root;
    azl::Image saved;
    {
        azl::EngineHarness harness({.width = 960, .height = 540}, 60.0, root.path);
        Client client(harness.document());
        Client other(harness.document());
        const uint64_t colorize = effectId(addEffect(client, "colorize", {{"hue", 0.3}, {"brightness", 0.3}}));
        other.next();
        harness.step();
        saved = harness.readScreen();

        const Message reply = preset(client, "save", "Hot", colorize);
        CHECK(reply["type"] == "preset");
        CHECK(reply["effectType"] == "colorize");
        CHECK(reply["params"]["brightness"] == 0.3);
        CHECK(preset(client, "list")["presets"]["colorize"]["Hot"]["hue"] == 0.3);

        setParam(client, colorize, "brightness", 0.8);
        other.next();
        const Message applied = preset(client, "apply", "Hot", colorize);
        CHECK(applied["type"] == "change");
        CHECK(applied["op"] == "setEffectParams");
        CHECK(applied["params"]["brightness"] == 0.3);
        CHECK(other.next() == applied);
        harness.step();
        const uint8_t* want = pixel(saved, kYellowX, kYellowY);
        checkRgb(harness.readScreen(), kYellowX, kYellowY, want[0], want[1], want[2], 0);

        client.send(msg::undo());
        CHECK(client.next()["params"]["brightness"] == 0.8);
        client.send(msg::undo());
        CHECK(client.next()["value"] == 0.3);

        CHECK(preset(client, "apply", "Cold", colorize)["type"] == "error");
        CHECK(preset(client, "save", "Hot", 999)["type"] == "error");
        CHECK(preset(client, "save", "", colorize)["type"] == "error");
    }

    azl::EngineHarness harness({.width = 960, .height = 540}, 60.0, root.path);
    Client client(harness.document());
    CHECK(preset(client, "list")["presets"]["colorize"].contains("Hot"));
    const uint64_t colorize = effectId(addEffect(client, "colorize"));
    CHECK(preset(client, "apply", "Hot", colorize)["type"] == "change");
    harness.step();
    CHECK(harness.readScreen().rgba == saved.rgba);
}

TEST_CASE("an applied preset names itself on the effect; type defaults, chain presets and a merged file persist in the "
          "presets file")
{
    TempDir root;
    auto ask = [](Client& client, Message m) {
        m["type"] = "preset";
        client.send(m);
        return client.next();
    };
    {
        azl::EngineHarness harness({.width = 320, .height = 180}, 60.0, root.path);
        Client client(harness.document());
        const uint64_t colorize = effectId(addEffect(client, "colorize", {{"brightness", 0.3}}));
        preset(client, "save", "Hot", colorize);
        setParam(client, colorize, "brightness", 0.8);
        const Message applied = preset(client, "apply", "Hot", colorize);
        CHECK(applied["preset"] == "Hot");
        CHECK(azl::effectById(harness.document().document(), colorize)->preset == "Hot");
        client.send(msg::undo());
        CHECK(client.next()["preset"] == "");
        CHECK(azl::effectById(harness.document().document(), colorize)->preset.empty());

        CHECK(ask(client, {{"op", "default"}, {"effectType", "colorize"}, {"name", "Hot"}})["op"] == "default");
        CHECK(ask(client, {{"op", "default"}, {"effectType", "colorize"}, {"name", "Nope"}})["type"] == "error");
        const Message chain = Message::parse(R"({"op": "saveChain", "name": "Glow",
            "effects": [{"type": "colorize", "params": {"brightness": 0.5}}, {"type": "trails"}]})");
        CHECK(ask(client, chain)["op"] == "saveChain");
        CHECK(ask(client, Message::parse(R"({"op": "saveChain", "name": "Bad", "effects": [{"type": "nope"}]})"))["type"] ==
              "error");
        CHECK(ask(client, {{"op", "renameChain"}, {"name", "Glow"}, {"to", "Glow 2"}})["to"] == "Glow 2");
        const Message file = Message::parse(R"({"presets": {"trails": {"Long": {"feedback": 0.9}}},
            "chains": {"Tail": [{"type": "trails"}]}})");
        CHECK(ask(client, {{"op", "merge"}, {"file", file}})["op"] == "merge");
        CHECK(ask(client, {{"op", "merge"}, {"file", {{"nothing", 1}}}})["type"] == "error");
        CHECK(ask(client, {{"op", "deleteChain"}, {"name", "Tail"}})["op"] == "deleteChain");
        CHECK(ask(client, {{"op", "rename"}, {"effectType", "classicStripper"}, {"name", "Two-Way Bars"}, {"to", "X"}})
                  ["type"] == "error");
    }
    azl::EngineHarness harness({.width = 320, .height = 180}, 60.0, root.path);
    Client client(harness.document());
    const Message list = preset(client, "list");
    CHECK(list["defaults"]["colorize"] == "Hot");
    CHECK(list["chains"].contains("Glow 2"));
    CHECK_FALSE(list["chains"].contains("Tail"));
    CHECK(list["chains"]["Glow 2"].size() == 2);
    CHECK(list["presets"]["trails"]["Long"]["feedback"] == 0.9);
}

namespace {

void send(Client& client, const azl::Edit& edit)
{
    client.send(msg::command(edit));
    REQUIRE(client.next()["type"] == "change");
}

size_t differingPixels(const azl::Image& a, const azl::Image& b, int tolerance = 2)
{
    size_t count = 0;
    for (size_t i = 0; i < a.rgba.size(); i += 4)
        for (size_t c = 0; c < 3; ++c)
            if (std::abs(int(a.rgba[i + c]) - int(b.rgba[i + c])) > tolerance) {
                ++count;
                break;
            }
    return count;
}

// A colorized test card under a group holding a shrunk test card; `effects` adds two neutral Trails (dry/wet 0, and
// feedback 0; both still run their side passes) to the inner layer and to the group itself.
azl::Composition groupStack(bool effects)
{
    std::vector<azl::Effect> neutral;
    if (effects)
        neutral = {{0, "trails", {{"mix", 0.0}}}, {0, "trails", {{"feedback", 0.0}}}};
    azl::Layer inner{.id = 2, .name = "Inner", .opacity = 1.0};
    inner.effects = {{1, "transform", {{"scale", 0.5}, {"rotation", 15.0}}}};
    for (size_t i = 0; i < neutral.size(); ++i)
        inner.effects.push_back({10 + i, neutral[i].type, neutral[i].params});
    azl::Layer group{.id = 10, .name = "Group", .opacity = 0.8, .group = true, .layers = {inner}};
    for (size_t i = 0; i < neutral.size(); ++i)
        group.effects.push_back({20 + i, neutral[i].type, neutral[i].params});
    azl::Composition composition;
    composition.layers = {{.id = 1, .name = "Below", .opacity = 1.0, .effects = {{2, "colorize", {}}}}, group};
    return composition;
}

} // namespace

TEST_CASE("Trails combine correctly inside a group, on a group and with every blend mode")
{
    azl::EngineHarness plain({.width = 960, .height = 540}, 60.0, {}, groupStack(false));
    azl::EngineHarness withFx({.width = 960, .height = 540}, 60.0, {}, groupStack(true));
    Client a(plain.document());
    Client b(withFx.document());
    for (int mode = 0; mode < azl::kBlendModeCount; ++mode) {
        CAPTURE(mode);
        send(a, azl::SetLayerBlend{10, azl::BlendMode(mode)});
        send(b, azl::SetLayerBlend{10, azl::BlendMode(mode)});
        send(a, azl::SetLayerBlend{2, azl::BlendMode(mode)});
        send(b, azl::SetLayerBlend{2, azl::BlendMode(mode)});
        plain.step(2);
        withFx.step(2);
        CHECK(differingPixels(plain.readScreen(), withFx.readScreen()) == 0);
    }
    CHECK(withFx.stats().passes >= plain.stats().passes + 4);
}

TEST_CASE("Trails history restarts when its layer moves into or out of a group")
{
    azl::Composition composition;
    composition.layers = {{.id = 10, .name = "Group", .opacity = 1.0, .group = true},
                          {.id = 2,
                           .name = "Trailing",
                           .opacity = 1.0,
                           .effects = {{1, "transform", {}}, {2, "trails", {{"feedback", 1.0}}}}}};
    azl::EngineHarness harness({.width = 960, .height = 540}, 60.0, {}, composition);
    Client client(harness.document());
    // The card's white left edge stays in the history at x = 0 after the card moves away.
    harness.step();
    setParam(client, 1, "positionX", 0.5);
    harness.step(2);
    checkRgb(harness.readScreen(), 0, 270, 255, 255, 255);

    send(client, azl::MoveLayer{2, 10, 0});
    harness.step();
    checkRgb(harness.readScreen(), 0, 270, 0, 0, 0);

    setParam(client, 1, "positionX", 0.0);
    harness.step();
    setParam(client, 1, "positionX", 0.5);
    harness.step();
    checkRgb(harness.readScreen(), 0, 270, 255, 255, 255);
    send(client, azl::MoveLayer{2, 0, 1});
    harness.step();
    checkRgb(harness.readScreen(), 0, 270, 0, 0, 0);
}

TEST_CASE("an effect moved from a layer's chain into the composition's keeps its Trails history and phase")
{
    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    auto move = [&](uint64_t effect) { send(client, azl::MoveEffect{effect, 0, 0}); };
    // Above the test card's moving marker.
    auto strips = [&] {
        const azl::Image image = harness.readScreen();
        return std::vector<uint8_t>(image.rgba.begin(), image.rgba.begin() + 450 * 960 * 4);
    };
    auto near = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        for (size_t i = 0; i < a.size(); ++i)
            if (std::abs(int(a[i]) - int(b[i])) > 2)
                return false;
        return true;
    };

    const uint64_t stripper = effectId(addEffect(client, "stripper", {{"number", 6.0}, {"speed", 0.7}}));
    harness.step(13);
    setParam(client, stripper, "speed", 0.0);
    harness.step();
    const std::vector<uint8_t> frozen = strips();
    move(stripper);
    harness.step();
    CHECK(near(strips(), frozen));
    // A copy, with an id of its own, starts its phase over.
    const uint64_t copy = effectId(addEffect(client, "stripper", {{"number", 6.0}}, 0));
    send(client, azl::RemoveEffect{stripper});
    harness.step();
    CHECK_FALSE(near(strips(), frozen));
    send(client, azl::RemoveEffect{copy});

    // The card's white left edge stays in the history at x = 0 after the card moves away.
    const uint64_t transform = effectId(addEffect(client, "transform"));
    const uint64_t trails = effectId(addEffect(client, "trails", {{"feedback", 1.0}}));
    harness.step();
    setParam(client, transform, "positionX", 0.5);
    harness.step(2);
    checkRgb(harness.readScreen(), 0, 270, 255, 255, 255);
    move(trails);
    harness.step(2);
    checkRgb(harness.readScreen(), 0, 270, 255, 255, 255);
}

TEST_CASE("a bypassed effect shows the unaffected picture, skips its side passes and compiles nothing")
{
    azl::EngineHarness plain({.width = 960, .height = 540});
    plain.step(8);
    const azl::Image expected = plain.readScreen();

    for (const char* type : {"colorize", "edgeDetection", "classicTrails", "trails"}) {
        CAPTURE(type);
        azl::EngineHarness harness({.width = 960, .height = 540});
        Client client(harness.document());
        const uint64_t effect = effectId(addEffect(client, type));
        harness.step(4);
        const uint64_t passes = harness.stats().passes;
        const uint64_t pipelines = harness.compileStats().pipelines;
        CHECK(harness.readScreen().rgba != expected.rgba);

        client.send(msg::command(azl::SetEffectBypass{effect, true}));
        REQUIRE(client.next()["bypass"] == true);
        harness.step(4);
        CHECK(harness.readScreen().rgba == expected.rgba);
        CHECK(harness.stats().passes == passes - azl::findEffect(type)->sidePasses.size());
        CHECK(harness.compileStats().pipelines == pipelines);
        CHECK(harness.document().document().layers[0].effects[0].bypass);

        client.send(msg::undo());
        REQUIRE(client.next()["bypass"] == false);
        harness.step(4);
        CHECK(harness.stats().passes == passes);
        CHECK(harness.compileStats().pipelines == pipelines);
    }
}

TEST_CASE("Stripper compiles its livelier features only while one is on or driven")
{
    azl::Composition composition;
    composition.layers.push_back({.id = 1, .name = "Layer 1", .effects = {{1, "stripper", {{"speed", 1.0}}}}});
    const azl::Effect& stripper = composition.layers[0].effects[0];
    CHECK(azl::shaderType(composition, stripper) == "stripper");
    composition.macros = {{.id = 1, .targets = {{{0, 1, "jitter"}, {}, 0.0, 1.0}}}};
    CHECK(azl::shaderType(composition, stripper) == "stripper_live");
    composition.macros.clear();
    composition.layers[0].effects[0].params["movement"] = 2.0;
    CHECK(azl::shaderType(composition, stripper) == "stripper_live");
    composition.layers[0].effects[0].params["speedSpread"] = 0.3;
    CHECK(azl::shaderType(composition, stripper) == "stripper_cross");
    CHECK(azl::findEffect("stripper_cross")->params.size() == azl::findEffect("stripper")->params.size());
}

TEST_CASE("Stripper's reshuffle glides to new strips over the first half of each beat and holds them for the rest")
{
    azl::EngineHarness harness({.width = 320, .height = 180});
    Client client(harness.document());
    client.send(msg::command(azl::SetTempo{120.0}));
    client.next();
    addEffect(client, "stripper", {{"number", 10.0}, {"minWidth", 0.1}, {"reshuffle", 1.0}});
    // Above the test card's moving marker.
    auto picture = [&] {
        const azl::Image image = harness.readScreen();
        return std::vector<uint8_t>(image.rgba.begin(), image.rgba.begin() + 150 * 320 * 4);
    };
    harness.step();
    std::vector<uint8_t> previous = picture();
    auto gliding = [](double beat) { return beat >= 1.0 && beat - std::floor(beat) < 0.5; };
    double beat = harness.transport().beat;
    std::vector<uint8_t> firstBeat;
    for (int i = 0; i < 75; ++i) {
        harness.step();
        const std::vector<uint8_t> now = picture();
        CAPTURE(harness.transport().beat);
        if (!gliding(beat) && !gliding(harness.transport().beat))
            CHECK(now == previous);
        if (i == 25)
            firstBeat = now;
        previous = now;
        beat = harness.transport().beat;
    }
    CHECK(previous != firstBeat);
}

TEST_CASE("Stripper's audio band drives its width and strength")
{
    azl::EngineHarness harness({.width = 320, .height = 180});
    Client client(harness.document());
    azl::AudioAnalysis analysis([&] { return harness.time(); }, {.thread = false});
    harness.setAudio(&analysis);
    addEffect(client, "stripper", {{"width", 0.8}, {"audioBand", 0.0}, {"audioWidth", 1.0}, {"audioMix", 0.5}});
    auto brightness = [&] {
        const azl::Image image = harness.readScreen();
        double sum = 0.0;
        for (size_t i = 0; i < image.rgba.size(); i += 4)
            sum += image.rgba[i] + image.rgba[i + 1] + image.rgba[i + 2];
        return sum / double(image.rgba.size());
    };
    const double rate = 48000.0;
    std::vector<float> tone(size_t(rate / 60.0));
    double pushed = 0.0;
    auto run = [&](float amplitude, int frames) {
        for (int f = 0; f < frames; ++f) {
            for (size_t i = 0; i < tone.size(); ++i)
                tone[i] = amplitude * float(std::sin(2.0 * M_PI * 220.0 * (pushed + double(i)) / rate));
            analysis.push(tone.data(), tone.size(), pushed / rate, rate);
            pushed += double(tone.size());
            analysis.pump();
            harness.step();
        }
        return brightness();
    };
    const double loud = run(0.5, 60);
    const double silent = run(0.0, 60);
    CHECK(loud > silent * 1.3);
    harness.setAudio(nullptr);
}

TEST_CASE("bypass survives a snapshot round trip and wins over whatever drives dry/wet")
{
    azl::Effect effect{3, "colorize", {{"hue", 0.5}}, true};
    const Message j = effect;
    CHECK(j["bypass"] == true);
    CHECK(j.get<azl::Effect>() == effect);
    CHECK_FALSE(Message(azl::Effect{3, "colorize", {}}).contains("bypass"));

    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    const uint64_t colorize = effectId(addEffect(client, "colorize", {{"mix", 0.0}}));
    client.send(msg::command(azl::AddModulator{std::nullopt, {.target = {0, {0, colorize, "mix"}}, .min = 1.0, .max = 1.0}}));
    REQUIRE(client.next()["type"] == "change");
    harness.step(2);
    const azl::Image driven = harness.readScreen();
    client.send(msg::command(azl::SetEffectBypass{colorize, true}));
    client.next();
    harness.step(2);
    CHECK(harness.readScreen().rgba != driven.rgba);
    checkRgb(harness.readScreen(), kYellowX, kYellowY, 191, 191, 0);
    const azl::Composition copy = Message(harness.document().document()).get<azl::Composition>();
    CHECK(copy.layers[0].effects[0].bypass);
    CHECK(command(client, azl::SetEffectBypass{999, true})["type"] == "error");
}

// Information for tuning (run it on the Mac mini): what each effect of the soak show costs at 1080p on top of the
// test card, as the soak show configures it.
// The pass arguments a fused pass threads through its stages (the outgoing clip's planes and the mix during a
// transition among them) once shared a name with a stage's own locals, and the chain failed to compile only then.
TEST_CASE("every effect type and variant compiles first in a layer chain on the test card, a clip and during a "
          "transition, in a clip's and a group's chain, and after a generator")
{
    std::vector<std::string> types;
    for (const std::string& type : azl::effectTypes()) {
        // Other tests register effects of their own, some broken on purpose.
        if (type.starts_with("test"))
            continue;
        types.push_back(type);
        for (const azl::EffectVariant& variant : azl::findEffect(type)->variants)
            types.push_back(type + "_" + variant.name);
    }
    azl::ChainCompiler compiler(azl::gpu::Device::systemDefault());
    std::vector<std::string> failures;
    compiler.onFailure([&](const std::vector<std::string>& chain, const std::string& error) {
        failures.push_back(chain.front() + ": " + error.substr(0, 300));
    });
    const azl::Composition composition = azl::defaultComposition();
    auto compile = [&](azl::ChainKind kind, const std::vector<std::string>& chain) {
        std::vector<azl::Effect> effects;
        for (const std::string& type : chain)
            effects.push_back({effects.size() + 1, type, {}});
        const azl::EffectList list = azl::effectList(effects);
        compiler.prepare(composition, kind, list);
        compiler.waitIdle();
        return compiler.find(azl::chainKey(composition, kind, list));
    };
    size_t fades = 0;
    for (const std::string& type : types) {
        CAPTURE(type);
        const auto layer = compile(azl::ChainKind::Layer, {type, "edgeDetection"});
        CHECK(layer);
        CHECK((layer && !layer->testCard.empty()));
        fades += layer && !layer->yuvFade.empty();
        CHECK(compile(azl::ChainKind::Clip, {type, "edgeDetection"}));
        CHECK(compile(azl::ChainKind::Group, {type, "edgeDetection"}));
    }
    for (const std::string& generator : azl::generatorTypes())
        for (const std::string& type : types)
            if (type == "classicStripper" || type == "classicTrails")
                CHECK(compile(azl::ChainKind::Layer, {generator, type}));
    CHECK(fades == types.size());
    for (const std::string& failure : failures)
        MESSAGE(failure);
    CHECK(failures.empty());
}

TEST_CASE("GPU time per effect at 1080p")
{
    struct Case {
        const char* label;
        const char* type;
        std::map<std::string, double> params;
    };
    const Case cases[] = {
        {"transform", "transform", {{"scale", 1.08}, {"rotation", 4}}},
        {"colorize", "colorize", {{"hue", 0.6}, {"saturation", 0.7}}},
        {"colorize(brightness,invert,alpha)", "colorize",
         {{"hue", 0.6}, {"saturation", 0.7}, {"brightness", 0.8}, {"invert", 1}, {"alpha", 0.9}}},
        {"edgeDetection", "edgeDetection", {{"multiplier", 0.6}}},
        {"trails", "trails", {{"feedback", 0.85}}},
        {"stripper", "stripper", {{"number", 12}, {"speed", 0.5}, {"feather", 0.2}}},
        {"stripper(widths,add)", "stripper",
         {{"number", 12}, {"speed", 0.5}, {"feather", 0.2}, {"minWidth", 0.2}, {"maxWidth", 0.8}, {"add", 1}}},
        {"stripper(swing,jitter,reshuffle,audio)", "stripper",
         {{"number", 12}, {"speed", 0.5}, {"feather", 0.2}, {"movement", 1}, {"jitter", 0.5}, {"reshuffle", 1},
          {"audioWidth", 0.5}, {"audioMix", 0.5}}},
        {"stripper(spreads,12)", "stripper",
         {{"number", 12}, {"speed", 0.5}, {"feather", 0.2}, {"directionSpread", 0.3}, {"speedSpread", 0.5}}},
        {"stripper(spreads,64)", "stripper",
         {{"number", 64}, {"speed", 0.5}, {"feather", 0.2}, {"directionSpread", 0.3}, {"speedSpread", 0.5}}},
        {"reveal", "reveal", {{"progress", 0.6}, {"columns", 8}, {"rows", 8}, {"reshuffle", 2}}},
        {"classicStripper", "classicStripper", {{"number", 0.6}, {"speed", 0.02}, {"feather", 0.3}}},
        {"classicColorize", "classicColorize", {{"colorBlue", 1.0}, {"greysKeep", 1.0}}},
        {"classicShiftRgb", "classicShiftRgb", {{"mode", 2.0}, {"r", 0.2}, {"g", -0.1}, {"b", 0.05}}},
        {"classicMirror", "classicMirror", {{"x", 0.5}, {"y", 0.5}}},
        {"classicKaleidoscope", "classicKaleidoscope", {{"angles", 0.45}, {"rotation", 0.1}}},
        {"classicTwitch(all)", "classicTwitch",
         {{"maxPosX", 0.3}, {"posFrequency", 1.0}, {"maxScale", 0.3}, {"scaleFrequency", 1.0}, {"maxLight", 0.5},
          {"lightFrequency", 1.0}, {"maxRGBShift", 0.5}, {"rgbFrequency", 1.0}, {"maxBlur", 0.4},
          {"blurFrequency", 1.0}}},
        {"classicAutoMask", "classicAutoMask", {{"contrast", 0.8}}},
        {"classicBrightnessContrast", "classicBrightnessContrast", {{"brightness", 0.1}, {"contrast", 0.3}}},
        {"classicLevels", "classicLevels", {{"base", 0.45}, {"midpoint", 0.7}, {"ratio", 0.6}}},
        {"classicThreshold", "classicThreshold", {{"threshold", 0.4}, {"smoothness", 0.15}}},
        {"classicHueRotate", "classicHueRotate", {{"hueScale", 0.7}, {"hueRotate", 0.3}, {"satScale", 0.8}}},
        {"classicMirrorQuad", "classicMirrorQuad", {{"flipX", 1.0}}},
        {"classicPolarKaleido", "classicPolarKaleido", {{"rings", 3.0}, {"parts", 5.0}, {"angle", 0.1}}},
        {"classicTrails", "classicTrails", {}},
        {"classicTrails(lighten)", "classicTrails", {{"mode", 2.0}}},
        {"classicStrobe", "classicStrobe", {{"frequency", 1.0}}},
        {"classicShiftGlitch", "classicShiftGlitch", {}},
        {"classicShiftGlitch(all)", "classicShiftGlitch", {{"vertical", 1.0}, {"horizontal", 1.0}, {"size", 1.0}}},
        {"classicEdgeDetection", "classicEdgeDetection", {}},
        {"edgeDetection(multiplier 0)", "edgeDetection", {{"multiplier", 0.0}}},
        {"classicEdgeDetection(multiplier 0)", "classicEdgeDetection", {{"multiplier", 0.0}}},
        {"trails(feedback 0)", "trails", {{"feedback", 0.0}}},
        {"classicShiftRgb(defaults)", "classicShiftRgb", {}},
        {"classicShiftRgb(mode 2, g only)", "classicShiftRgb", {{"mode", 2.0}, {"g", 0.1}}},
        {"classicStripper(zoom)", "classicStripper", {{"number", 0.6}, {"mode", 0.5}, {"directionSpread", 0.2}}},
        {"classicStripper(zoom, directionSpread 0)", "classicStripper", {{"number", 0.6}, {"mode", 0.5}}},
        {"transform(rotation 0)", "transform", {{"scale", 1.08}}},
        {"classicColorize(colorAlpha 0)", "classicColorize", {{"colorAlpha", 0.0}}}};
    TempDir media;
    const fs::path clip = copyMedia("count_240.mp4", media.path / "clip.mp4");
    // On the test card, or on a playing clip (a video source fuses differently).
    auto measure = [&](const std::vector<azl::Effect>& effects, bool video) {
        azl::Composition composition = azl::defaultComposition();
        composition.layers[0].effects = effects;
        azl::EngineHarness harness({.width = 1920, .height = 1080}, 60.0, {}, composition);
        if (video) {
            Client client(harness.document());
            playMedia(client, clip);
        }
        harness.step(30);
        const uint64_t before = harness.stats().gpuMicros;
        harness.step(120);
        return double(harness.stats().gpuMicros - before) / 120 / 1000.0;
    };
    for (const bool video : {false, true}) {
        const double base = measure({}, video);
        std::string line = std::string("GPU ms/frame at 1080p: ") + (video ? "clip " : "test card ") +
                           std::to_string(base);
        for (const Case& c : cases)
            line += ", +" + std::string(c.label) + " " + std::to_string(measure({{1, c.type, c.params}}, video) - base);
        MESSAGE(line);
    }
}

// Not a pass/fail check: Classic Shift Glitch at the end of the reference show's base layer chain (Colorize) on a playing
// clip at 1080p, ms per frame over the chain without it, as the median and the least of interleaved runs, with
// the CPU's band hashes and with every pixel hashing them. Horizontal 0 has no band to look for, so it is the remap's
// fixed price; Mirror is another remap for comparison.
TEST_CASE("GPU time of Classic Shift Glitch on the reference show's base chain at 1080p")
{
    const std::string hashing = registerShiftGlitch("classicShiftGlitch_hashing", kShiftGlitchHashing, nullptr);
    const std::map<std::string, double> all{{"vertical", 1.0}, {"horizontal", 1.0}, {"size", 1.0}};
    struct Case {
        std::string label;
        std::string type;
        std::map<std::string, double> params;
    };
    const Case cases[] = {{"base", "", {}},
                          {"defaults", "classicShiftGlitch", {}},
                          {"defaults, hashing per pixel", hashing, {}},
                          {"all 1", "classicShiftGlitch", all},
                          {"all 1, hashing per pixel", hashing, all},
                          {"horizontal 0", "classicShiftGlitch", {{"horizontal", 0.0}}},
                          {"Mirror", "classicMirror", {{"x", 0.5}}}};
    TempDir media;
    const fs::path clip = copyMedia("count_240.mp4", media.path / "clip.mp4");
    auto measure = [&](const Case& c) {
        azl::Composition composition = azl::defaultComposition();
        composition.layers[0].effects = {{1, "colorize", {{"hue", 0.0074}, {"saturation", 0.8809}}}};
        if (!c.type.empty())
            composition.layers[0].effects.push_back({3, c.type, c.params});
        azl::EngineHarness harness({.width = 1920, .height = 1080}, 60.0, {}, composition);
        Client client(harness.document());
        playMedia(client, clip);
        harness.step(30);
        const uint64_t before = harness.stats().gpuMicros;
        harness.step(120);
        return double(harness.stats().gpuMicros - before) / 120 / 1000.0;
    };
    constexpr int kRounds = 9;
    std::vector<std::vector<double>> ms(std::size(cases));
    for (int round = 0; round < kRounds; ++round)
        for (size_t i = 0; i < std::size(cases); ++i)
            ms[i].push_back(measure(cases[i]));
    for (std::vector<double>& v : ms)
        std::sort(v.begin(), v.end());
    for (size_t i = 0; i < std::size(cases); ++i)
        MESSAGE("Shift Glitch on Colorize, clip 1080p, " << cases[i].label << ": median " << ms[i][kRounds / 2]
                                                                 << " ms (+" << ms[i][kRounds / 2] - ms[0][kRounds / 2]
                                                                 << "), least " << ms[i][0] << " (+"
                                                                 << ms[i][0] - ms[0][0] << ")");
}

namespace {

// The reference show, without its high layer and clip effects: three clip layers and a
// text layer, each through its chain, every layer above the first added.
azl::Composition gigLike()
{
    const std::map<std::string, double> strips{
        {"number", 0.8}, {"mode", 0.24285714285714285}, {"movementType", 0.4142857142857143},
        {"speed", 0.02857142857142857}, {"globalWidth", 0.07142857142857142}, {"minWidth", 0.18571428571428572},
        {"maxWidth", 0.7714285714285715}};
    const std::map<std::string, double> reveal{{"progress", 0.37}, {"gap", 0.05}, {"seed", 33.0}};
    azl::Composition composition;
    composition.layers.push_back({.id = 1, .name = "base", .effects = {{1, "reveal", reveal}}});
    composition.layers.push_back({.id = 2,
                                  .name = "low",
                                  .effects = {{2, "reveal", {{"progress", 1.0}, {"gap", 0.08}}, true},
                                              {3, "colorize", {{"hue", 0.0074}, {"saturation", 0.88}}, true},
                                              {4, "classicStripper", strips}},
                                  .blend = azl::BlendMode::Add});
    composition.layers.push_back({.id = 3,
                                  .name = "mid",
                                  .effects = {{5, "edgeDetection", {{"multiplier", 0.3}}},
                                              {6, "classicStripper", strips},
                                              {7, "classicColorize", {{"colorGreen", 1.0}, {"colorBlue", 1.0}}}},
                                  .blend = azl::BlendMode::Add});
    composition.layers.push_back(
        {.id = 5, .name = "text", .effects = {{8, "classicStripper", strips}}, .blend = azl::BlendMode::Add});
    return composition;
}

} // namespace

// With NOCTURNE_GIG_FRAMES=DIR it also writes frames 30, 60 and 90 there as PNG, to compare two builds' pictures; with
// NOCTURNE_BAKE_STATIC=1 it bakes static chain prefixes.
TEST_CASE("GPU time of a gig-like show at 1080p")
{
    TempDir media;
    const fs::path clip = copyMedia("count_240.mp4", media.path / "clip.mp4");
    const bool bake = std::getenv("NOCTURNE_BAKE_STATIC") != nullptr;
    azl::EngineHarness harness({.width = 1920, .height = 1080, .bakeStatic = bake}, 60.0, {}, gigLike());
    Client client(harness.document());
    const std::string hash = addMedia(client, clip)["entry"]["hash"];
    for (const uint64_t layer : {1, 2, 3})
        apply(client, azl::SetLayerClip{layer, azl::ClipRef{hash, "clip"}});
    const azl::TextClip text{.text = "HEADLINER", .font = "Futura Bold", .size = 0.18};
    apply(client, azl::SetLayerClip{5, azl::ClipRef{.name = "text", .text = text}});
    const char* dump = std::getenv("NOCTURNE_GIG_FRAMES");
    constexpr int kFrames = 480;
    harness.step(30);
    const uint64_t before = harness.stats().gpuMicros;
    uint64_t passes = 0, baked = 0;
    for (int frame = 30; frame < 30 + kFrames; ++frame) {
        if (dump && frame % 30 == 0 && frame <= 90)
            savePng(harness.readScreen(), (fs::path(dump) / ("gig-" + std::to_string(frame) + ".png")).string());
        harness.step();
        passes += harness.stats().passes;
        baked += harness.stats().bakedPasses;
    }
    MESSAGE("gig-like show GPU ms/frame at 1080p: " << double(harness.stats().gpuMicros - before) / kFrames / 1000.0
                                                    << ", passes " << double(passes) / kFrames << " per frame, "
                                                    << double(baked) / kFrames << " from bakes");
}
