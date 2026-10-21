#include "clips.hpp"

#include "document/composition_file.hpp"

#include <algorithm>
#include <map>
#include <vector>

namespace {

using azl::AutopilotMode;
using azl::ClipRef;

// At 120 BPM and 60 fps a bar is 120 render frames; count_709_tv and count_601_pc are a bar long, count_100 two bars.
constexpr azl::EngineConfig kSize{.width = 960, .height = 540};
const char* const kFiles[] = {"count_709_tv.mp4", "count_601_pc.mp4", "count_100.mp4", "count_709_tv.mp4"};

Message edit(Client& client, const azl::Edit& e)
{
    client.send(msg::command(e));
    return client.next();
}

// Two layers and two decks: deck 1 holds columns 1 to 4, deck 2 columns 5 and 6.
azl::Composition twoLayers()
{
    azl::Composition composition = azl::defaultComposition();
    composition.layers.push_back({.id = 2, .name = "Layer 2"});
    composition.decks.push_back({2, "Deck 2", {{5, "Column 5"}, {6, "Column 6"}}});
    return composition;
}

struct Show {
    explicit Show(uint32_t seed = 1, double fps = 60.0)
        : harness(config(seed), fps, {}, twoLayers()), client(harness.document())
    {
    }

    static azl::EngineConfig config(uint32_t seed)
    {
        azl::EngineConfig c = kSize;
        c.seed = seed;
        return c;
    }

    void fill(uint64_t layer, uint64_t column)
    {
        const char* file = kFiles[(column - 1) % 4];
        const fs::path path = media.path / (std::to_string(layer) + "-" + std::to_string(column) + "-" + file);
        const Message added = addMedia(client, copyMedia(file, path));
        REQUIRE(added["type"] == "library");
        const ClipRef clip{added["entry"]["hash"], "clip", {}};
        REQUIRE(edit(client, azl::SetSlot{layer, column, clip})["op"] == "setSlot");
    }

    void columns(AutopilotMode mode, int bars)
    {
        REQUIRE(edit(client, azl::SetColumnAutopilot{mode, bars})["op"] == "setColumnAutopilot");
    }

    void triggerColumn(uint64_t column)
    {
        client.send({{"type", "trigger"}, {"op", "column"}, {"column", column}});
        REQUIRE(client.next()["op"] == "triggerColumn");
    }

    void triggerClip(uint64_t layer, uint64_t column)
    {
        client.send({{"type", "trigger"}, {"op", "clip"}, {"layer", layer}, {"column", column}});
        REQUIRE(client.next()["op"] == "triggerClip");
    }

    void selectDeck(uint64_t deck)
    {
        client.send({{"type", "trigger"}, {"op", "deck"}, {"deck", deck}});
        REQUIRE(client.next()["op"] == "selectDeck");
    }

    Message state()
    {
        client.send({{"type", "clock"}, {"op", "state"}});
        Message state = client.next();
        while (state["type"] == "error")
            state = client.next();
        REQUIRE(state["op"] == "state");
        return state;
    }

    // The column each layer plays as of the last frame; a layer playing nothing is missing.
    std::map<uint64_t, uint64_t> playing()
    {
        std::map<uint64_t, uint64_t> columns;
        const Message now = state();
        for (const Message& clip : now["clips"])
            columns[clip["layer"]] = clip["column"];
        return columns;
    }

    Message layer(uint64_t id)
    {
        const Message now = state();
        for (const Message& clip : now["clips"])
            if (clip["layer"] == id)
                return clip;
        FAIL("layer " << id << " plays nothing");
        return {};
    }

    TempDir media;
    azl::EngineHarness harness;
    Client client;
};

using Playing = std::map<uint64_t, uint64_t>;

} // namespace

TEST_CASE("the column Autopilot triggers the next column of the shown deck on every layer every N bars, on the bar")
{
    Show show;
    for (const uint64_t column : {1, 2, 3})
        show.fill(1, column);
    show.fill(2, 1);
    show.fill(2, 3);
    show.triggerColumn(1);
    show.columns(AutopilotMode::Next, 1);

    // Column 2 clears layer 2, whose cell there is empty; column 4 is empty on every layer and skipped.
    show.harness.step(120);
    CHECK(show.playing() == Playing{{1, 1}, {2, 1}});
    CHECK(show.state()["columns"] == Message{{"column", 1}, {"next", 2}, {"held", 0}});
    show.harness.step();
    CHECK(show.playing() == Playing{{1, 2}});
    CHECK(show.layer(1)["frame"] == 0);
    CHECK(show.state()["columns"]["next"] == 3);
    show.harness.step(120);
    CHECK(show.playing() == Playing{{1, 3}, {2, 3}});
    show.harness.step(120);
    CHECK(show.playing() == Playing{{1, 1}, {2, 1}});

    // Every two bars from the bar it is in.
    show.columns(AutopilotMode::Next, 2);
    show.harness.step(239);
    CHECK(show.playing() == Playing{{1, 1}, {2, 1}});
    show.harness.step();
    CHECK(show.playing() == Playing{{1, 2}});
    show.harness.step(239);
    CHECK(show.playing() == Playing{{1, 2}});
    show.harness.step();
    CHECK(show.playing() == Playing{{1, 3}, {2, 3}});

    // Switched off, the layers hold what it played last.
    show.columns(AutopilotMode::Off, 2);
    show.harness.step(480);
    CHECK(show.playing() == Playing{{1, 3}, {2, 3}});
    CHECK(show.state()["columns"] == Message{{"column", 0}, {"next", 0}, {"held", 0}});
    CHECK(show.harness.stats().videoMisses == 0);
}

TEST_CASE("random column Autopilot repeats with its seed and never plays the same column twice in a row")
{
    auto sequence = [](uint32_t seed) {
        Show show(seed, 15.0);
        for (const uint64_t column : {1, 2, 3, 4})
            show.fill(1, column);
        show.triggerColumn(1);
        show.columns(AutopilotMode::Random, 1);
        std::vector<uint64_t> columns;
        for (int bar = 0; bar < 24; ++bar) {
            show.harness.step(30);
            columns.push_back(show.layer(1)["column"]);
        }
        CHECK(show.harness.stats().videoMisses == 0);
        return columns;
    };
    const std::vector<uint64_t> columns = sequence(7);
    CHECK(columns == sequence(7));
    CHECK(columns != sequence(8));
    for (size_t i = 1; i < columns.size(); ++i)
        CHECK(columns[i] != columns[i - 1]);
    for (const uint64_t column : {1, 2, 3, 4})
        CHECK(std::count(columns.begin(), columns.end(), column) > 0);
}

TEST_CASE("while the column Autopilot runs the layers' Autopilots hold their clips, and step on once it is off")
{
    Show show;
    for (const uint64_t column : {1, 2, 3})
        show.fill(1, column);
    REQUIRE(edit(show.client, azl::SetLayerAutopilot{1, AutopilotMode::Next, 1})["op"] == "setLayerAutopilot");
    show.triggerColumn(1);
    show.harness.step(130);
    CHECK(show.layer(1)["column"] == 2);
    CHECK(show.layer(1)["next"] == 3);

    // Paused, the layer keeps the clip its Autopilot played rather than going back to its trigger, and names no next.
    show.columns(AutopilotMode::Next, 4);
    show.harness.step(240);
    CHECK(show.layer(1)["column"] == 2);
    CHECK(show.layer(1)["next"] == 0);

    // Switched off in bar 4, the layer's Autopilot steps on from that bar, a bar later.
    show.columns(AutopilotMode::Off, 4);
    show.harness.step(10);
    CHECK(show.layer(1)["column"] == 2);
    CHECK(show.layer(1)["next"] == 3);
    show.harness.step(110);
    CHECK(show.layer(1)["column"] == 3);

    // Switched on in bar 5, it steps on bar 6 to the column after the one triggered last, replacing what the layer's
    // Autopilot played; once off, that Autopilot moves on from the column a bar later.
    show.columns(AutopilotMode::Next, 1);
    show.harness.step(110);
    CHECK(show.layer(1)["column"] == 3);
    show.harness.step();
    CHECK(show.layer(1)["column"] == 2);
    show.columns(AutopilotMode::Off, 1);
    show.harness.step(119);
    CHECK(show.layer(1)["column"] == 2);
    show.harness.step();
    CHECK(show.layer(1)["column"] == 3);
    CHECK(show.harness.stats().videoMisses == 0);
}

TEST_CASE("a trigger holds the column Autopilot for its hold, scaled by the pace; a column trigger moves it on from "
          "there")
{
    Show show;
    for (const uint64_t column : {1, 2, 3}) {
        show.fill(1, column);
        show.fill(2, column);
    }
    REQUIRE(edit(show.client, azl::SetColumnAutopilot{std::nullopt, std::nullopt, 2})["hold"] == 2);
    show.triggerColumn(1);
    show.columns(AutopilotMode::Next, 1);
    show.harness.step(130);
    CHECK(show.playing() == Playing{{1, 2}, {2, 2}});

    // Given in bar 2, a clip trigger lands on bar 3 (beat 8) and holds every layer until bar 5 (beat 16).
    show.triggerClip(1, 3);
    show.harness.step(111);
    CHECK(show.playing() == Playing{{1, 3}, {2, 2}});
    CHECK(show.state()["columns"]["held"] == 2);
    show.harness.step(238);
    CHECK(show.playing() == Playing{{1, 3}, {2, 2}});
    CHECK(show.state()["columns"]["held"] == 1);
    show.harness.step(2);
    CHECK(show.playing() == Playing{{1, 3}, {2, 3}});
    CHECK(show.state()["columns"]["held"] == 0);
    show.harness.step(120);
    CHECK(show.playing() == Playing{{1, 1}, {2, 1}});

    // At x2 a column trigger in bar 6 lands on bar 7 (beat 24), holds four bars and Autopilot moves on from it.
    REQUIRE(edit(show.client, azl::SetAutopilotPace{2.0})["op"] == "setAutopilotPace");
    show.harness.step(10);
    show.triggerColumn(3);
    show.harness.step(111);
    CHECK(show.playing() == Playing{{1, 3}, {2, 3}});
    CHECK(show.state()["columns"]["held"] == 4);
    CHECK(show.state()["columns"]["next"] == 1);
    show.harness.step(470);
    CHECK(show.playing() == Playing{{1, 3}, {2, 3}});
    show.harness.step(20);
    CHECK(show.playing() == Playing{{1, 1}, {2, 1}});

    // Triggering the column triggered last again moves it on from that column too.
    show.triggerColumn(3);
    show.harness.step(111);
    CHECK(show.playing() == Playing{{1, 3}, {2, 3}});
    CHECK(show.state()["columns"]["next"] == 1);
    CHECK(show.harness.stats().videoMisses == 0);
}

TEST_CASE("the pace scales the column Autopilot's steps, and a change rescales the running one")
{
    Show show;
    for (const uint64_t column : {1, 2, 3})
        show.fill(1, column);
    show.triggerColumn(1);
    show.columns(AutopilotMode::Next, 1);
    REQUIRE(edit(show.client, azl::SetAutopilotPace{2.0})["op"] == "setAutopilotPace");
    show.harness.step(239);
    CHECK(show.layer(1)["column"] == 1);
    show.harness.step(2);
    CHECK(show.layer(1)["column"] == 2);

    // Half way through a two-bar step, x4 leaves half of a four-bar step: it ends on beat 16, not 12 or 24.
    show.harness.step(119);
    REQUIRE(edit(show.client, azl::SetAutopilotPace{4.0})["op"] == "setAutopilotPace");
    show.harness.step(238);
    CHECK(show.layer(1)["column"] == 2);
    show.harness.step(4);
    CHECK(show.layer(1)["column"] == 3);
    CHECK(show.harness.stats().videoMisses == 0);
}

TEST_CASE("after a deck switch the column Autopilot's next step lands in the shown deck")
{
    Show show;
    for (const uint64_t column : {1, 2, 5, 6})
        show.fill(1, column);
    show.fill(2, 6);
    show.triggerColumn(1);
    show.columns(AutopilotMode::Next, 1);
    show.harness.step(130);
    CHECK(show.layer(1)["column"] == 2);
    CHECK(show.state()["columns"]["next"] == 1);

    show.selectDeck(2);
    show.harness.step();
    CHECK(show.state()["columns"]["next"] == 5);
    show.harness.step(110);
    CHECK(show.playing() == Playing{{1, 5}});
    show.harness.step(120);
    CHECK(show.playing() == Playing{{1, 6}, {2, 6}});
    show.harness.step(120);
    CHECK(show.playing() == Playing{{1, 5}});

    // Random in a deck of two alternates.
    show.columns(AutopilotMode::Random, 1);
    show.harness.step(120);
    CHECK(show.layer(1)["column"] == 6);
    show.harness.step(120);
    CHECK(show.layer(1)["column"] == 5);
    CHECK(show.harness.stats().videoMisses == 0);
}

TEST_CASE("moving the shown deck keeps it shown and the column Autopilot stepping through it, across undo and redo")
{
    Show show;
    for (const uint64_t column : {1, 2, 5, 6})
        show.fill(1, column);
    show.selectDeck(2);
    show.triggerColumn(5);
    show.columns(AutopilotMode::Next, 1);
    auto deckOrder = [&] {
        std::vector<uint64_t> ids;
        for (const azl::Deck& deck : show.harness.document().document().decks)
            ids.push_back(deck.id);
        return ids;
    };

    const Message moved = edit(show.client, azl::MoveDeck{2, 0});
    CHECK(moved["op"] == "moveDeck");
    CHECK(moved["index"] == 0);
    CHECK(deckOrder() == std::vector<uint64_t>{2, 1});
    show.harness.step(130);
    CHECK(show.playing() == Playing{{1, 6}});
    show.client.send(msg::undo());
    CHECK(show.client.next()["index"] == 1);
    CHECK(deckOrder() == std::vector<uint64_t>{1, 2});
    show.harness.step(120);
    CHECK(show.playing() == Playing{{1, 5}});
    show.client.send(msg::redo());
    CHECK(show.client.next()["index"] == 0);
    CHECK(deckOrder() == std::vector<uint64_t>{2, 1});
    CHECK(show.harness.document().document().deck == 2);
    show.harness.step(120);
    CHECK(show.playing() == Playing{{1, 6}});
    CHECK(edit(show.client, azl::MoveDeck{2, 2})["type"] == "error");
}

TEST_CASE("the column Autopilot is an undoable composition edit, saved only when set and checked on the way in")
{
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    CHECK_FALSE(client.snapshot["composition"].contains("columnAutopilot"));
    const Message on = edit(client, azl::SetColumnAutopilot{AutopilotMode::Random, std::nullopt});
    CHECK(on["mode"] == "random");
    CHECK_FALSE(on.contains("bars"));
    CHECK(edit(client, azl::SetColumnAutopilot{std::nullopt, 8})["bars"] == 8);
    CHECK(Client(harness.document()).snapshot["composition"]["columnAutopilot"] ==
          Message{{"mode", "random"}, {"bars", 8}, {"hold", 16}});
    client.send(msg::undo());
    CHECK(client.next()["bars"] == 4);
    client.send(msg::undo());
    const Message off = client.next();
    CHECK(off["mode"] == "off");
    CHECK_FALSE(Client(harness.document()).snapshot["composition"].contains("columnAutopilot"));

    for (const int bars : {0, 65})
        CHECK(edit(client, azl::SetColumnAutopilot{std::nullopt, bars})["type"] == "error");
    for (const int bars : {-1, 65})
        CHECK(edit(client, azl::SetColumnAutopilot{std::nullopt, std::nullopt, bars})["type"] == "error");
    client.send({{"type", "command"}, {"op", "setColumnAutopilot"}, {"mode", "shuffle"}});
    CHECK(client.next()["type"] == "error");
}

TEST_CASE("the column Autopilot round-trips through a composition file; old and odd files load with it off")
{
    azl::Composition composition = azl::defaultComposition();
    composition.columnAutopilot = {AutopilotMode::Next, 16, 8};
    azl::CompositionFile file;
    file.composition = composition;
    std::vector<std::string> warnings;
    const azl::CompositionFile loaded = azl::compositionFileFromJson(azl::toJson(file), warnings);
    CHECK(loaded.composition.columnAutopilot == composition.columnAutopilot);
    CHECK(warnings.empty());

    nlohmann::json old = azl::toJson(azl::CompositionFile{.composition = azl::defaultComposition()});
    CHECK_FALSE(old["composition"].contains("columnAutopilot"));
    CHECK(azl::compositionFileFromJson(old, warnings).composition.columnAutopilot == azl::Autopilot{});
    CHECK(warnings.empty());

    for (const nlohmann::json& odd : {nlohmann::json{{"mode", "shuffle"}}, nlohmann::json{{"bars", 0}},
                                      nlohmann::json{{"hold", 99}}, nlohmann::json(3)}) {
        CAPTURE(odd.dump());
        nlohmann::json j = old;
        j["composition"]["columnAutopilot"] = odd;
        std::vector<std::string> odds;
        CHECK(azl::compositionFileFromJson(j, odds).composition.columnAutopilot == azl::Autopilot{});
        CHECK(odds.size() == 1);
    }
}

TEST_CASE("a resumed engine continues the column Autopilot's step on the column it played, a later trigger kept")
{
    TempDir media;
    TempDir library;
    azl::Composition saved;
    std::vector<azl::ResumePilot> pilots;
    {
        azl::EngineHarness harness(kSize, 60.0, library.path, twoLayers());
        Client client(harness.document());
        for (const uint64_t layer : {1, 2})
            for (const uint64_t column : {1, 2, 3}) {
                const fs::path path = media.path / (std::to_string(layer) + "-" + std::to_string(column) + ".mp4");
                const Message added = addMedia(client, copyMedia(kFiles[column - 1], path));
                REQUIRE(edit(client, azl::SetSlot{layer, column, ClipRef{added["entry"]["hash"], "clip", {}}})["op"] ==
                        "setSlot");
            }
        client.send({{"type", "trigger"}, {"op", "column"}, {"column", 1}});
        REQUIRE(client.next()["op"] == "triggerColumn");
        REQUIRE(edit(client, azl::SetColumnAutopilot{AutopilotMode::Next, 2})["op"] == "setColumnAutopilot");
        harness.step(250);
        // Layer 2 triggered after the step on beat 8: it lands on beat 12.
        client.send({{"type", "trigger"}, {"op", "clip"}, {"layer", 2}, {"column", 3}});
        REQUIRE(client.next()["op"] == "triggerClip");
        harness.step(110);
        pilots = harness.transport().pilots;
        saved = harness.document().document();
    }
    REQUIRE(pilots.size() == 1);
    CHECK(pilots[0] == azl::ResumePilot{0, std::nullopt, azl::Play{2, 8.0}, 12.0, 3, true});
    CHECK(azl::pilotsFrom(azl::pilotsJson(pilots)) == pilots);

    azl::EngineHarness harness(kSize, 60.0, library.path, saved);
    harness.resume(14.0, pilots);
    harness.step();
    std::map<uint64_t, uint64_t> playing;
    for (const auto& clip : harness.transport().clips)
        playing[clip.layer] = clip.column;
    CHECK(playing == Playing{{1, 2}, {2, 3}});
    CHECK(harness.transport().columns.held > 0);
    CHECK(harness.transport().columns.next == 3);
}
