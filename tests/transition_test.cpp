#include "clips.hpp"
#include "golden.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

using azl::AutopilotMode;
using azl::ClipRef;
using azl::TransitionMode;

// At 120 BPM and 60 fps a beat is 30 render frames and a bar 120. count_240 is 16 beats long, so it shows clip frame
// n / 2 at render frame n, and frames 128..239 have their top bit set; count_709_tv shows n / 2 from its start.
constexpr azl::EngineConfig kSize{.width = 960, .height = 540};
// The top bit's bar of the bar code.
constexpr uint32_t kTopBitX = 60;

Message edit(Client& client, const azl::Edit& e)
{
    client.send(msg::command(e));
    return client.next();
}

std::string add(Client& client, const fs::path& path)
{
    const Message added = addMedia(client, path);
    REQUIRE(added["type"] == "library");
    return added["entry"]["hash"];
}

void setSlot(Client& client, uint64_t layer, uint64_t column, const std::string& hash)
{
    REQUIRE(edit(client, azl::SetSlot{layer, column, ClipRef{hash, "clip", {}}})["op"] == "setSlot");
}

void transition(Client& client, TransitionMode type, double beats, uint64_t layer = 1)
{
    REQUIRE(edit(client, azl::SetLayerTransition{layer, type, beats})["op"] == "setLayerTransition");
}

void triggerColumn(Client& client, uint64_t column)
{
    client.send({{"type", "trigger"}, {"op", "column"}, {"column", column}});
    REQUIRE(client.next()["op"] == "triggerColumn");
}

// Preloads a cell that is neither in the active nor the next column, for a trigger that lands at once.
void arm(Client& client, uint64_t layer, uint64_t column)
{
    client.send({{"type", "trigger"}, {"op", "arm"}, {"layer", layer}, {"column", column}, {"armed", true}});
    REQUIRE(client.next()["op"] == "armClip");
}

void triggerClip(Client& client, uint64_t layer, uint64_t column)
{
    client.send({{"type", "trigger"}, {"op", "clip"}, {"layer", layer}, {"column", column}});
    REQUIRE(client.next()["op"] == "triggerClip");
}

int topBit(azl::EngineHarness& harness)
{
    return pixel(harness.readScreen(), kTopBitX, kBitRow)[0];
}

// The bar code's bits from `first` (0 is the top bit) on, as a number.
int lowBits(const azl::Image& image, uint32_t first)
{
    int value = 0;
    for (uint32_t bit = first; bit < 8; ++bit)
        value = value * 2 + (pixel(image, kBitWidth / 2 + bit * kBitWidth, kBitRow)[0] > 128);
    return value;
}

// Layer 1 plays count_240 from column 1 from frame 0; column 2 holds count_709_tv. A trigger given at frame 300 lands
// on beat 12, frame 360, where count_240 is at clip frame 180.
struct Grid {
    explicit Grid(azl::EngineConfig config = kSize) : harness(config), client(harness.document())
    {
        setSlot(client, 1, 1, add(client, copyMedia("count_240.mp4", media.path / "a.mp4")));
        setSlot(client, 1, 2, add(client, copyMedia("count_709_tv.mp4", media.path / "b.mp4")));
        triggerColumn(client, 1);
        harness.step(300);
    }

    TempDir media;
    azl::EngineHarness harness;
    Client client;
};

const char* typeName(TransitionMode type)
{
    const char* names[] = {"alpha", "wipe", "dissolve", "push", "windowSlice", "radial", "randomSquares"};
    return names[int(type)];
}

} // namespace

TEST_CASE("golden frames halfway through each transition type, the ISF ones too, with no pipeline compiled for it")
{
    for (const TransitionMode type :
         {TransitionMode::Alpha, TransitionMode::Wipe, TransitionMode::Dissolve, TransitionMode::Push,
          TransitionMode::WindowSlice, TransitionMode::Radial, TransitionMode::RandomSquares}) {
        CAPTURE(typeName(type));
        Grid grid;
        transition(grid.client, type, 2.0);
        const uint64_t pipelines = grid.harness.compileStats().pipelines;
        triggerColumn(grid.client, 2);
        // Frame 390 is beat 13: count_240 at clip frame 195, count_709_tv at 15.
        grid.harness.step(91);
        checkGolden(grid.harness.readScreen(), std::string("transition_") + typeName(type));
        grid.harness.step(30);
        CHECK(shownFrame(grid.harness.readScreen()) == 30);
        CHECK(grid.harness.compileStats().pipelines == pipelines);
        CHECK(grid.harness.stats().videoMisses == 0);
    }
}

TEST_CASE("an alpha transition weighs the two clips in 1/256 steps")
{
    Grid grid;
    transition(grid.client, TransitionMode::Alpha, 4.0);
    triggerColumn(grid.client, 2);
    grid.harness.step(60);
    CHECK(topBit(grid.harness) > 250);
    // A quarter of the way, then halfway: the outgoing clip's set bit at 3/4 and 1/2.
    grid.harness.step(31);
    CHECK(std::abs(topBit(grid.harness) - 191) <= 3);
    grid.harness.step(30);
    CHECK(std::abs(topBit(grid.harness) - 128) <= 3);
    CHECK(grid.harness.stats().videoMisses == 0);
}

TEST_CASE("the outgoing clip plays on during a transition")
{
    Grid grid;
    transition(grid.client, TransitionMode::Wipe, 4.0);
    triggerColumn(grid.client, 2);
    // Until halfway the four low bits, right of the wipe, still show count_240.
    grid.harness.step(91);
    for (int n = 390; n < 420; ++n) {
        CAPTURE(n);
        REQUIRE(lowBits(grid.harness.readScreen(), 4) == n / 2 % 16);
        grid.harness.step();
    }
    CHECK(grid.harness.stats().videoMisses == 0);
}

TEST_CASE("transition length is in beats: a tempo change halfway stretches the rest")
{
    Grid grid;
    transition(grid.client, TransitionMode::Alpha, 2.0);
    triggerColumn(grid.client, 2);
    grid.harness.step(91);
    CHECK(std::abs(topBit(grid.harness) - 128) <= 3);
    REQUIRE(edit(grid.client, azl::SetTempo{60})["op"] == "setTempo");
    // At 60 BPM a beat is 60 frames: 30 frames later a quarter of the transition is left, 60 frames later none.
    grid.harness.step(30);
    CHECK(std::abs(topBit(grid.harness) - 64) <= 3);
    grid.harness.step(30);
    CHECK(topBit(grid.harness) < 4);
    CHECK(grid.harness.stats().videoMisses == 0);
}

TEST_CASE("a clip's own transition length overrides the layer's; 0 cuts")
{
    Grid grid;
    transition(grid.client, TransitionMode::Alpha, 4.0);
    CHECK(edit(grid.client, azl::SetClipTransition{1, 2, 0.0})["beats"] == 0.0);
    triggerColumn(grid.client, 2);
    grid.harness.step(61);
    CHECK(shownFrame(grid.harness.readScreen()) == 0);

    CHECK(edit(grid.client, azl::SetClipTransition{1, 1, 2.0})["beats"] == 2.0);
    arm(grid.client, 1, 1);
    grid.harness.step(59);
    REQUIRE(edit(grid.client, azl::SetQuantise{0})["op"] == "setQuantise");
    triggerColumn(grid.client, 1);
    // A beat later count_709_tv shows frame 45 (bit value 32 set) and count_240 frame 15 (not set), halfway mixed.
    grid.harness.step(31);
    CHECK(std::abs(pixel(grid.harness.readScreen(), 2 * kBitWidth + kBitWidth / 2, kBitRow)[0] - 128) <= 3);
    CHECK(grid.harness.stats().videoMisses == 0);
}

TEST_CASE("Autopilot steps through every transition type with no video miss")
{
    TempDir media;
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    const char* files[] = {"count_100.mp4", "count_709_tv.mp4", "count_601_pc.mp4"};
    for (uint64_t column = 1; column <= 3; ++column)
        setSlot(client, 1, column, add(client, copyMedia(files[column - 1], media.path / files[column - 1])));
    REQUIRE(edit(client, azl::SetLayerAutopilot{1, AutopilotMode::Next, 1})["op"] == "setLayerAutopilot");
    triggerColumn(client, 1);
    harness.step(120);
    for (const TransitionMode type :
         {TransitionMode::Alpha, TransitionMode::Wipe, TransitionMode::Dissolve, TransitionMode::Push,
          TransitionMode::WindowSlice, TransitionMode::Radial, TransitionMode::RandomSquares}) {
        CAPTURE(typeName(type));
        transition(client, type, 1.0);
        for (int bar = 0; bar < 3; ++bar) {
            // A beat into the bar the transition is over and the new clip shows a beat's worth of frames: 15, or 12
            // for count_100 (100 frames over 8 beats) in column 1.
            harness.step(31);
            CHECK(shownFrame(harness.readScreen()) == (bar == 2 ? 12 : 15));
            harness.step(89);
        }
    }
    CHECK(harness.stats().videoMisses == 0);
}

TEST_CASE("a manual trigger during a transition starts a new one from the incoming clip")
{
    Grid grid;
    REQUIRE(edit(grid.client, azl::SetQuantise{0})["op"] == "setQuantise");
    setSlot(grid.client, 1, 3, add(grid.client, copyMedia("count_601_pc.mp4", grid.media.path / "c.mp4")));
    transition(grid.client, TransitionMode::Wipe, 2.0);
    arm(grid.client, 1, 3);
    grid.harness.step();
    triggerClip(grid.client, 1, 2);
    grid.harness.step(30);
    triggerClip(grid.client, 1, 3);
    grid.harness.step(61);
    CHECK(shownFrame(grid.harness.readScreen()) == 30);
    CHECK(grid.harness.stats().videoMisses == 0);
}

TEST_CASE("transition settings are undoable edits on layers and clips, checked on the way in")
{
    TempDir media;
    azl::Composition withGroup = azl::defaultComposition();
    withGroup.layers.push_back({.id = 2, .name = "Group", .group = true});
    azl::EngineHarness harness(kSize, 60.0, {}, withGroup);
    Client client(harness.document());
    CHECK(client.snapshot["composition"]["layers"][0]["transition"] == Message{{"mode", "alpha"}, {"beats", 0.0}});

    const Message wipe = edit(client, azl::SetLayerTransition{1, TransitionMode::Wipe, std::nullopt});
    CHECK(wipe["mode"] == "wipe");
    CHECK_FALSE(wipe.contains("beats"));
    CHECK(edit(client, azl::SetLayerTransition{1, std::nullopt, 0.5})["beats"] == 0.5);
    Client late(harness.document());
    CHECK(late.snapshot["composition"]["layers"][0]["transition"] == Message{{"mode", "wipe"}, {"beats", 0.5}});
    client.send(msg::undo());
    CHECK(client.next()["beats"] == 0.0);

    for (const double beats : {-1.0, 64.5})
        CHECK(edit(client, azl::SetLayerTransition{1, std::nullopt, beats})["type"] == "error");
    CHECK(edit(client, azl::SetLayerTransition{2, TransitionMode::Push, 1.0})["type"] == "error");
    client.send({{"type", "command"}, {"op", "setLayerTransition"}, {"layer", 1}, {"mode", "spin"}});
    CHECK(client.next()["type"] == "error");

    const std::string a = add(client, copyMedia("count_709_tv.mp4", media.path / "a.mp4"));
    CHECK(edit(client, azl::SetClipTransition{1, 1, 2.0})["type"] == "error");
    setSlot(client, 1, 1, a);
    CHECK(edit(client, azl::SetClipTransition{1, 1, 65.0})["type"] == "error");
    CHECK(edit(client, azl::SetClipTransition{1, 1, 2.0})["beats"] == 2.0);
    Client later(harness.document());
    CHECK(later.snapshot["composition"]["layers"][0]["clips"]["1"]["transition"] == 2.0);
    client.send(msg::undo());
    CHECK(client.next()["beats"].is_null());
    Client last(harness.document());
    CHECK(last.snapshot["composition"]["layers"][0]["clips"]["1"]["transition"].is_null());
}

TEST_CASE("the debug frame bits strip shows the render frame index on every output, and nothing when off")
{
    TempDir media;
    azl::EngineConfig config = kSize;
    config.debugFrameBits = true;
    azl::EngineHarness harness(config);
    Client client(harness.document());
    playMedia(client, copyMedia("count_709_tv.mp4", media.path / "a.mp4"));
    const auto stripIndex = [&] {
        const azl::Image image = harness.readScreen();
        int index = 0;
        for (uint32_t bit = 0; bit < 16; ++bit) {
            const uint8_t value = pixel(image, bit * 60 + 30, 535)[0];
            CHECK_MESSAGE((value < 50 || value > 250), "bit " << bit << " is " << int(value));
            index = index * 2 + (value > 128);
        }
        return index;
    };
    for (int frame = 0; frame < 300; ++frame) {
        harness.step();
        REQUIRE(stripIndex() == frame);
    }
    // The clip's own picture is untouched above the strip.
    CHECK(shownFrame(harness.readScreen()) == 29);

    azl::EngineHarness plain(kSize);
    Client other(plain.document());
    playMedia(other, copyMedia("count_709_tv.mp4", media.path / "b.mp4"));
    plain.step(2);
    harness.step(2);
    for (const uint32_t x : {30u, 930u})
        CHECK(std::abs(pixel(plain.readScreen(), x, 535)[1] - pixel(harness.readScreen(), x, 520)[1]) <= 2);
}

// Not a pass/fail check: prints the GPU time per frame of 5 layers with a Transform + Colorize chain at 1080p, steady,
// while one layer transitions and while all five do.
TEST_CASE("GPU time of transitions")
{
    TempDir media;
    azl::EngineHarness harness({.width = 1920, .height = 1080});
    Client client(harness.document());
    const std::string a = add(client, copyMedia("count_240.mp4", media.path / "a.mp4"));
    const std::string b = add(client, copyMedia("count_100.mp4", media.path / "b.mp4"));
    for (uint64_t layer = 1; layer <= 5; ++layer) {
        if (layer > 1)
            REQUIRE(edit(client, azl::AddLayer{0, std::nullopt, {.id = layer}})["op"] == "addLayer");
        for (const std::string type : {"transform", "colorize"})
            REQUIRE(edit(client, azl::AddEffect{layer, std::nullopt, {0, type, {}}})["op"] == "addEffect");
        setSlot(client, layer, 1, a);
        setSlot(client, layer, 2, b);
        transition(client, TransitionMode::Alpha, 2.0, layer);
    }
    REQUIRE(edit(client, azl::SetQuantise{0})["op"] == "setQuantise");
    triggerColumn(client, 1);
    harness.step(30);

    const auto measure = [&](int frames) {
        const uint64_t before = harness.stats().gpuMicros;
        harness.step(frames);
        return double(harness.stats().gpuMicros - before) / frames / 1000.0;
    };
    const double steady = measure(60);
    triggerClip(client, 1, 2);
    const double one = measure(59);
    harness.step(30);
    triggerColumn(client, 1);
    const double all = measure(59);
    MESSAGE("GPU ms/frame at 1080p, 5 layers: steady " << steady << ", one layer in transition " << one << " (+"
                                                       << one - steady << "), all five " << all << " (+"
                                                       << all - steady << ")");
    CHECK(harness.stats().videoMisses == 0);
}

// Not a pass/fail check: the GPU time per frame of the reference show's chains at 1080p (base: Colorize on each clip; low:
// Colorize; mid: Colorize, Edge Detection, Trails; high: Stripper; text: Stripper, all on the layers), steady and while all five layers transition, at full resolution and with the frame budget guard drawing
// every layer at half resolution.
TEST_CASE("GPU time of transitions on the reference show's chains")
{
    TempDir media;
    azl::EngineHarness harness({.width = 1920, .height = 1080});
    Client client(harness.document());
    const std::string a = add(client, copyMedia("count_240.mp4", media.path / "a.mp4"));
    const std::string b = add(client, copyMedia("count_100.mp4", media.path / "b.mp4"));
    const std::vector<std::vector<std::string>> chains{
        {}, {"colorize"}, {"colorize", "edgeDetection", "trails"}, {"stripper"}, {"stripper"}};
    for (uint64_t layer = 1; layer <= chains.size(); ++layer) {
        if (layer > 1)
            REQUIRE(edit(client, azl::AddLayer{0, std::nullopt, {.id = layer}})["op"] == "addLayer");
        setSlot(client, layer, 1, a);
        setSlot(client, layer, 2, b);
        for (const std::string& type : chains[layer - 1])
            REQUIRE(edit(client, azl::AddEffect{layer, std::nullopt, {0, type, {}}})["op"] == "addEffect");
        transition(client, TransitionMode::Alpha, 2.0, layer);
    }
    for (uint64_t column : {1, 2})
        REQUIRE(edit(client, azl::AddEffect{1, std::nullopt, {0, "colorize", {}}, column})["op"] == "addEffect");
    REQUIRE(edit(client, azl::SetQuantise{0})["op"] == "setQuantise");
    triggerColumn(client, 1);
    harness.step(30);

    const auto measure = [&](int frames) {
        const uint64_t before = harness.stats().gpuMicros;
        harness.step(frames);
        return double(harness.stats().gpuMicros - before) / frames / 1000.0;
    };
    const double steady = measure(60);
    const uint64_t steadyPasses = harness.stats().passes;
    triggerColumn(client, 2);
    harness.step(1);
    const uint64_t fadePasses = harness.stats().passes;
    const double all = measure(58);
    harness.engine().setFrameBudget(1e-6);
    harness.step(90);
    const uint64_t halved = harness.stats().budgetLevel;
    const double steadyHalf = measure(30);
    triggerColumn(client, 1);
    const double allHalf = measure(59);
    MESSAGE("GPU ms/frame at 1080p, the reference show's chains: steady " << steady << " (" << steadyPasses
                                                                     << " passes), all five in transition " << all
                                                                     << " (" << fadePasses << " passes); with "
                                                                     << halved << " layers at half resolution "
                                                                     << steadyHalf << " and " << allHalf);
    CHECK(harness.stats().videoMisses == 0);
}

// Colorize and a Trails with no feedback are linear in the picture, so the layer's chain on the mixed clips is the mix
// of the chained clips: with the chain only the layer's, it runs once at the mix and adds no pass; with a clip effect
// on each clip (at mix 0, so the picture is the same) each clip runs its own chain.
TEST_CASE("an alpha transition runs a chain only the layer's once, on the mixed clips")
{
    const auto run = [](bool clipEffects, uint64_t& steady, uint64_t& fading) {
        Grid grid;
        REQUIRE(edit(grid.client, azl::AddEffect{1, std::nullopt, {0, "colorize", {{"hue", 0.3}}}})["op"] ==
                "addEffect");
        REQUIRE(edit(grid.client, azl::AddEffect{1, std::nullopt, {0, "trails", {{"feedback", 0.0}}}})["op"] ==
                "addEffect");
        for (uint64_t column : {1, 2})
            if (clipEffects)
                REQUIRE(edit(grid.client, azl::AddEffect{1, std::nullopt, {0, "colorize", {{"mix", 0.0}}}, column})
                            ["op"] == "addEffect");
        transition(grid.client, TransitionMode::Alpha, 2.0);
        grid.harness.step(1);
        steady = grid.harness.stats().passes;
        triggerColumn(grid.client, 2);
        grid.harness.step(90);
        fading = grid.harness.stats().passes;
        CHECK(grid.harness.stats().videoMisses == 0);
        return grid.harness.readScreen();
    };
    uint64_t steady = 0, fading = 0, steadyPerClip = 0, fadingPerClip = 0;
    const azl::Image once = run(false, steady, fading);
    const azl::Image perClip = run(true, steadyPerClip, fadingPerClip);
    CHECK(fading == steady);
    CHECK(fadingPerClip > steadyPerClip);
    int worst = 0;
    for (size_t i = 0; i < once.rgba.size(); ++i)
        worst = std::max(worst, std::abs(int(once.rgba[i]) - int(perClip.rgba[i])));
    CHECK(worst <= 2);
}

// The guard runs against a budget no frame meets, then one every frame meets, beside an engine without it.
TEST_CASE("the frame budget guard draws layers at half resolution while frames overrun, then full again")
{
    Grid guarded;
    Grid reference;
    const auto difference = [&] {
        const azl::Image a = guarded.harness.readScreen();
        const azl::Image b = reference.harness.readScreen();
        double sum = 0.0;
        for (size_t i = 0; i < a.rgba.size(); ++i)
            sum += std::abs(int(a.rgba[i]) - int(b.rgba[i]));
        return sum / double(a.rgba.size());
    };
    const auto step = [&](int frames) {
        guarded.harness.step(frames);
        reference.harness.step(frames);
    };
    guarded.harness.engine().setFrameBudget(1e-6);
    step(60);
    CHECK(guarded.harness.stats().budgetLevel == 1);
    CHECK(guarded.harness.stats().budgetChanges == 1);
    const double reduced = difference();
    CHECK(reduced > 0.0);
    CHECK(reduced < 2.0);
    guarded.harness.engine().setFrameBudget(1.0);
    step(120);
    CHECK(guarded.harness.stats().budgetLevel == 0);
    CHECK(guarded.harness.stats().budgetChanges == 2);
    CHECK(difference() == 0.0);
}

TEST_CASE("the frame guard setting turns the guard off, undoes, and is saved only when off")
{
    Grid grid;
    CHECK_FALSE(grid.client.snapshot["composition"].contains("frameGuard"));
    grid.harness.engine().setFrameBudget(1e-6);
    grid.harness.step(60);
    CHECK(grid.harness.stats().budgetLevel == 1);

    CHECK(edit(grid.client, azl::SetFrameGuard{false})["on"] == false);
    CHECK(Client(grid.harness.document()).snapshot["composition"]["frameGuard"] == false);
    grid.harness.step(120);
    CHECK(grid.harness.stats().budgetLevel == 0);

    grid.client.send(msg::undo());
    CHECK(grid.client.next()["on"] == true);
    CHECK_FALSE(Client(grid.harness.document()).snapshot["composition"].contains("frameGuard"));
    grid.harness.step(60);
    CHECK(grid.harness.stats().budgetLevel == 1);
}

// bench/run.sh's typical scenario: five layers, two of them in a group, two or three effects each, four columns of
// clips with Random Jump on the bottom layer, a column trigger every bar.
TEST_CASE("switching columns over the benchmark's typical composition compiles nothing after warm-up")
{
    TempDir media;
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    const std::vector<std::string> clips{
        add(client, copyMedia("count_709_tv.mp4", media.path / "a.mp4")),
        add(client, copyMedia("count_100.mp4", media.path / "b.mp4")),
        add(client, copyMedia("count_240_gop240.mp4", media.path / "c.mp4")),
        add(client, copyMedia("count_601_pc.mp4", media.path / "d.mp4"))};
    const std::string bottom = add(client, copyMedia("count_240.mp4", media.path / "e.mp4"));
    apply(client, azl::SetTempo{128.0});
    apply(client, azl::SetQuantise{4});
    for (int i = 0; i < 4; ++i)
        apply(client, azl::AddLayer{0, std::nullopt, {}});
    apply(client, azl::AddLayer{0, std::nullopt, {.group = true}});
    apply(client, azl::MoveLayer{4, 6, 0});
    apply(client, azl::MoveLayer{5, 6, 1});
    for (uint64_t column = 1; column <= 4; ++column) {
        setSlot(client, 1, column, bottom);
        for (uint64_t layer = 2; layer <= 5; ++layer)
            setSlot(client, layer, column, clips[(layer + column) % clips.size()]);
        apply(client, azl::SetClipTransport{1, std::nullopt, azl::PlayMode::RandomJump, std::nullopt, 8.0, 2.0, column});
    }
    const std::vector<std::pair<std::string, std::map<std::string, double>>> effects{
        {"transform", {{"scale", 1.08}, {"rotation", 4.0}}},
        {"colorize", {{"hue", 0.6}, {"saturation", 0.7}}},
        {"edgeDetection", {{"multiplier", 0.6}}},
        {"classicTrails", {{"feedback", 0.8}}},
        {"trails", {{"feedback", 0.85}}},
        {"stripper", {{"number", 12.0}, {"speed", 0.5}, {"feather", 0.2}}}};
    const std::vector<std::pair<uint64_t, std::vector<size_t>>> chains{
        {1, {0, 1}}, {2, {2, 1, 0}}, {3, {3, 0}}, {4, {4, 1}}, {5, {5, 0, 1}}};
    for (const auto& [layer, indices] : chains)
        for (const size_t i : indices)
            apply(client, azl::AddEffect{layer, std::nullopt, {0, effects[i].first, effects[i].second}});
    apply(client, azl::SetLayerBlend{2, azl::BlendMode::Screen});
    apply(client, azl::SetLayerBlend{3, azl::BlendMode::Add});
    apply(client, azl::SetLayerBlend{6, azl::BlendMode::Screen});
    apply(client, azl::SetLayerOpacity{5, 0.8});
    triggerColumn(client, 1);
    harness.step(60);

    const uint64_t pipelines = harness.compileStats().pipelines;
    CHECK(pipelines > 0);
    for (uint64_t column : {2, 3, 4, 1, 2, 3, 4, 1}) {
        triggerColumn(client, column);
        harness.step(120);
    }
    CHECK(harness.compileStats().pipelines == pipelines);
    CHECK(harness.stats().skipped == 0);
}

namespace {

using EffectSpecs = std::vector<std::pair<std::string, std::map<std::string, double>>>;

// Where a layer's chain runs during a transition: on the layer, on a group around the layer (which runs it once on
// the mixed clips by definition), or on each clip.
enum class ChainOn { Layer, Group, Clips };

struct ChainRun {
    std::vector<azl::Image> frames;
    uint64_t steadyPasses = 0;
    uint64_t fadePasses = 0;
};

// Layer 1 plays count_240, then from frame 150 count_709_tv after a two-beat transition (60 frames): a frame every 6
// from just before the switch to just after the transition. From there on the layer draws its chain fused with its
// clip's again, which a group draws from an 8-bit sub-stack, so their Classic Trails drift apart over the next seconds.
ChainRun runChain(ChainOn on, TransitionMode type, bool clipEffects, const EffectSpecs& chain)
{
    azl::Composition composition{{{.id = 1, .name = "Clips"}}};
    if (on == ChainOn::Group)
        composition = azl::Composition{{{.id = 10, .name = "Group", .group = true, .layers = {{.id = 1, .name = "Clips"}}}}};
    TempDir media;
    azl::EngineHarness harness({.width = 320, .height = 180}, 60.0, {}, composition);
    Client client(harness.document());
    setSlot(client, 1, 1, add(client, copyMedia("count_240.mp4", media.path / "a.mp4")));
    setSlot(client, 1, 2, add(client, copyMedia("count_709_tv.mp4", media.path / "b.mp4")));
    auto addEffect = [&](uint64_t layer, const std::string& type, const std::map<std::string, double>& params,
                         std::optional<uint64_t> column) {
        REQUIRE(edit(client, azl::AddEffect{layer, std::nullopt, {0, type, params}, column})["op"] == "addEffect");
    };
    for (uint64_t column : {1, 2})
        if (clipEffects)
            addEffect(1, "colorize", {{"mix", 0.0}}, column);
    for (const auto& [type, params] : chain) {
        if (on == ChainOn::Clips) {
            addEffect(1, type, params, 1);
            addEffect(1, type, params, 2);
        } else {
            addEffect(on == ChainOn::Group ? 10 : 1, type, params, std::nullopt);
        }
    }
    transition(client, type, 2.0);
    REQUIRE(edit(client, azl::SetQuantise{0})["op"] == "setQuantise");
    triggerColumn(client, 1);
    harness.step(144);
    ChainRun run;
    run.steadyPasses = harness.stats().passes;
    for (int i = 0; i < 13; ++i) {
        if (i == 1)
            triggerColumn(client, 2);
        harness.step(6);
        run.frames.push_back(harness.readScreen());
        if (i == 5)
            run.fadePasses = harness.stats().passes;
    }
    CHECK(harness.stats().videoMisses == 0);
    return run;
}

int largestDifference(const std::vector<azl::Image>& a, const std::vector<azl::Image>& b)
{
    int largest = 0;
    for (size_t f = 0; f < a.size(); ++f)
        for (size_t i = 0; i < a[f].rgba.size(); ++i)
            largest = std::max(largest, std::abs(int(a[f].rgba[i]) - int(b[f].rgba[i])));
    return largest;
}

} // namespace

// A layer's chain runs once on its clips mixed by the transition, as the same chain on a group around the layer does,
// with its state, phases and clocks running on from before the switch to after it: a Transform or Shift Glitch moves
// the wipe's edge with the picture, Trails keeps one history, Reveal reshuffles on, Stripper and Strobe keep their
// beat. Run on each clip instead, the pictures part.
TEST_CASE("a layer's chain runs once on its clips mixed by any transition, its state running on through the switch")
{
    const std::vector<EffectSpecs> chains{
        {{"transform", {{"positionX", 0.2}}},
         {"reveal", {{"columns", 5.0}, {"rows", 4.0}, {"progress", 0.7}, {"softness", 0.05}, {"reshuffle", 0.5}}},
         {"trails", {{"feedback", 0.9}}}},
        {{"classicShiftGlitch", {{"frequency", 0.75}, {"vertical", 0.3}}},
         {"classicStripper", {{"speed", 0.3}}},
         {"classicStrobe", {{"frequency", 0.5}, {"colorAlpha", 0.3}}},
         {"classicTrails", {{"feedback", 0.8}}}}};
    for (size_t c = 0; c < chains.size(); ++c)
        for (const TransitionMode type : {TransitionMode::Alpha, TransitionMode::Wipe, TransitionMode::Dissolve,
                                          TransitionMode::RandomSquares})
            for (const bool clipEffects : {false, true}) {
                const std::string what = "chain " + std::to_string(c) + ", " + typeName(type) +
                                         (clipEffects ? " with clip effects" : "");
                CAPTURE(what);
                const int largest = largestDifference(runChain(ChainOn::Layer, type, clipEffects, chains[c]).frames,
                                                      runChain(ChainOn::Group, type, clipEffects, chains[c]).frames);
                MESSAGE(what << ": largest difference " << largest);
                CHECK(largest <= 2);
            }
    CHECK(largestDifference(runChain(ChainOn::Layer, TransitionMode::Wipe, false, chains[0]).frames,
                            runChain(ChainOn::Clips, TransitionMode::Wipe, false, chains[0]).frames) > 100);
}

// As the gig show's base layer (Classic Colorize and Reveal), a chain of one pass in an alpha transition between clips
// without clip effects: it reads both clips at its source and runs once, with no pass added. A soft threshold at 0.75
// turns the clips' bars, black in one clip and white in the other, black on the mix halfway; run on each clip, they
// would show grey.
TEST_CASE("an alpha transition runs a one-pass layer chain once at its source, adding no pass")
{
    const EffectSpecs chain{
        {"classicThreshold", {{"threshold", 0.75}, {"smoothness", 0.4}}},
        {"reveal", {{"columns", 5.0}, {"rows", 4.0}, {"progress", 0.7}, {"softness", 0.05}, {"evolve", 1.0}}}};
    const ChainRun layer = runChain(ChainOn::Layer, TransitionMode::Alpha, false, chain);
    CHECK(layer.steadyPasses == 1);
    CHECK(layer.fadePasses == layer.steadyPasses);
    const int once = largestDifference(layer.frames, runChain(ChainOn::Group, TransitionMode::Alpha, false, chain).frames);
    const int apart = largestDifference(layer.frames, runChain(ChainOn::Clips, TransitionMode::Alpha, false, chain).frames);
    MESSAGE("one-pass chain: largest difference " << once << " from once, " << apart << " from each clip's");
    CHECK(once <= 6);
    CHECK(apart > 100);
}
