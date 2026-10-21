#include "clips.hpp"

#include <map>

namespace {

using azl::ClipRef;
using azl::PlayMode;
using azl::SyncMode;

// At 120 BPM and 60 fps a beat is 30 render frames and a bar 120. count_240 (8 s) and count_709_tv (2 s) pick 16 and
// 4 beats, so either shows clip frame n / 2 at render frame n after it starts.
constexpr azl::EngineConfig kSize{.width = 960, .height = 540};
// Two reverse units hold about 24 frames of count_240, less than its 60-frame GOPs.
constexpr size_t kSmallCache = size_t(2) << 20;

Message edit(Client& client, const azl::Edit& e)
{
    client.send(msg::command(e));
    return client.next();
}

Message send(Client& client, const Message& message)
{
    client.send(message);
    return client.next();
}

Message trigger(Client& client, Message message)
{
    message["type"] = "trigger";
    client.send(message);
    return client.next();
}

Message triggerColumn(Client& client, uint64_t column)
{
    return trigger(client, {{"op", "column"}, {"column", column}});
}

Message triggerClip(Client& client, uint64_t layer, uint64_t column)
{
    return trigger(client, {{"op", "clip"}, {"layer", layer}, {"column", column}});
}

std::string add(Client& client, const fs::path& path)
{
    const Message added = addMedia(client, path);
    REQUIRE(added["type"] == "library");
    return added["entry"]["hash"];
}

void setSlot(Client& client, uint64_t layer, uint64_t column, const std::string& hash, azl::Transport transport = {})
{
    REQUIRE(edit(client, azl::SetSlot{layer, column, ClipRef{hash, "clip", transport}})["op"] == "setSlot");
}

// The column each layer plays as of the last frame; layers playing nothing are left out.
std::map<uint64_t, uint64_t> columns(Client& client)
{
    client.send({{"type", "clock"}, {"op", "state"}});
    const Message state = client.next();
    REQUIRE(state["op"] == "state");
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

// Layer 1 plays count_240 from column 1 from frame 0; column 2 holds count_709_tv, which the bar trigger at frame 50
// is for.
struct TwoClips {
    explicit TwoClips(azl::EngineConfig config = kSize, azl::Transport second = {})
        : harness(config), client(harness.document())
    {
        a = add(client, copyMedia("count_240.mp4", media.path / "a.mp4"));
        b = add(client, copyMedia("count_709_tv.mp4", media.path / "b.mp4"));
        setSlot(client, 1, 1, a);
        setSlot(client, 1, 2, b, second);
        REQUIRE(triggerColumn(client, 1)["beat"] == 0.0);
    }

    TempDir media;
    azl::EngineHarness harness;
    Client client;
    std::string a;
    std::string b;
};

} // namespace

TEST_CASE("a trigger lands on the first frame of the next bar; the old clip plays until then")
{
    TwoClips grid;
    CHECK(frameAfterStep(grid.harness, 50) == 24);
    const Message change = triggerClip(grid.client, 1, 2);
    CHECK(change["op"] == "triggerClip");
    CHECK(change["cause"] == "trigger");
    CHECK(change["beat"] == 4.0);

    for (int n = 50; n < 120; ++n)
        REQUIRE(frameAfterStep(grid.harness) == n / 2);
    CHECK(columns(grid.client)[1] == 1);
    CHECK(frameAfterStep(grid.harness) == 0);
    CHECK(columns(grid.client)[1] == 2);
    CHECK(frameAfterStep(grid.harness) == 0);
    CHECK(frameAfterStep(grid.harness) == 1);
    CHECK(grid.harness.stats().videoMisses == 0);
}

TEST_CASE("quantisation is adjustable: off, a beat, half a bar, N bars")
{
    // The frame the trigger given at frame 50 (beat 1.67) lands on.
    for (const auto [beats, frame] : {std::pair{0, 50}, {1, 60}, {2, 60}, {8, 240}, {16, 480}}) {
        CAPTURE(beats);
        TwoClips grid;
        CHECK(edit(grid.client, azl::SetQuantise{beats})["beats"] == beats);
        grid.harness.step(50);
        triggerClip(grid.client, 1, 2);
        grid.harness.step(frame - 50);
        CHECK(columns(grid.client)[1] == 1);
        CHECK(frameAfterStep(grid.harness) == 0);
        CHECK(columns(grid.client)[1] == 2);
        CHECK(grid.harness.stats().videoMisses == 0);
    }
}

TEST_CASE("at 128 BPM a bar trigger lands on the first frame past beat 4, the clip anchored on the beat itself")
{
    TwoClips grid;
    edit(grid.client, azl::SetTempo{128});
    grid.harness.step(50);
    CHECK(triggerClip(grid.client, 1, 2)["beat"] == 4.0);
    // 128 BPM at 60 fps: beat 4 is frame 112.5. count_240's 16 beats show 15 clip frames a beat.
    CHECK(frameAfterStep(grid.harness, 63) == 59);
    CHECK(frameAfterStep(grid.harness) == 0);
    // count_709_tv's 4 beats: 15 clip frames a beat, so frame 113 + 28 is 1.0178 beats in.
    CHECK(frameAfterStep(grid.harness, 28) == 15);
    CHECK(grid.harness.stats().videoMisses == 0);
}

TEST_CASE("a column trigger switches every layer on the bar; a layer whose cell is empty clears")
{
    TempDir media;
    azl::Composition three = azl::defaultComposition();
    three.layers.push_back({.id = 2, .name = "Layer 2"});
    three.layers.push_back({.id = 3, .name = "Layer 3"});
    azl::EngineHarness harness(kSize, 60.0, {}, three);
    Client client(harness.document());
    const std::string a = add(client, copyMedia("count_240.mp4", media.path / "a.mp4"));
    const std::string b = add(client, copyMedia("count_709_tv.mp4", media.path / "b.mp4"));
    const std::string c = add(client, copyMedia("count_100.mp4", media.path / "c.mp4"));
    setSlot(client, 1, 1, a);
    setSlot(client, 1, 2, b);
    setSlot(client, 2, 1, b);
    setSlot(client, 2, 2, c);
    setSlot(client, 3, 1, c);

    triggerColumn(client, 1);
    harness.step(50);
    CHECK(columns(client) == std::map<uint64_t, uint64_t>{{1, 1}, {2, 1}, {3, 1}});
    const Message change = triggerColumn(client, 2);
    CHECK(change["op"] == "triggerColumn");
    CHECK(change["beat"] == 4.0);
    harness.step(70);
    CHECK(columns(client) == std::map<uint64_t, uint64_t>{{1, 1}, {2, 1}, {3, 1}});
    harness.step();
    CHECK(columns(client) == std::map<uint64_t, uint64_t>{{1, 2}, {2, 2}});
    // Layer 3 on top is cleared, so layer 2's clip shows: count_100 just started.
    CHECK(shownFrame(harness.readScreen()) == 0);
    CHECK(harness.stats().videoMisses == 0);

    Client late(harness.document());
    CHECK(late.snapshot["composition"]["column"] == 2);
    CHECK(late.snapshot["composition"]["layers"][2]["playing"]["column"] == 2);
    CHECK(late.snapshot["composition"]["layers"][2]["clip"].is_null());
}

TEST_CASE("a deck switch keeps what plays and makes the new deck's first column the next one")
{
    TwoClips grid;
    const Message deck = edit(grid.client, azl::AddDeck{std::nullopt, {0, "Deck 2", {{0, ""}, {0, ""}}}, {}});
    REQUIRE(deck["op"] == "addDeck");
    const uint64_t id = deck["deck"]["id"];
    const uint64_t first = deck["deck"]["columns"][0]["id"];
    CHECK(deck["deck"]["columns"][0]["name"] == "Column " + std::to_string(first));
    setSlot(grid.client, 1, first, grid.b, {SyncMode::Timeline, PlayMode::Reverse, 0});

    grid.harness.step(10);
    const Message selected = trigger(grid.client, {{"op", "deck"}, {"deck", id}});
    CHECK(selected["op"] == "selectDeck");
    CHECK(selected["cause"] == "trigger");
    for (int n = 10; n < 50; ++n)
        REQUIRE(frameAfterStep(grid.harness) == n / 2);

    triggerColumn(grid.client, first);
    grid.harness.step(70);
    CHECK(frameAfterStep(grid.harness) == 59);
    CHECK(frameAfterStep(grid.harness, 2) == 58);
    CHECK(grid.harness.stats().videoMisses == 0);
}

TEST_CASE("a deck change to another deck's first column has no miss, unquantised and with the deck never shown")
{
    TwoClips grid;
    const Message deck = edit(grid.client, azl::AddDeck{std::nullopt, {0, "Deck 2", {{0, ""}, {0, ""}}}, {}});
    const uint64_t first = deck["deck"]["columns"][0]["id"];
    setSlot(grid.client, 1, first, grid.b);
    CHECK(edit(grid.client, azl::SetQuantise{0})["beats"] == 0);
    grid.harness.step(10);

    triggerColumn(grid.client, first);
    CHECK(frameAfterStep(grid.harness) == 0);
    CHECK(columns(grid.client)[1] == first);
    CHECK(grid.harness.stats().videoMisses == 0);
}

TEST_CASE("a preloaded long-GOP clip starts in reverse with no miss; cold it misses")
{
    for (const auto [cache, preload] : {std::pair{kSmallCache, azl::EngineConfig{}.preloadBytes},
                                        {azl::EngineConfig{}.clipCacheBytes, azl::EngineConfig{}.preloadBytes},
                                        {kSmallCache, size_t(0)}}) {
        CAPTURE(cache);
        CAPTURE(preload);
        TempDir media;
        azl::EngineConfig config = kSize;
        config.clipCacheBytes = cache;
        config.preloadBytes = preload;
        azl::EngineHarness harness(config);
        Client client(harness.document());
        setSlot(client, 1, 1, add(client, copyMedia("count_709_tv.mp4", media.path / "a.mp4")));
        setSlot(client, 1, 2, add(client, copyMedia("count_240.mp4", media.path / "b.mp4")),
                {SyncMode::Timeline, PlayMode::Reverse, 0});
        triggerColumn(client, 1);
        harness.step(50);
        triggerColumn(client, 2);
        harness.step(70);

        std::vector<int> frames;
        std::vector<int> expected;
        for (int n = 0; n < 200; ++n) {
            frames.push_back(frameAfterStep(harness));
            expected.push_back(239 - n / 2);
        }
        CHECK(frames == expected);
        if (preload)
            CHECK(harness.stats().videoMisses == 0);
        else
            CHECK(harness.stats().videoMisses > 0);
    }
}

TEST_CASE("a cold trigger keeps the old picture until the new clip's first frame, never black")
{
    TwoClips grid;
    setSlot(grid.client, 1, 4, add(grid.client, copyMedia("count_601_pc.mp4", grid.media.path / "c.mp4")));
    grid.harness.step(50);
    grid.harness.holdClipOpens(true);
    triggerClip(grid.client, 1, 4);
    grid.harness.step(70);
    CHECK(shownFrame(grid.harness.readScreen()) == 59);
    for (int n = 120; n < 130; ++n) {
        grid.harness.step();
        const azl::Image screen = grid.harness.readScreen();
        REQUIRE(shownFrame(screen) == 59);
        checkPatches(screen);
    }
    CHECK(grid.harness.stats().videoMisses == 10);
    grid.harness.holdClipOpens(false);
    CHECK(frameAfterStep(grid.harness) == 5);
    CHECK(frameAfterStep(grid.harness) == 5);
}

TEST_CASE("a source destroyed while its last frame is still shown leaves that frame intact")
{
    TwoClips grid;
    setSlot(grid.client, 1, 3, kUnknownHash);
    grid.harness.step(50);
    triggerColumn(grid.client, 3);
    grid.harness.step(70);
    CHECK(shownFrame(grid.harness.readScreen()) == 59);
    REQUIRE(grid.harness.decodeStats().cachedBytes > 0);

    // count_240's decoder closes once nothing plays or preloads it (the deck's first column preloads it until its cell
    // is emptied); its frame stays on the layer.
    edit(grid.client, azl::SetSlot{1, 1, std::nullopt});
    int frames = 0;
    for (; frames < 100 && grid.harness.decodeStats().cachedBytes > 0; ++frames)
        REQUIRE(frameAfterStep(grid.harness) == 59);
    CHECK(grid.harness.decodeStats().cachedBytes == 0);
    for (int n = 0; n < 30; ++n) {
        grid.harness.step();
        const azl::Image screen = grid.harness.readScreen();
        REQUIRE(shownFrame(screen) == 59);
        checkPatches(screen);
    }
}

TEST_CASE("a corrupt clip preloads without stalling, and triggered it keeps the layer's picture and is reported")
{
    TwoClips grid;
    const std::string corrupt = add(grid.client, copyMedia("truncated.mp4", grid.media.path / "corrupt.mp4"));
    setSlot(grid.client, 1, 2, corrupt);

    // Column 2 is the next one, so the corrupt clip is opened in the background as soon as the layer plays.
    CHECK(frameAfterStep(grid.harness) == 0);
    Message failed = grid.client.next();
    CHECK(failed["type"] == "error");
    CHECK(failed["hash"] == corrupt);
    CHECK(failed["layer"] == 1);
    for (int n = 1; n < 50; ++n)
        REQUIRE(frameAfterStep(grid.harness) == n / 2);

    triggerColumn(grid.client, 2);
    grid.harness.step(70);
    for (int n = 0; n < 30; ++n) {
        grid.harness.step();
        const azl::Image screen = grid.harness.readScreen();
        REQUIRE(shownFrame(screen) == 59);
        checkPatches(screen);
    }
    failed = grid.client.next();
    CHECK(failed["type"] == "error");
    CHECK(failed["hash"] == corrupt);
    CHECK(grid.harness.stats().videoMisses == 0);
}

TEST_CASE("triggering the playing clip restarts it on the bar; a once-and-clear clip can be triggered again")
{
    TwoClips grid({.width = 960, .height = 540}, {SyncMode::Bpm, PlayMode::OnceClear, 0});
    grid.harness.step(50);
    triggerClip(grid.client, 1, 1);
    CHECK(frameAfterStep(grid.harness, 70) == 59);
    CHECK(frameAfterStep(grid.harness) == 0);
    CHECK(frameAfterStep(grid.harness, 2) == 1);

    // count_709_tv once: 4 beats from beat 8 (frame 240), then nothing.
    triggerColumn(grid.client, 2);
    CHECK(frameAfterStep(grid.harness, 240 - 123 + 1) == 0);
    CHECK(frameAfterStep(grid.harness, 119) == 59);
    grid.harness.step();
    CHECK(blank(grid.harness.readScreen()));
    triggerClip(grid.client, 1, 2);
    grid.harness.step(119);
    CHECK(blank(grid.harness.readScreen()));
    CHECK(frameAfterStep(grid.harness) == 0);
    CHECK(grid.harness.stats().videoMisses == 0);
}

TEST_CASE("the clip grid is edited with undoable commands; triggers are live and stay out of the undo history")
{
    TempDir media;
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    const Message& composition = client.snapshot["composition"];
    CHECK(composition["decks"].size() == 1);
    CHECK(composition["decks"][0]["columns"].size() == 4);
    CHECK(composition["deck"] == 1);
    CHECK(composition["quantise"] == 4);
    const std::string a = add(client, copyMedia("count_709_tv.mp4", media.path / "a.mp4"));

    setSlot(client, 1, 2, a);
    CHECK(triggerClip(client, 1, 2)["beat"] == 0.0);
    CHECK(send(client, msg::undo())["op"] == "setSlot");
    Client late(harness.document());
    CHECK(late.snapshot["composition"]["layers"][0]["clips"].empty());
    CHECK(late.snapshot["composition"]["layers"][0]["playing"]["column"] == 2);
    CHECK(send(client, msg::redo())["clip"]["hash"] == a);

    // Removing a column takes its cells; undo brings both back.
    CHECK(edit(client, azl::RemoveColumn{2})["op"] == "removeColumn");
    CHECK(send(client, msg::undo())["cells"][0]["clip"]["hash"] == a);
    CHECK(edit(client, azl::AddColumn{1, 0, {0, "First"}, {}})["column"]["id"] == 5);
    Client later(harness.document());
    CHECK(later.snapshot["composition"]["decks"][0]["columns"][0]["name"] == "First");
    CHECK(later.snapshot["composition"]["layers"][0]["clips"]["2"]["hash"] == a);

    CHECK(edit(client, azl::RemoveDeck{1})["type"] == "error");
    CHECK(edit(client, azl::AddDeck{})["deck"]["id"] == 2);
    CHECK(edit(client, azl::RemoveDeck{2})["op"] == "removeDeck");

    CHECK(edit(client, azl::TriggerClip{1, 2, 0.0})["type"] == "error");
    CHECK(triggerClip(client, 1, 3)["type"] == "error");
    CHECK(triggerColumn(client, 99)["type"] == "error");
    CHECK(trigger(client, {{"op", "deck"}, {"deck", 7}})["type"] == "error");
    CHECK(trigger(client, {{"op", "arm"}, {"layer", 1}, {"column", 3}})["armed"] == true);
    for (const int beats : {-1, 3, 5, 68})
        CHECK(edit(client, azl::SetQuantise{beats})["type"] == "error");
    CHECK(edit(client, azl::SetSlot{1, 99, ClipRef{a, "a", {}}})["type"] == "error");
}

TEST_CASE("setLayerClip fills the cell the layer plays, the shown deck's first column when it plays nothing")
{
    TempDir media;
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    const Message entry = playMedia(client, copyMedia("count_709_tv.mp4", media.path / "a.mp4"));
    CHECK(frameAfterStep(harness, 3) == 1);
    Client late(harness.document());
    const Message& layer = late.snapshot["composition"]["layers"][0];
    CHECK(layer["clips"]["1"]["hash"] == entry["hash"]);
    CHECK(layer["playing"]["column"] == 1);
    CHECK(layer["playing"]["beat"].is_null());

    setSlot(client, 1, 3, entry["hash"]);
    triggerColumn(client, 3);
    harness.step(120);
    REQUIRE(columns(client)[1] == 3);
    const std::string other = add(client, copyMedia("count_100.mp4", media.path / "b.mp4"));
    REQUIRE(setClip(client, other)["type"] == "change");
    CHECK(frameAfterStep(harness) == 0);
    Client later(harness.document());
    CHECK(later.snapshot["composition"]["layers"][0]["clips"]["1"]["hash"] == entry["hash"]);
    CHECK(later.snapshot["composition"]["layers"][0]["clips"]["3"]["hash"] == other);
    CHECK(later.snapshot["composition"]["layers"][0]["clip"]["hash"] == other);
}
