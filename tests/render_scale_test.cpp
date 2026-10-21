#include "clips.hpp"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr azl::EngineConfig kSize{.width = 960, .height = 540};

// The test card on layer 1 (it has no clips) and a layer 2 for a text over it.
azl::Composition cardUnderText()
{
    return {.layers = {{.id = 1, .name = "card"}, {.id = 2, .name = "text"}}};
}

void playText(Client& client, uint64_t layer)
{
    apply(client, azl::SetSlot{layer, 1,
                               azl::ClipRef{.name = "Text", .text = azl::TextClip{.text = "SCALE", .size = 0.25,
                                                                                  .color = {1.0, 0.0, 0.0, 1.0}}}});
    client.send({{"type", "trigger"}, {"op", "clip"}, {"layer", layer}, {"column", 1}});
    REQUIRE(client.next()["op"] == "triggerClip");
}

// The pixels showing the text's full colour, which the card never shows.
std::vector<size_t> textPixels(const azl::Image& image)
{
    std::vector<size_t> at;
    for (size_t i = 0; i < image.rgba.size(); i += 4)
        if (image.rgba[i] == 255 && image.rgba[i + 1] == 0 && image.rgba[i + 2] == 0)
            at.push_back(i);
    return at;
}

double meanDifference(const azl::Image& a, const azl::Image& b)
{
    double sum = 0.0;
    for (size_t i = 0; i < a.rgba.size(); ++i)
        sum += std::abs(int(a.rgba[i]) - int(b.rgba[i]));
    return sum / double(a.rgba.size());
}

struct Shot {
    azl::Image screen;
    uint64_t passes;
};

Shot render(azl::EngineHarness& harness, int percent)
{
    harness.engine().setRenderScale(percent);
    harness.step(2);
    return {harness.readScreen(), harness.stats().passes};
}

} // namespace

TEST_CASE("render scale: the layers below a text render smaller and scale up once; the text stays at output size")
{
    azl::EngineHarness harness(kSize, 60.0, {}, cardUnderText());
    Client client(harness.document());
    playText(client, 2);
    const Shot full = render(harness, 100);
    const std::vector<size_t> text = textPixels(full.screen);
    REQUIRE(text.size() > 2000);

    for (int percent : {83, 67}) {
        CAPTURE(percent);
        const Shot scaled = render(harness, percent);
        CHECK(scaled.screen.width == full.screen.width);
        CHECK(scaled.passes == full.passes + 1);
        CHECK(scaled.screen.rgba != full.screen.rgba);
        CHECK(meanDifference(scaled.screen, full.screen) < 12.0);
        CHECK(textPixels(scaled.screen) == text);
    }
    CHECK(render(harness, 100).passes == full.passes);
}

TEST_CASE("render scale: a layer's published picture is the whole layer scaled up")
{
    azl::Composition c = cardUnderText();
    c.screens = {{.id = 1, .name = "Card", .slices = {{.id = 1, .input = 1}}}};
    azl::EngineHarness harness(kSize, 60.0, {}, c);
    Client client(harness.document());
    playText(client, 2);
    harness.step(2);
    const azl::Image full = harness.readScreen(1);
    harness.engine().setRenderScale(67);
    harness.step(2);
    const azl::Image scaled = harness.readScreen(1);
    CHECK(scaled.rgba != full.rgba);
    CHECK(meanDifference(scaled, full) < 12.0);
}

TEST_CASE("render scale: nothing scales below a text on the lowest layer, and a stack without text scales whole")
{
    azl::Composition textBelow = cardUnderText();
    std::swap(textBelow.layers[0], textBelow.layers[1]);
    azl::EngineHarness below(kSize, 60.0, {}, textBelow);
    Client client(below.document());
    playText(client, 2);
    const Shot full = render(below, 100);
    const Shot scaled = render(below, 67);
    CHECK(scaled.passes == full.passes);
    CHECK(textPixels(scaled.screen) == textPixels(full.screen));

    azl::Composition card = cardUnderText();
    card.layers.pop_back();
    card.effects = {{1, "colorize", {{"hue", 0.5}, {"saturation", 1.0}}}};
    azl::EngineHarness whole(kSize, 60.0, {}, card);
    const Shot wholeFull = render(whole, 100);
    const Shot wholeScaled = render(whole, 67);
    CHECK(wholeScaled.passes == wholeFull.passes + 1);
    CHECK(meanDifference(wholeScaled.screen, wholeFull.screen) < 12.0);
}

TEST_CASE("render scale: a layer kept at output size draws at it with those above it; set and undone by an edit")
{
    // Two test cards; the upper one covers the lower.
    azl::EngineHarness harness(kSize, 60.0, {}, {.layers = {{.id = 1, .name = "below"}, {.id = 2, .name = "kept"}}});
    Client client(harness.document());
    const Shot full = render(harness, 100);
    const Shot scaled = render(harness, 67);
    CHECK(scaled.screen.rgba != full.screen.rgba);

    CHECK(command(client, azl::SetLayerFullResolution{2, true})["op"] == "setLayerFullResolution");
    CHECK(harness.document().document().layers[1].fullResolution);
    const Shot kept = render(harness, 67);
    CHECK(kept.passes == full.passes + 1);
    // The card's bottom row holds a marker that moves with time.
    const size_t rows = size_t(kSize.width) * 4 * (kSize.height / 9 * 8);
    const azl::Image now = render(harness, 100).screen;
    CHECK(std::equal(kept.screen.rgba.begin(), kept.screen.rgba.begin() + rows, now.rgba.begin()));
    const nlohmann::json saved = harness.document().document().layers[1];
    CHECK(saved.get<azl::Layer>().fullResolution);

    client.send(msg::undo());
    CHECK(client.next()["op"] == "setLayerFullResolution");
    CHECK_FALSE(harness.document().document().layers[1].fullResolution);
    CHECK(render(harness, 67).passes == scaled.passes);
}

TEST_CASE("render scale: GPU time at 1080p of five layers under a text, 100 against 83 and 67 %")
{
    TempDir media;
    azl::EngineHarness harness({.width = 1920, .height = 1080});
    Client client(harness.document());
    const Message added = addMedia(client, copyMedia("count_240.mp4", media.path / "a.mp4"));
    REQUIRE(added["type"] == "library");
    const std::string clip = added["entry"]["hash"];
    const std::vector<std::vector<std::string>> chains{{},
                                                       {"colorize", "transform"},
                                                       {"colorize", "edgeDetection", "trails"},
                                                       {"classicStripper"},
                                                       {"colorize"}};
    for (uint64_t layer = 1; layer <= chains.size() + 1; ++layer) {
        if (layer > 1)
            apply(client, azl::AddLayer{0, std::nullopt, {.id = layer}});
        if (layer > chains.size())
            break;
        apply(client, azl::SetSlot{layer, 1, azl::ClipRef{clip, "clip", {}}});
        for (const std::string& type : chains[layer - 1])
            apply(client, azl::AddEffect{layer, std::nullopt, {0, type, {}}});
    }
    client.send({{"type", "trigger"}, {"op", "column"}, {"column", 1}});
    REQUIRE(client.next()["op"] == "triggerColumn");
    playText(client, chains.size() + 1);
    harness.step(120);
    const auto gpu = [&](int percent) {
        harness.engine().setRenderScale(percent);
        harness.step(30);
        const uint64_t before = harness.stats().gpuMicros;
        harness.step(120);
        return double(harness.stats().gpuMicros - before) / 120 / 1000.0;
    };
    std::vector<double> d83, d67;
    std::ostringstream rounds;
    for (int round = 0; round < 3; ++round) {
        const double full = gpu(100), five = gpu(83), full2 = gpu(100), two = gpu(67);
        d83.push_back(five - full);
        d67.push_back(two - full2);
        rounds << " " << full << "/" << five << ", " << full2 << "/" << two << ";";
    }
    std::sort(d83.begin(), d83.end());
    std::sort(d67.begin(), d67.end());
    MESSAGE("GPU ms/frame at 1080p, 5 layers and a text, 100/83 and 100/67 by round:" << rounds.str()
                                                                                     << " median 83 % " << d83[1]
                                                                                     << " ms, 67 % " << d67[1] << " ms");
}

TEST_CASE("render scale: under a composition chain the text stays at output size inside it")
{
    if (!azl::findEffect("testPassThrough"))
        azl::registerEffect({"testPassThrough", azl::StageKind::Pointwise, {}, false, "\n    return c;\n", {}});
    azl::Composition c = cardUnderText();
    c.effects = {{1, "testPassThrough", {}}};
    azl::EngineHarness harness(kSize, 60.0, {}, c);
    Client client(harness.document());
    playText(client, 2);
    const Shot full = render(harness, 100);
    const Shot scaled = render(harness, 67);
    CHECK(scaled.passes == full.passes + 1);
    const std::vector<size_t> text = textPixels(full.screen);
    CHECK(text.size() > 2000);
    CHECK(textPixels(scaled.screen) == text);
}
