#include "document/cue_edits.hpp"
#include "engine/autopilot_stepper.hpp"
#include "net/websocket.hpp"
#include "test_client.hpp"
#include "test_paths.hpp"

#include <cstdlib>
#include <fstream>
#include <vector>

namespace {

using azl::AutopilotMode;
using azl::ClipRef;
using azl::Section;

// At 120 BPM and 15 fps a beat is 7.5 frames and a bar 30.
constexpr azl::EngineConfig kSize{.width = 320, .height = 180};
constexpr int kPerBar = 30;

Message cueMessage(Client& client, Message m)
{
    m["type"] = "cue";
    client.send(m);
    return client.next();
}

Message mark(Client& client, const char* section, int bar)
{
    const Message reply = cueMessage(client, {{"op", "mark"}, {"section", section}, {"bar", bar}});
    REQUIRE_MESSAGE(reply["op"] == "setTimeline", reply.dump());
    CHECK(reply["cause"] == "cue");
    return reply;
}

Message cueState(Client& client)
{
    const Message reply = cueMessage(client, {{"op", "state"}});
    REQUIRE_MESSAGE(reply["op"] == "state", reply.dump());
    return reply;
}

// Four clips in columns 1-4 of layer 1.
void fillColumns(Client& client, const fs::path& dir)
{
    const char* files[] = {"count_709_tv.mp4", "count_601_pc.mp4", "count_100.mp4", "count_240.mp4"};
    for (uint64_t column = 1; column <= 4; ++column) {
        const Message added = addMedia(client, copyMedia(files[column - 1], dir / files[column - 1]));
        REQUIRE(added["type"] == "library");
        apply(client, azl::SetSlot{1, column, ClipRef{added["entry"]["hash"], "clip", {}}});
    }
}

void trigger(Client& client, Message m, const char* op)
{
    m["type"] = "trigger";
    client.send(m);
    const Message reply = client.next();
    REQUIRE_MESSAGE(reply["op"] == op, reply.dump());
}

uint64_t column(Client& client)
{
    client.send({{"type", "clock"}, {"op", "state"}});
    const Message state = client.next();
    REQUIRE(state["op"] == "state");
    for (const Message& clip : state["clips"])
        if (clip["layer"] == 1)
            return clip["column"];
    FAIL("layer 1 plays nothing");
    return 0;
}

double macroValue(Client& client, uint64_t macro)
{
    client.send({{"type", "modulation"}, {"op", "state"}});
    const Message state = client.next();
    REQUIRE(state["op"] == "state");
    for (const Message& m : state["macros"])
        if (m["id"] == macro)
            return m["value"];
    FAIL("no macro " << macro);
    return 0.0;
}

uint64_t addEnergy(Client& client)
{
    return apply(client, azl::AddMacro{std::nullopt, {.name = "Energy"}})["macro"]["id"];
}

azl::Cue dropCue(uint64_t macro)
{
    return {.name = "Drop", .section = Section::Drop, .autopilot = {{1, std::nullopt, 1}}, .macros = {{macro, 1.0}}};
}

} // namespace

TEST_CASE("a drop Cue changes Autopilot's step and a macro on the drop's beat, and both return when the drop ends")
{
    TempDir media;
    azl::EngineHarness harness(kSize, 15.0);
    Client client(harness.document());
    fillColumns(client, media.path);
    apply(client, azl::SetLayerAutopilot{1, AutopilotMode::Next, 4});
    const uint64_t energy = addEnergy(client);
    const uint64_t drop = apply(client, azl::AddCue{std::nullopt, dropCue(energy)})["cue"]["id"];
    trigger(client, {{"op", "column"}, {"column", 1}}, "triggerColumn");
    mark(client, "drop", 11);
    mark(client, "breakdown", 15);

    // Steps of 4 bars from bar 0; the drop at bar 10 (beat 40) steps every bar from its own beat, and the breakdown at
    // bar 14 goes back to 4 bars from the last step, at bar 13.
    const uint64_t expected[] = {1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 4, 1, 2, 3, 3, 3, 3, 4};
    for (int bar = 0; bar < 18; ++bar) {
        CAPTURE(bar);
        const double energized = bar >= 10 && bar < 14 ? 1.0 : 0.0;
        for (int n = 0; n < kPerBar; ++n) {
            harness.step();
            REQUIRE(column(client) == expected[bar]);
            if (n == 0 || n == kPerBar - 1)
                REQUIRE(macroValue(client, energy) == energized);
        }
        if (bar == 12) {
            const Message state = cueState(client);
            CHECK(state["section"] == "drop");
            CHECK(state["start"] == 40.0);
            CHECK(state["end"] == 56.0);
            REQUIRE(state["active"].size() == 1);
            CHECK(state["active"][0]["cue"] == drop);
            CHECK(state["active"][0]["beat"] == 40.0);
            CHECK(state["autopilot"] == Message::parse(R"([{"layer": 1, "mode": "next", "bars": 1}])"));
        }
    }
    const Message after = cueState(client);
    CHECK(after["section"] == "breakdown");
    CHECK(after["active"].empty());
    CHECK(after["autopilot"].empty());

    // Firing changed nothing in the document: undo takes back the last edit, the Cue.
    client.send(msg::snapshotRequest());
    const Message document = client.next()["composition"];
    CHECK(document["layers"][0]["autopilot"]["bars"] == 4);
    CHECK(document["macros"][0]["value"] == 0.0);
    client.send(msg::undo());
    CHECK(client.next()["op"] == "removeCue");
    CHECK(harness.stats().videoMisses == 0);
}

TEST_CASE("every engine holding the timeline fires a Cue on the same beat, whatever its frame rate")
{
    azl::Composition show = azl::defaultComposition();
    show.macros.push_back({.id = 1, .name = "Energy"});
    show.cues.push_back(dropCue(1));
    show.cues.back().autopilot.clear();
    show.cues.push_back({.id = 2, .name = "Late", .section = Section::Drop, .bar = 3, .macros = {{1, 0.5}}});
    show.timeline.sections = {{Section::Intro, 0.0}, {Section::Drop, 18.0}, {Section::Outro, 30.0}};

    for (const double fps : {15.0, 24.0, 60.0}) {
        CAPTURE(fps);
        azl::EngineHarness harness(kSize, fps);
        Client client(harness.document());
        apply(client, azl::LoadComposition{show});
        for (int frame = 0; frame < int(fps * 18); ++frame) {
            harness.step();
            const double beat = frame / fps * 2.0;
            const double value = beat >= 26.0 && beat < 30.0 ? 0.5 : beat >= 18.0 && beat < 30.0 ? 1.0 : 0.0;
            CAPTURE(beat);
            REQUIRE(macroValue(client, 1) == value);
        }
    }
}

TEST_CASE("cancel takes back a queued trigger, the scheduled Sections and a Cue's next firing")
{
    TempDir media;
    azl::EngineHarness harness(kSize, 15.0);
    Client client(harness.document());
    fillColumns(client, media.path);
    const uint64_t energy = addEnergy(client);
    apply(client, azl::AddCue{std::nullopt, dropCue(energy)});
    const azl::Cue atBar4{.name = "Bar 4", .bar = 4, .macros = {{energy, 0.5}}};
    const uint64_t bar4 = apply(client, azl::AddCue{std::nullopt, atBar4})["cue"]["id"];
    trigger(client, {{"op", "column"}, {"column", 1}}, "triggerColumn");
    harness.step(15);
    REQUIRE(column(client) == 1);

    trigger(client, {{"op", "clip"}, {"layer", 1}, {"column", 2}}, "triggerClip");
    trigger(client, {{"op", "cancel"}, {"layer", 1}}, "cancelTrigger");
    harness.step(30);
    CHECK(column(client) == 1);
    client.send({{"type", "trigger"}, {"op", "cancel"}});
    CHECK(client.next()["type"] == "error");

    trigger(client, {{"op", "column"}, {"column", 3}}, "triggerColumn");
    client.send({{"type", "trigger"}, {"op", "cancel"}, {"column", 2}});
    CHECK(client.next()["type"] == "error");
    trigger(client, {{"op", "cancel"}, {"column", 3}}, "cancelTrigger");
    harness.step(30);
    CHECK(column(client) == 1);

    mark(client, "drop", 4);
    mark(client, "outro", 9);
    CHECK(cueMessage(client, {{"op", "cancel"}, {"bar", 5}})["type"] == "error");
    CHECK(cueMessage(client, {{"op", "cancel"}, {"bar", 9}})["op"] == "setTimeline");
    Message state = cueState(client);
    REQUIRE(state["sections"].size() == 1);
    REQUIRE(state["upcoming"].size() == 2);
    CHECK(cueMessage(client, {{"op", "cancel"}})["op"] == "setTimeline");
    CHECK(cueMessage(client, {{"op", "cancel"}, {"cue", bar4}})["op"] == "setTimeline");
    state = cueState(client);
    CHECK(state["upcoming"].empty());
    CHECK(state["sections"].empty());
    CHECK(cueMessage(client, {{"op", "cancel"}})["type"] == "error");
    CHECK(cueMessage(client, {{"op", "cancel"}, {"cue", bar4}})["type"] == "error");
    harness.step(4 * kPerBar);
    CHECK(macroValue(client, energy) == 0.0);
    CHECK(cueState(client)["active"].empty());
}

TEST_CASE("Cues are undoable edits saved in composition files; the timeline is playback and is not saved")
{
    TempDir dir;
    azl::EngineHarness harness(kSize, 15.0);
    Client client(harness.document());
    const uint64_t energy = addEnergy(client);

    CHECK(command(client, azl::AddCue{std::nullopt, {.autopilot = {{9, AutopilotMode::Next, {}}}}})["type"] == "error");
    CHECK(command(client, azl::AddCue{std::nullopt, {.autopilot = {{1, {}, {}}}}})["type"] == "error");
    CHECK(command(client, azl::AddCue{std::nullopt, {.bar = 0}})["type"] == "error");
    CHECK(command(client, azl::AddCue{std::nullopt, {.macros = {{energy, 2.0}}}})["type"] == "error");
    CHECK(command(client, azl::AddCue{std::nullopt, {.macros = {{energy + 1, 1.0}}}})["type"] == "error");

    azl::Cue drop = dropCue(energy);
    drop.autopilot = {{1, AutopilotMode::Random, 2}};
    const Message added = apply(client, azl::AddCue{std::nullopt, drop});
    drop.id = added["cue"]["id"];
    apply(client, azl::AddCue{std::nullopt, {.name = "Outro at bar 64", .bar = 64}});
    client.send(msg::undo());
    CHECK(client.next()["op"] == "removeCue");
    client.send(msg::redo());
    CHECK(client.next()["op"] == "addCue");
    drop.name = "Big drop";
    apply(client, azl::SetCue{drop});
    mark(client, "drop", 3);
    client.send(msg::undo());
    CHECK(client.next()["op"] == "setCue");

    client.send(msg::snapshotRequest());
    const Message saved = client.next()["composition"];
    REQUIRE(saved["cues"].size() == 2);
    CHECK(saved["cues"][0]["name"] == "Drop");
    CHECK(saved["timeline"]["sections"].size() == 1);

    client.send({{"type", "composition"}, {"op", "save"}, {"path", (dir.path / "show.json").string()}});
    REQUIRE(client.next()["type"] == "composition");
    std::ifstream in(dir.path / "show.json");
    const Message file = Message::parse(in);
    CHECK(file["composition"]["cues"] == saved["cues"]);
    CHECK(!file["composition"].contains("timeline"));

    azl::EngineHarness other(kSize, 15.0);
    Client loader(other.document());
    loader.send({{"type", "composition"}, {"op", "load"}, {"path", (dir.path / "show.json").string()}});
    REQUIRE(loader.next()["type"] == "composition");
    REQUIRE(loader.next()["op"] == "loadComposition");
    loader.send(msg::snapshotRequest());
    const Message loaded = loader.next()["composition"];
    CHECK(loaded["cues"] == saved["cues"]);
    CHECK(loaded["macros"] == saved["macros"]);

    const azl::Composition back = Message(harness.document().document()).get<azl::Composition>();
    CHECK(back.cues == harness.document().document().cues);
    CHECK(back.timeline == harness.document().document().timeline);
}

TEST_CASE("a resync restarts the Section playing on beat 0 and drops the scheduled ones")
{
    azl::EngineHarness harness(kSize, 15.0);
    Client client(harness.document());
    mark(client, "build", 2);
    mark(client, "drop", 9);
    harness.step(46);
    CHECK(cueState(client)["section"] == "build");

    client.send({{"type", "clock"}, {"op", "resync"}});
    const Message change = client.next();
    CHECK(change["cause"] == "resync");
    CHECK(change["timeline"]["sections"] == Message::parse(R"([{"section": "build", "beat": 0.0}])"));
    CHECK(client.next()["op"] == "resync");
    harness.step();
    const Message state = cueState(client);
    CHECK(state["section"] == "build");
    CHECK(state["start"] == 0.0);
}

TEST_CASE("Cues fire at a bar of their Section or of the beat clock and hold until their Section ends")
{
    azl::Composition c = azl::defaultComposition();
    c.cues = {{.id = 1, .section = Section::Drop, .autopilot = {{1, AutopilotMode::Next, 2}}},
              {.id = 2, .section = Section::Drop, .bar = 2, .autopilot = {{1, {}, 1}}},
              {.id = 3, .bar = 5, .macros = {{7, 0.25}}},
              {.id = 4, .section = Section::Drop, .bar = 9}};
    c.timeline.sections = {{Section::Intro, 0.0}, {Section::Drop, 8.0}, {Section::Outro, 24.0}};
    azl::CueState state;

    azl::cueStateAt(c, 7.9, state);
    CHECK(state.section == Section::Intro);
    CHECK(state.active.empty());
    azl::cueStateAt(c, 8.0, state);
    CHECK(state.active == std::vector<azl::CueFiring>{{1, 8.0, 24.0}});
    CHECK(azl::cuedAutopilot(c.layers[0], 0, state.autopilot) == azl::Autopilot{AutopilotMode::Next, 2});
    azl::cueStateAt(c, 16.0, state);
    CHECK(state.active == std::vector<azl::CueFiring>{{1, 8.0, 24.0}, {2, 12.0, 24.0}, {3, 16.0, 24.0}});
    CHECK(azl::cuedAutopilot(c.layers[0], 0, state.autopilot) == azl::Autopilot{AutopilotMode::Next, 1});
    CHECK(state.macros == std::vector<azl::CueMacro>{{7, 0.25}});
    azl::cueStateAt(c, 24.0, state);
    CHECK(state.section == Section::Outro);
    CHECK(state.active.empty());
    CHECK(azl::cuedAutopilot(c.layers[0], 0, state.autopilot) == c.layers[0].autopilot);

    CHECK(azl::upcomingCues(c, 4.0) ==
          std::vector<azl::CueFiring>{{1, 8.0, 24.0}, {2, 12.0, 24.0}, {3, 16.0, 24.0}});
    c.timeline.skips = {{2, 12.0}};
    CHECK(azl::upcomingCues(c, 8.0) == std::vector<azl::CueFiring>{{3, 16.0, 24.0}});

    const azl::Timeline marked = azl::markSection(c.timeline, Section::Breakdown, 28.0, 25.0);
    CHECK(marked.sections == std::vector<azl::SectionMark>{{Section::Outro, 24.0}, {Section::Breakdown, 28.0}});
    CHECK(marked.skips.empty());

    azl::Edit unordered = azl::SetTimeline{{{{Section::Drop, 8.0}, {Section::Intro, 4.0}}, {}}};
    CHECK_THROWS_AS(azl::applyEdit(c, unordered), azl::ProtocolError);
}

TEST_CASE("a Cue switched off or repeating is saved only then, and both are undoable edits")
{
    const azl::Cue plain{.id = 1, .section = Section::Drop};
    const Message old = plain;
    CHECK(!old.contains("enabled"));
    CHECK(!old.contains("every"));
    CHECK(old.get<azl::Cue>() == plain);
    const azl::Cue changed{.id = 1, .section = Section::Drop, .enabled = false, .every = 16};
    const Message saved = changed;
    CHECK(saved["enabled"] == false);
    CHECK(saved["every"] == 16);
    CHECK(saved.get<azl::Cue>() == changed);

    azl::EngineHarness harness(kSize, 15.0);
    Client client(harness.document());
    const uint64_t cue = apply(client, azl::AddCue{std::nullopt, {.section = Section::Drop}})["cue"]["id"];
    CHECK(command(client, azl::SetCueRepeat{cue, 0})["type"] == "error");
    CHECK(command(client, azl::SetCueEnabled{cue + 1, false})["type"] == "error");
    CHECK(command(client, azl::AddCue{std::nullopt, {.every = 0}})["type"] == "error");

    apply(client, azl::SetCueEnabled{cue, false});
    apply(client, azl::SetCueRepeat{cue, 16});
    CHECK(harness.document().document().cues[0] == azl::Cue{.id = cue, .section = Section::Drop, .enabled = false, .every = 16});
    client.send(msg::undo());
    CHECK(client.next()["op"] == "setCueRepeat");
    CHECK(!harness.document().document().cues[0].every);
    client.send(msg::undo());
    CHECK(client.next()["op"] == "setCueEnabled");
    CHECK(harness.document().document().cues[0].enabled);
    client.send(msg::redo());
    CHECK(client.next()["op"] == "setCueEnabled");
    client.send(msg::redo());
    CHECK(client.next()["op"] == "setCueRepeat");
    CHECK(harness.document().document().cues[0] == azl::Cue{.id = cue, .section = Section::Drop, .enabled = false, .every = 16});

    const Message back = Message(harness.document().document());
    CHECK(back["cues"][0]["enabled"] == false);
    CHECK(back["cues"][0]["every"] == 16);
    CHECK(back.get<azl::Composition>().cues == harness.document().document().cues);
}

TEST_CASE("a Cue switched off never fires; a repeating one fires every N bars from its first firing")
{
    azl::Composition c = azl::defaultComposition();
    c.cues = {{.id = 1, .section = Section::Drop, .bar = 2, .macros = {{7, 0.5}}, .every = 4},
              {.id = 2, .bar = 3, .every = 2},
              {.id = 3, .section = Section::Drop, .macros = {{7, 1.0}}, .enabled = false}};
    c.timeline.sections = {{Section::Intro, 0.0}, {Section::Drop, 32.0}, {Section::Outro, 96.0}};
    azl::CueState state;

    azl::cueStateAt(c, 7.9, state);
    CHECK(state.active.empty());
    azl::cueStateAt(c, 8.0, state);
    CHECK(state.active == std::vector<azl::CueFiring>{{2, 8.0, 32.0}});
    azl::cueStateAt(c, 31.0, state);
    CHECK(state.active == std::vector<azl::CueFiring>{{2, 24.0, 32.0}});
    azl::cueStateAt(c, 36.0, state);
    CHECK(state.active == std::vector<azl::CueFiring>{{2, 32.0, 96.0}, {1, 36.0, 96.0}});
    CHECK(state.macros == std::vector<azl::CueMacro>{{7, 0.5}});
    azl::cueStateAt(c, 53.0, state);
    CHECK(state.active == std::vector<azl::CueFiring>{{2, 48.0, 96.0}, {1, 52.0, 96.0}});
    azl::cueStateAt(c, 96.0, state);
    CHECK(state.active == std::vector<azl::CueFiring>{{2, 96.0, azl::kNever}});

    CHECK(azl::upcomingCues(c, 30.0) ==
          std::vector<azl::CueFiring>{{2, 32.0, 96.0}, {1, 36.0, 96.0}, {2, 96.0, azl::kNever}});
    CHECK(azl::upcomingCues(c, 36.0) ==
          std::vector<azl::CueFiring>{{2, 40.0, 96.0}, {1, 52.0, 96.0}, {2, 96.0, azl::kNever}});
    c.timeline.skips = {{1, 52.0}};
    CHECK(azl::upcomingCues(c, 36.0)[1] == azl::CueFiring{1, 68.0, 96.0});
    azl::cueStateAt(c, 60.0, state);
    CHECK(state.active[0] == azl::CueFiring{1, 36.0, 96.0});

    c.cues[2].enabled = true;
    azl::cueStateAt(c, 32.0, state);
    CHECK(state.active == std::vector<azl::CueFiring>{{2, 32.0, 96.0}, {3, 32.0, 96.0}});
}

TEST_CASE("at a fake clock a repeating column Cue fires on the bar every N bars, and one switched off never does")
{
    azl::Composition c;
    c.decks = {{.id = 1, .name = "Deck", .columns = {{1, "1"}, {2, "2"}, {3, "3"}}}};
    c.deck = 1;
    azl::Layer layer{.id = 10, .name = "layer"};
    for (const uint64_t column : {1, 2, 3})
        layer.clips[column].hash = "clip" + std::to_string(column);
    c.layers = {layer};
    c.cues = {{.id = 1, .bar = 2, .column = azl::CueColumn{3, std::nullopt}, .every = 4},
              {.id = 2, .bar = 3, .column = azl::CueColumn{2, std::nullopt}, .enabled = false}};

    azl::BeatClock clock;
    azl::CueState cues;
    std::atomic<uint32_t> seed{7};
    azl::AutopilotStepper stepper{clock, cues, seed, [](const azl::Layer&, uint64_t column) { return column != 0; }};
    std::vector<azl::Play> plays;
    for (double beat = 0.0; beat < 60.0; beat += 1.0 / 7.5) {
        azl::cueStateAt(c, beat, cues);
        stepper.fireColumnCues(c, beat);
        const std::optional<azl::Play> play = stepper.playOf(c.layers[0]);
        if (play && (plays.empty() || plays.back() != *play))
            plays.push_back(*play);
    }
    CHECK(plays == std::vector<azl::Play>{{3, 4.0}, {3, 20.0}, {3, 36.0}, {3, 52.0}});
}

TEST_CASE("nocturne-ctl switches a Cue off and on and sets its repeat")
{
    azl::EngineHarness harness(kSize, 15.0);
    Client client(harness.document());
    const uint64_t cue = apply(client, azl::AddCue{std::nullopt, {.section = Section::Drop}})["cue"]["id"];
    azl::WebSocketServer server(harness.document(), 0);
    auto ctl = [&](const std::string& args) {
        const fs::path binary = ctlPath();
        const std::string command =
            "'" + binary.string() + "' --port " + std::to_string(server.port()) + " " + args + " >/dev/null 2>&1";
        const int status = std::system(command.c_str());
        return WEXITSTATUS(status);
    };
    const std::string id = std::to_string(cue);
    CHECK(ctl("cue off " + id) == 0);
    CHECK(ctl("cue every " + id + " 16") == 0);
    CHECK(harness.document().document().cues[0].enabled == false);
    CHECK(harness.document().document().cues[0].every == 16);
    CHECK(ctl("cue on " + id) == 0);
    CHECK(ctl("cue every " + id + " once") == 0);
    CHECK(harness.document().document().cues[0] == azl::Cue{.id = cue, .section = Section::Drop});
    CHECK(ctl("cue every " + id + " 0") != 0);
    CHECK(ctl("cue off") == 2);
}
