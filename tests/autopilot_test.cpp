#include "clips.hpp"

#include <algorithm>
#include <vector>

namespace {

using azl::AutopilotMode;
using azl::ClipRef;

// At 120 BPM and 60 fps a bar is 120 render frames. count_709_tv and count_601_pc are a bar long (60 clip frames),
// count_100 two bars, count_240 four bars.
constexpr azl::EngineConfig kSize{.width = 960, .height = 540};

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

void setSlot(Client& client, uint64_t column, const std::string& hash, int step = 0)
{
    REQUIRE(edit(client, azl::SetSlot{1, column, ClipRef{hash, "clip", {}, step}})["op"] == "setSlot");
}

void triggerColumn(Client& client, uint64_t column)
{
    client.send({{"type", "trigger"}, {"op", "column"}, {"column", column}});
    REQUIRE(client.next()["op"] == "triggerColumn");
}

void autopilot(Client& client, AutopilotMode mode, int bars)
{
    REQUIRE(edit(client, azl::SetLayerAutopilot{1, mode, bars})["op"] == "setLayerAutopilot");
}

void pace(Client& client, double value)
{
    REQUIRE(edit(client, azl::SetAutopilotPace{value})["op"] == "setAutopilotPace");
}

void hold(Client& client, int bars)
{
    REQUIRE(edit(client, azl::SetLayerAutopilot{1, std::nullopt, std::nullopt, bars})["op"] == "setLayerAutopilot");
}

struct Status {
    uint64_t column = 0;
    uint64_t next = 0;
    int frame = 0;
    int held = 0;
};

// Layer 1 as of the last frame. Clip failures reported on the way are skipped.
Status status(Client& client)
{
    client.send({{"type", "clock"}, {"op", "state"}});
    Message state = client.next();
    while (state["type"] == "error")
        state = client.next();
    REQUIRE(state["op"] == "state");
    for (const Message& clip : state["clips"])
        if (clip["layer"] == 1)
            return {clip["column"], clip["next"], clip["frame"], clip["held"]};
    FAIL("layer 1 plays nothing");
    return {};
}

// The column layer 1 plays in each bar of `bars`, checking that it changes only on the first frame of a bar, where the
// new clip shows its first frame, and that it is the column named as next during the bar before.
std::vector<uint64_t> barSequence(uint32_t seed, int bars, double fps = 15.0)
{
    TempDir media;
    azl::EngineConfig config = kSize;
    config.seed = seed;
    azl::EngineHarness harness(config, fps);
    Client client(harness.document());
    const char* files[] = {"count_709_tv.mp4", "count_601_pc.mp4", "count_100.mp4", "count_240.mp4"};
    for (uint64_t column = 1; column <= 4; ++column)
        setSlot(client, column, add(client, copyMedia(files[column - 1], media.path / files[column - 1])));
    autopilot(client, AutopilotMode::Random, 1);
    triggerColumn(client, 1);

    const int perBar = int(fps * 2.0);
    std::vector<uint64_t> sequence;
    uint64_t announced = 0;
    for (int bar = 0; bar < bars; ++bar) {
        CAPTURE(bar);
        harness.step();
        const Status first = status(client);
        CHECK(first.frame == 0);
        if (bar > 0)
            CHECK(first.column == announced);
        sequence.push_back(first.column);
        for (int n = 1; n < perBar; ++n) {
            harness.step();
            const Status now = status(client);
            REQUIRE(now.column == first.column);
            announced = now.next;
        }
    }
    CHECK(harness.stats().videoMisses == 0);
    return sequence;
}

} // namespace

TEST_CASE("Autopilot next: a short clip loops until the step ends and a long one is cut at the step end")
{
    TempDir media;
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    setSlot(client, 1, add(client, copyMedia("count_240.mp4", media.path / "long.mp4")));
    setSlot(client, 2, add(client, copyMedia("count_709_tv.mp4", media.path / "short.mp4")));
    autopilot(client, AutopilotMode::Next, 2);
    triggerColumn(client, 1);

    for (int n = 0; n < 720; ++n) {
        CAPTURE(n);
        const int shown = frameAfterStep(harness);
        if (n < 240)
            REQUIRE(shown == n / 2);
        else if (n < 480)
            REQUIRE(shown == (n - 240) % 120 / 2);
        else
            REQUIRE(shown == (n - 480) / 2);
        if (n == 239 || n == 479)
            CHECK(status(client).next == (n == 239 ? 2 : 1));
    }
    CHECK(status(client).column == 1);
    CHECK(harness.stats().videoMisses == 0);
}

TEST_CASE("a clip's own step overrides the layer's; setting it back to 0 returns to the layer's")
{
    TempDir media;
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    setSlot(client, 1, add(client, copyMedia("count_100.mp4", media.path / "a.mp4")));
    setSlot(client, 2, add(client, copyMedia("count_709_tv.mp4", media.path / "b.mp4")), 3);
    setSlot(client, 3, add(client, copyMedia("count_601_pc.mp4", media.path / "c.mp4")));
    autopilot(client, AutopilotMode::Next, 1);
    triggerColumn(client, 1);

    // Column 1 for a bar, column 2 for three (its clip looping), column 3 for a bar, then column 1 again.
    const std::vector<std::pair<int, uint64_t>> spans{{120, 1}, {360, 2}, {120, 3}, {120, 1}};
    for (const auto& [frames, column] : spans) {
        CAPTURE(column);
        for (int n = 0; n < frames; ++n) {
            harness.step();
            const Status now = status(client);
            REQUIRE(now.column == column);
            if (column == 2)
                REQUIRE(now.frame == n % 120 / 2);
        }
    }

    const Message reset = edit(client, azl::SetClipStep{1, 2, 0});
    CHECK(reset["op"] == "setClipStep");
    CHECK(reset["bars"] == 0);
    harness.step(120);
    CHECK(status(client).column == 2);
    harness.step(120);
    CHECK(status(client).column == 3);
    CHECK(harness.stats().videoMisses == 0);
}

TEST_CASE("random Autopilot over 32 bars repeats exactly with its seed, never plays a clip twice in a row and misses "
          "no frame")
{
    const std::vector<uint64_t> sequence = barSequence(7, 32);
    CHECK(sequence == barSequence(7, 32));
    CHECK(sequence != barSequence(8, 32));
    for (size_t i = 1; i < sequence.size(); ++i)
        CHECK(sequence[i] != sequence[i - 1]);
    for (const uint64_t column : {1, 2, 3, 4})
        CHECK(std::count(sequence.begin(), sequence.end(), column) > 0);
    // The keyed draws are the same on every platform, so this pins the sequence on the laptop and the Mac mini alike.
    CHECK(sequence == std::vector<uint64_t>{1, 3, 2, 3, 1, 4, 3, 2, 4, 1, 4, 2, 1, 3, 2, 4,
                                           3, 2, 1, 3, 4, 2, 3, 1, 4, 1, 3, 2, 1, 3, 2, 4});
}

TEST_CASE("Autopilot skips a corrupt clip, and preloads the clip it picks instead")
{
    TempDir media;
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    setSlot(client, 1, add(client, copyMedia("count_709_tv.mp4", media.path / "a.mp4")));
    setSlot(client, 2, add(client, copyMedia("truncated.mp4", media.path / "corrupt.mp4")));
    setSlot(client, 3, add(client, copyMedia("count_601_pc.mp4", media.path / "c.mp4")));
    autopilot(client, AutopilotMode::Next, 1);
    triggerColumn(client, 1);

    for (const uint64_t column : {1, 3, 1, 3}) {
        for (int n = 0; n < 120; ++n) {
            harness.step();
            REQUIRE(status(client).column == column);
        }
    }
    CHECK(harness.stats().videoMisses == 0);
}

TEST_CASE("with no hold a trigger restarts the steps from its bar; switched off, Autopilot holds the clip it played")
{
    TempDir media;
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    const char* files[] = {"count_709_tv.mp4", "count_601_pc.mp4", "count_100.mp4"};
    for (uint64_t column = 1; column <= 3; ++column)
        setSlot(client, column, add(client, copyMedia(files[column - 1], media.path / files[column - 1])));
    autopilot(client, AutopilotMode::Next, 2);
    hold(client, 0);
    triggerColumn(client, 1);

    harness.step(300);
    CHECK(status(client).column == 2);
    // Given at bar 2.5, the trigger lands on bar 3, and the next step two bars later, on bar 5.
    triggerColumn(client, 3);
    harness.step(61);
    CHECK(status(client).column == 3);
    harness.step(239);
    CHECK(status(client).column == 3);
    harness.step();
    CHECK(status(client).column == 1);

    autopilot(client, AutopilotMode::Off, 2);
    harness.step(600);
    CHECK(status(client).column == 1);
    CHECK(status(client).next == 0);
    // Switched on again it steps from the bar it is in.
    autopilot(client, AutopilotMode::Next, 1);
    harness.step(120);
    CHECK(status(client).column == 2);
    CHECK(harness.stats().videoMisses == 0);
}

TEST_CASE("a trigger holds an Autopiloted layer for its hold, then Autopilot moves on from that clip on the bar")
{
    TempDir media;
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    const char* files[] = {"count_709_tv.mp4", "count_601_pc.mp4", "count_100.mp4"};
    for (uint64_t column = 1; column <= 3; ++column)
        setSlot(client, column, add(client, copyMedia(files[column - 1], media.path / files[column - 1])));
    autopilot(client, AutopilotMode::Next, 1);
    hold(client, 3);
    // The first trigger starts Autopilot: nothing to take over from.
    triggerColumn(client, 1);
    harness.step(130);
    CHECK(status(client).column == 2);
    CHECK(status(client).held == 0);

    // Given in bar 2, the trigger lands on bar 3 (beat 8) and holds column 1 until bar 6 (beat 20).
    triggerColumn(client, 1);
    harness.step(111);
    CHECK(status(client).column == 1);
    CHECK(status(client).held == 3);
    harness.step(358);
    CHECK(status(client).column == 1);
    CHECK(status(client).held == 1);
    CHECK(status(client).next == 2);
    harness.step(2);
    CHECK(status(client).column == 2);
    CHECK(status(client).held == 0);
    CHECK(status(client).frame == 0);
    // Autopilot's own steps never hold.
    harness.step(120);
    CHECK(status(client).column == 3);
    CHECK(status(client).held == 0);
    CHECK(harness.stats().videoMisses == 0);
}

TEST_CASE("a column Cue on an Autopiloted layer does not hold it")
{
    TempDir media;
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    const char* files[] = {"count_709_tv.mp4", "count_601_pc.mp4", "count_100.mp4"};
    for (uint64_t column = 1; column <= 3; ++column)
        setSlot(client, column, add(client, copyMedia(files[column - 1], media.path / files[column - 1])));
    autopilot(client, AutopilotMode::Next, 2);
    const azl::Cue drop{.section = azl::Section::Drop, .column = azl::CueColumn{3, std::nullopt}};
    REQUIRE(edit(client, azl::AddCue{std::nullopt, drop})["op"] == "addCue");
    triggerColumn(client, 1);
    client.send({{"type", "cue"}, {"op", "mark"}, {"section", "drop"}, {"bar", 3}});
    REQUIRE(client.next()["op"] == "setTimeline");

    // The Cue plays column 3 on bar 3 (beat 8), where Autopilot would have played 2; Autopilot moves on a step later.
    harness.step(241);
    CHECK(status(client).column == 3);
    CHECK(status(client).held == 0);
    harness.step(240);
    CHECK(status(client).column == 1);
    CHECK(status(client).held == 0);
}

TEST_CASE("the pace scales steps and the hold in whole bars of at least one")
{
    TempDir media;
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    const char* files[] = {"count_709_tv.mp4", "count_601_pc.mp4", "count_100.mp4"};
    for (uint64_t column = 1; column <= 3; ++column)
        setSlot(client, column, add(client, copyMedia(files[column - 1], media.path / files[column - 1])));
    autopilot(client, AutopilotMode::Next, 1);
    hold(client, 2);
    pace(client, 2.0);
    triggerColumn(client, 1);

    // Two-bar steps; a trigger given in bar 2 lands on bar 3 (beat 8) and holds column 1 for four bars, to beat 24.
    harness.step(130);
    CHECK(status(client).column == 1);
    triggerColumn(client, 1);
    harness.step(111);
    CHECK(status(client).column == 1);
    CHECK(status(client).held == 4);
    harness.step(470);
    CHECK(status(client).column == 1);
    CHECK(status(client).held == 1);
    harness.step(20);
    CHECK(status(client).column == 2);
    CHECK(status(client).held == 0);
    harness.step(220);
    CHECK(status(client).column == 2);
    harness.step(20);
    CHECK(status(client).column == 3);

    // Half a one-bar step is still a bar.
    pace(client, 0.5);
    harness.step(240);
    CHECK(status(client).column == 1);
    harness.step(120);
    CHECK(status(client).column == 2);
    CHECK(harness.stats().videoMisses == 0);
}

TEST_CASE("a pace change rescales the running step from its progress without restarting the clip")
{
    TempDir media;
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    setSlot(client, 1, add(client, copyMedia("count_240.mp4", media.path / "long.mp4")));
    setSlot(client, 2, add(client, copyMedia("count_709_tv.mp4", media.path / "short.mp4")));
    autopilot(client, AutopilotMode::Next, 2);
    triggerColumn(client, 1);

    // Half way through a two-bar step, x2 leaves half of a four-bar step: it ends on beat 12, not 8, 16 or 20.
    harness.step(120);
    const int before = status(client).frame;
    pace(client, 2.0);
    harness.step(2);
    CHECK(status(client).column == 1);
    CHECK(status(client).frame == before + 1);
    harness.step(228);
    CHECK(status(client).column == 1);
    harness.step(20);
    CHECK(status(client).column == 2);
    CHECK(status(client).frame < 10);
    // The next step is a whole four bars.
    harness.step(460);
    CHECK(status(client).column == 2);
    harness.step(20);
    CHECK(status(client).column == 1);
    CHECK(harness.stats().videoMisses == 0);
}

TEST_CASE("the pace is an undoable composition edit, saved only off x1 and checked on the way in")
{
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    CHECK_FALSE(client.snapshot["composition"].contains("pace"));
    CHECK(edit(client, azl::SetAutopilotPace{4.0})["pace"] == 4.0);
    CHECK(Client(harness.document()).snapshot["composition"]["pace"] == 4.0);
    client.send(msg::undo());
    CHECK(client.next()["pace"] == 1.0);
    CHECK_FALSE(Client(harness.document()).snapshot["composition"].contains("pace"));
    for (const double bad : {0.0, 3.0, 16.0})
        CHECK(edit(client, azl::SetAutopilotPace{bad})["type"] == "error");
}

TEST_CASE("Autopilot settings are undoable edits on layers and clips, checked on the way in")
{
    TempDir media;
    azl::Composition withGroup = azl::defaultComposition();
    withGroup.layers.push_back({.id = 2, .name = "Group", .group = true});
    azl::EngineHarness harness(kSize, 60.0, {}, withGroup);
    Client client(harness.document());
    CHECK(client.snapshot["composition"]["layers"][0]["autopilot"] == Message{{"mode", "off"}, {"bars", 4}});

    const Message on = edit(client, azl::SetLayerAutopilot{1, AutopilotMode::Random, std::nullopt});
    CHECK(on["mode"] == "random");
    CHECK_FALSE(on.contains("bars"));
    CHECK(edit(client, azl::SetLayerAutopilot{1, std::nullopt, 8})["bars"] == 8);
    Client late(harness.document());
    CHECK(late.snapshot["composition"]["layers"][0]["autopilot"] == Message{{"mode", "random"}, {"bars", 8}});
    client.send(msg::undo());
    CHECK(client.next()["bars"] == 4);
    CHECK(edit(client, azl::SetLayerAutopilot{1, std::nullopt, std::nullopt, 0})["hold"] == 0);
    CHECK(Client(harness.document()).snapshot["composition"]["layers"][0]["autopilot"]["hold"] == 0);
    client.send(msg::undo());
    CHECK(client.next()["hold"] == 16);
    CHECK_FALSE(Client(harness.document()).snapshot["composition"]["layers"][0]["autopilot"].contains("hold"));

    for (const int bars : {0, 65})
        CHECK(edit(client, azl::SetLayerAutopilot{1, std::nullopt, bars})["type"] == "error");
    for (const int bars : {-1, 65})
        CHECK(edit(client, azl::SetLayerAutopilot{1, std::nullopt, std::nullopt, bars})["type"] == "error");
    CHECK(edit(client, azl::SetLayerAutopilot{2, AutopilotMode::Next, 4})["type"] == "error");
    client.send({{"type", "command"}, {"op", "setLayerAutopilot"}, {"layer", 1}, {"mode", "shuffle"}});
    CHECK(client.next()["type"] == "error");

    const std::string a = add(client, copyMedia("count_709_tv.mp4", media.path / "a.mp4"));
    CHECK(edit(client, azl::SetClipStep{1, 1, 2})["type"] == "error");
    setSlot(client, 1, a);
    CHECK(edit(client, azl::SetClipStep{1, 1, 65})["type"] == "error");
    CHECK(edit(client, azl::SetClipStep{1, 1, 2})["bars"] == 2);
    Client later(harness.document());
    CHECK(later.snapshot["composition"]["layers"][0]["clips"]["1"]["step"] == 2);
    client.send(msg::undo());
    CHECK(client.next()["bars"] == 0);
}
