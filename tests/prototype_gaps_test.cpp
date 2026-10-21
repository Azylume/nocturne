#include "clips.hpp"

#include "document/cue_edits.hpp"

#include <map>

namespace {

using azl::ClipRef;
using azl::PlayMode;
using azl::SyncMode;

constexpr azl::EngineConfig kSize{.width = 960, .height = 540};

Message send(Client& client, Message message)
{
    client.send(message);
    return client.next();
}

// Skips what other services broadcast meanwhile, such as a clip that failed to open.
Message reply(Client& client, const Message& message)
{
    client.send(message);
    for (;;) {
        Message m = client.next();
        if (m["type"] == message["type"] || (m["type"] == "error" && !m.contains("hash")))
            return m;
    }
}

Message trigger(Client& client, Message message)
{
    message["type"] = "trigger";
    return send(client, message);
}

std::string add(Client& client, const fs::path& path)
{
    const Message added = addMedia(client, path);
    REQUIRE(added["type"] == "library");
    return added["entry"]["hash"];
}

void setSlot(Client& client, uint64_t layer, uint64_t column, const std::string& hash, azl::Transport transport = {})
{
    REQUIRE(command(client, azl::SetSlot{layer, column, ClipRef{hash, "clip", transport}})["op"] == "setSlot");
}

std::map<uint64_t, uint64_t> columns(Client& client)
{
    const Message state = reply(client, {{"type", "clock"}, {"op", "state"}});
    std::map<uint64_t, uint64_t> playing;
    for (const Message& clip : state["clips"])
        playing[clip["layer"].get<uint64_t>()] = clip["column"].get<uint64_t>();
    return playing;
}

bool blank(const azl::Image& image)
{
    for (const auto [x, y] : {std::pair{60u, 135u}, {240u, 405u}, {720u, 405u}})
        if (pixel(image, x, y)[0] > 4 || pixel(image, x, y)[1] > 4 || pixel(image, x, y)[2] > 4)
            return false;
    return true;
}

Message undo(Client& client)
{
    return send(client, msg::undo());
}

// Layer 1 plays count_709_tv from column 1 from beat 0.
struct Grid {
    explicit Grid(azl::Composition initial = azl::defaultComposition(), azl::EngineConfig config = kSize)
        : harness(config, 60.0, {}, initial), client(harness.document())
    {
        a = add(client, copyMedia("count_709_tv.mp4", media.path / "a.mp4"));
        b = add(client, copyMedia("count_240.mp4", media.path / "b.mp4"));
        setSlot(client, 1, 1, a);
    }

    void start() { REQUIRE(trigger(client, {{"op", "column"}, {"column", 1}})["beat"] == 0.0); }

    TempDir media;
    azl::EngineHarness harness;
    Client client;
    std::string a;
    std::string b;
};

} // namespace

TEST_CASE("the composition's master opacity fades the picture to black, as one undo step, and a macro drives it")
{
    Grid grid;
    grid.start();
    grid.harness.step();
    checkPatches(grid.harness.readScreen());

    CHECK(apply(grid.client, azl::SetLayerOpacity{0, 0.5})["layer"] == 0);
    grid.harness.step();
    const azl::Image half = grid.harness.readScreen();
    checkColour(half, 240, 405, 100, 20, 20);
    checkColour(half, 720, 405, 20, 80, 40);
    CHECK(Client(grid.harness.document()).snapshot["composition"]["opacity"] == 0.5);
    CHECK(command(grid.client, azl::SetLayerOpacity{0, 1.5})["type"] == "error");

    undo(grid.client);
    grid.harness.step();
    checkPatches(grid.harness.readScreen());
    CHECK_FALSE(Client(grid.harness.document()).snapshot["composition"].contains("opacity"));

    azl::Macro master{.name = "Master", .value = 1.0, .targets = {{azl::ParamRef{0, 0, "opacity"}, {}, 0.0, -1.0}}};
    apply(grid.client, azl::AddMacro{std::nullopt, master});
    grid.harness.step();
    CHECK(blank(grid.harness.readScreen()));
}

// Triggers columns 2, 3 and 4, each into a part of count_240, and checks the frames; returns the misses.
static uint64_t playRegions(azl::EngineConfig config)
{
    Grid grid(azl::defaultComposition(), config);
    // count_240 at 30 fps: in 0.4 and out 0.65 play frames 96..155.
    setSlot(grid.client, 1, 2, grid.b, {.sync = SyncMode::Timeline, .speed = 2.0, .in = 0.4, .out = 0.65});
    setSlot(grid.client, 1, 3, grid.b, {.sync = SyncMode::Timeline, .playMode = PlayMode::Reverse, .in = 0.4, .out = 0.65});
    setSlot(grid.client, 1, 4, grid.b, {.beats = 4, .in = 0.5, .out = 0.75});
    grid.start();
    grid.harness.step(50);

    // Speed 2 at 60 render fps shows one clip frame per render frame, looping within the region.
    trigger(grid.client, {{"op", "column"}, {"column", 2}});
    grid.harness.step(70);
    std::vector<int> frames, expected;
    for (int n = 0; n < 150; ++n) {
        frames.push_back(frameAfterStep(grid.harness));
        expected.push_back(96 + n % 60);
    }
    CHECK(frames == expected);

    // Reverse starts at the out point.
    trigger(grid.client, {{"op", "column"}, {"column", 3}});
    grid.harness.step(90);
    frames.clear();
    expected.clear();
    for (int n = 0; n < 150; ++n) {
        frames.push_back(frameAfterStep(grid.harness));
        expected.push_back(155 - n % 120 / 2);
    }
    CHECK(frames == expected);

    // BPM-synced, the region (frames 120..179) spans the clip's 4 beats.
    trigger(grid.client, {{"op", "column"}, {"column", 4}});
    grid.harness.step(90);
    frames.clear();
    expected.clear();
    for (int n = 0; n < 150; ++n) {
        frames.push_back(frameAfterStep(grid.harness));
        expected.push_back(120 + n % 120 / 2);
    }
    CHECK(frames == expected);
    return grid.harness.stats().videoMisses;
}

TEST_CASE("per-clip speed, in and out play the right frames; triggers into them preload with no miss, cold they miss")
{
    CHECK(playRegions(kSize) == 0);
    azl::EngineConfig cold = kSize;
    cold.preloadBytes = 0;
    CHECK(playRegions(cold) > 0);
}

TEST_CASE("speed, in and out are undoable transport fields with their limits")
{
    Grid grid;
    setSlot(grid.client, 1, 4, grid.b, {.beats = 4, .in = 0.5, .out = 0.75});
    CHECK(command(grid.client, azl::SetClipTransport{.layer = 1, .column = 4, .in = 0.8})["type"] == "error");
    CHECK(command(grid.client, azl::SetClipTransport{.layer = 1, .column = 4, .speed = 5.0})["type"] == "error");
    const Message change = apply(grid.client, azl::SetClipTransport{.layer = 1, .column = 4, .speed = 0.5});
    CHECK(change["speed"] == 0.5);
    undo(grid.client);
    CHECK(Client(grid.harness.document()).snapshot["composition"]["layers"][0]["clips"]["4"]["transport"].value(
              "speed", 1.0) == 1.0);
}

TEST_CASE("a clip's trigger mode overrides the composition's quantisation, undoably")
{
    Grid grid;
    setSlot(grid.client, 1, 2, grid.b);
    grid.start();
    grid.harness.step(50);
    CHECK(apply(grid.client, azl::SetClipTrigger{1, 2, 1})["quantise"] == 1);
    CHECK(trigger(grid.client, {{"op", "clip"}, {"layer", 1}, {"column", 2}})["beat"] == 2.0);
    CHECK(command(grid.client, azl::SetClipTrigger{1, 2, 3})["type"] == "error");
    apply(grid.client, azl::SetClipTrigger{1, 2, 0});
    const double now = trigger(grid.client, {{"op", "clip"}, {"layer", 1}, {"column", 2}})["beat"];
    CHECK(now == doctest::Approx(50.0 / 30.0));
    undo(grid.client);
    undo(grid.client);
    CHECK(Client(grid.harness.document()).snapshot["composition"]["layers"][0]["clips"]["2"].value("quantise", -1) == -1);
    CHECK(trigger(grid.client, {{"op", "clip"}, {"layer", 1}, {"column", 2}})["beat"] == 4.0);
}

TEST_CASE("stop layer is live: the layer shows nothing until its next trigger, and nothing lands in the history")
{
    Grid grid;
    grid.start();
    grid.harness.step(10);
    const Message before = Client(grid.harness.document()).snapshot["history"];
    const Message change = trigger(grid.client, {{"op", "stop"}, {"layer", 1}});
    CHECK(change["op"] == "stopLayer");
    CHECK(change["cause"] == "trigger");
    grid.harness.step();
    CHECK(blank(grid.harness.readScreen()));
    const Client late(grid.harness.document());
    CHECK(late.snapshot["composition"]["layers"][0]["playing"].is_null());
    CHECK(late.snapshot["history"] == before);
    CHECK(command(grid.client, azl::StopLayer{1})["type"] == "error");

    trigger(grid.client, {{"op", "clip"}, {"layer", 1}, {"column", 1}});
    grid.harness.step(120);
    CHECK_FALSE(blank(grid.harness.readScreen()));
}

TEST_CASE("relink finds missing media again by hash or at a new path, and the clip plays at once")
{
    Grid grid;
    const fs::path original = copyMedia("count_601_pc.mp4", grid.media.path / "show" / "loop.mp4");
    const std::string hash = add(grid.client, original);
    fs::remove(original);
    setSlot(grid.client, 1, 2, hash);
    grid.start();
    trigger(grid.client, {{"op", "clip"}, {"layer", 1}, {"column", 2}});
    grid.harness.step(130);
    CHECK(columns(grid.client).count(1) == 0);

    const fs::path moved = copyMedia("count_601_pc.mp4", grid.media.path / "elsewhere" / "renamed.mp4");
    Message relinked = reply(grid.client, {{"type", "library"}, {"op", "relink"}, {"hash", hash}, {"path", moved.string()}});
    REQUIRE(relinked["op"] == "relink");
    CHECK(relinked["entry"]["hash"] == hash);
    grid.harness.step(5);
    CHECK(columns(grid.client)[1] == 2);
    CHECK(shownFrame(grid.harness.readScreen()) >= 0);

    // Moved again inside a library folder, it is found by hash alone.
    fs::rename(moved, grid.media.path / "elsewhere" / "again.mp4");
    relinked = reply(grid.client, {{"type", "library"}, {"op", "relink"}, {"hash", hash}});
    CHECK(relinked["entry"]["path"] == (grid.media.path / "elsewhere" / "again.mp4").string());

    const fs::path other = copyMedia("count_100.mp4", grid.media.path / "elsewhere" / "other.mp4");
    relinked = reply(grid.client, {{"type", "library"}, {"op", "relink"}, {"hash", hash}, {"path", other.string()}});
    CHECK(relinked["entry"]["hash"] != hash);
    CHECK(reply(grid.client, {{"type", "library"}, {"op", "relink"}, {"hash", kUnknownHash}})["type"] == "error");
}

TEST_CASE("a macro Cue ramps over its bars from the value it held, and a later ramp starts where the first one is")
{
    azl::Composition composition = azl::defaultComposition();
    composition.macros.push_back({.id = 1, .name = "Energy", .value = 0.2});
    composition.cues.push_back({.id = 1, .section = azl::Section::Drop, .bar = 1, .macros = {{1, 1.0, 2}}});
    composition.cues.push_back({.id = 2, .section = azl::Section::Drop, .bar = 2, .macros = {{1, 0.0, 1}}});
    composition.timeline.sections = {{azl::Section::Drop, 4.0}};
    azl::CueState state;
    auto at = [&](double beat) {
        azl::cueStateAt(composition, beat, state);
        REQUIRE(state.macros.size() == 1);
        return state.macros[0].value;
    };
    CHECK(at(4.0) == doctest::Approx(0.2));
    CHECK(at(6.0) == doctest::Approx(0.4));
    CHECK(at(8.0) == doctest::Approx(0.6));
    CHECK(at(10.0) == doctest::Approx(0.3));
    CHECK(at(12.0) == doctest::Approx(0.0));
    CHECK(at(30.0) == doctest::Approx(0.0));

    azl::EngineHarness harness(kSize, 60.0, {}, composition);
    harness.step(241);
    const auto macros = harness.engine().modulation()->macros();
    REQUIRE(macros.size() == 1);
    CHECK(macros[0].value == doctest::Approx(0.6).epsilon(0.01));
}

TEST_CASE("a column Cue fires its own deck's column whatever deck is shown; the current-deck option follows the deck")
{
    azl::Composition composition = azl::defaultComposition();
    composition.decks = {{1, "Deck 1", {{1, "A"}, {2, "B"}}}, {2, "Deck 2", {{3, "C"}, {4, "D"}}}};
    Grid grid(composition);
    const std::string c = add(grid.client, copyMedia("count_100.mp4", grid.media.path / "c.mp4"));
    setSlot(grid.client, 1, 2, c);
    setSlot(grid.client, 1, 3, c);
    setSlot(grid.client, 1, 4, grid.b);

    azl::Cue fixed{.section = azl::Section::Drop, .column = azl::CueColumn{4, std::nullopt}};
    azl::Cue shown{.section = azl::Section::Build, .column = azl::CueColumn{0, 1}};
    CHECK(command(grid.client, azl::AddCue{std::nullopt, {.column = azl::CueColumn{4, 1}}})["type"] == "error");
    CHECK(command(grid.client, azl::AddCue{std::nullopt, {.column = azl::CueColumn{9, std::nullopt}}})["type"] == "error");
    const Message added = apply(grid.client, azl::AddCue{std::nullopt, fixed});
    CHECK(added["cue"]["column"] == Message{{"column", 4}});
    apply(grid.client, azl::AddCue{std::nullopt, shown});
    grid.start();

    send(grid.client, {{"type", "cue"}, {"op", "mark"}, {"section", "drop"}, {"bar", 2}});
    trigger(grid.client, {{"op", "deck"}, {"deck", 2}});
    grid.harness.step(60);
    trigger(grid.client, {{"op", "deck"}, {"deck", 1}});
    grid.harness.step(60);
    CHECK(columns(grid.client)[1] == 1);
    grid.harness.step();
    CHECK(columns(grid.client)[1] == 4);
    CHECK(shownFrame(grid.harness.readScreen()) == 0);

    send(grid.client, {{"type", "cue"}, {"op", "mark"}, {"section", "build"}, {"bar", 4}});
    grid.harness.step(240);
    CHECK(columns(grid.client)[1] == 2);

    trigger(grid.client, {{"op", "deck"}, {"deck", 2}});
    send(grid.client, {{"type", "cue"}, {"op", "mark"}, {"section", "build"}, {"bar", 6}});
    grid.harness.step(240);
    CHECK(columns(grid.client)[1] == 4);
    CHECK(grid.harness.stats().videoMisses == 0);

    // A trigger takes the layer back from the Cue.
    trigger(grid.client, {{"op", "clip"}, {"layer", 1}, {"column", 3}});
    grid.harness.step(120);
    CHECK(columns(grid.client)[1] == 3);
}

TEST_CASE("a snapshot lists every undo and redo step, so a client joining late shows the whole history")
{
    Grid grid;
    apply(grid.client, azl::SetLayerOpacity{1, 0.5});
    apply(grid.client, azl::SetQuantise{1});
    apply(grid.client, azl::SetTempo{130});
    undo(grid.client);
    const Client late(grid.harness.document());
    const Message steps = late.snapshot["steps"];
    CHECK(steps["undo"] == Message::array({"setSlot", "setLayerOpacity", "setQuantise"}));
    CHECK(steps["undo"].size() == late.snapshot["history"]["undo"].get<size_t>());
    CHECK(steps["redo"] == Message::array({"setTempo"}));
}

TEST_CASE("identify shows each output's screen name and output large over the test card, for a while")
{
    Grid grid;
    grid.start();
    apply(grid.client, azl::AddScreen{std::nullopt, {.name = "Left", .display = "proj-a", .slices = {azl::Slice{}}}});
    grid.harness.step();
    auto red = [](const azl::Image& image) {
        const uint8_t* p = pixel(image, 0, 270);
        return p[0] > 250 && p[1] < 5 && p[2] < 5;
    };
    CHECK_FALSE(red(grid.harness.readScreen(1)));
    const Message list = send(grid.client, {{"type", "outputs"}, {"op", "identify"}, {"seconds", 3}});
    CHECK(list["op"] == "list");
    CHECK(red(grid.harness.readScreen(1)));
    send(grid.client, {{"type", "outputs"}, {"op", "identify"}, {"display", "proj-a"}, {"seconds", 0}});
    CHECK(red(grid.harness.readScreen(1)));
    send(grid.client, {{"type", "outputs"}, {"op", "identify"}, {"seconds", 0}});
    CHECK_FALSE(red(grid.harness.readScreen(1)));
}
