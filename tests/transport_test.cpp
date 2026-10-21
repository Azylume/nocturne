#include "clips.hpp"

#include <algorithm>
#include <cmath>

namespace {

using azl::PlayMode;
using azl::SyncMode;

constexpr azl::EngineConfig kSize{.width = 960, .height = 540};

Message edit(Client& client, const azl::Edit& e)
{
    client.send(msg::command(e));
    return client.next();
}

Message setTempo(Client& client, double bpm)
{
    return edit(client, azl::SetTempo{bpm});
}

Message clock(Client& client, Message request)
{
    request["type"] = "clock";
    client.send(request);
    return client.next();
}

Message clockState(Client& client)
{
    const Message state = clock(client, {{"op", "state"}});
    REQUIRE(state["op"] == "state");
    return state;
}

Message nudge(Client& client, double percent)
{
    return clock(client, {{"op", "nudge"}, {"percent", percent}});
}

// The frames shown by the next `count` rendered frames.
std::vector<int> nextFrames(azl::EngineHarness& harness, int count)
{
    std::vector<int> frames;
    for (int i = 0; i < count; ++i)
        frames.push_back(frameAfterStep(harness));
    return frames;
}

// Nothing drawn: the composition's black where the clip's bar code and colour patch would be.
bool blank(const azl::Image& image)
{
    for (const auto [x, y] : {std::pair{60u, 135u}, {240u, 405u}, {720u, 405u}})
        if (pixel(image, x, y)[0] > 4 || pixel(image, x, y)[1] > 4 || pixel(image, x, y)[2] > 4)
            return false;
    return true;
}

azl::Transport timeline(PlayMode mode)
{
    return {SyncMode::Timeline, mode, 0};
}

} // namespace

TEST_CASE("a BPM-synced clip picks its beat count and is exactly where N beats put it")
{
    TempDir media;
    azl::EngineHarness harness(kSize);
    Client client(harness.document());

    SUBCASE("2 s picks 4 beats; at 150 BPM a beat is 24 render frames and 15 clip frames")
    {
        REQUIRE(setTempo(client, 150)["bpm"] == 150.0);
        playMedia(client, copyMedia("count_709_tv.mp4", media.path / "count.mp4"));
        CHECK(frameAfterStep(harness) == 0);
        CHECK(clockState(client)["clips"][0]["beats"] == 4);
        int rendered = 1;
        for (const int beats : {1, 2, 3, 5, 7, 8, 13}) {
            CHECK(frameAfterStep(harness, 24 * beats + 1 - rendered) == (15 * beats) % 60);
            rendered = 24 * beats + 1;
        }
    }

    SUBCASE("3.33 s picks 8 beats (144 BPM); at 120 BPM 4 beats are half the clip")
    {
        playMedia(client, copyMedia("count_100.mp4", media.path / "count100.mp4"));
        CHECK(frameAfterStep(harness) == 0);
        CHECK(clockState(client)["clips"][0]["beats"] == 8);
        CHECK(frameAfterStep(harness, 120) == 50);
        CHECK(frameAfterStep(harness, 60) == 75);
        CHECK(frameAfterStep(harness, 120) == 25);
    }
}

TEST_CASE("a tempo change mid-clip keeps the clip's position continuous")
{
    TempDir media;
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    playMedia(client, copyMedia("count_709_tv.mp4", media.path / "count.mp4"));

    // 120 BPM: a 4-beat clip at its native 30 fps, half a clip frame per render frame.
    CHECK(frameAfterStep(harness, 30) == 14);
    setTempo(client, 60);
    CHECK(frameAfterStep(harness) == 15);
    CHECK(frameAfterStep(harness, 4) == 16);
    CHECK(frameAfterStep(harness, 56) == 30);
    setTempo(client, 240);
    CHECK(frameAfterStep(harness) == 30);
    CHECK(frameAfterStep(harness, 15) == 45);
    CHECK(clockState(client)["beat"].get<double>() == doctest::Approx(3.0 + 1.0 / 60.0));
}

TEST_CASE("tap tempo takes the mean interval of a run; a two-second pause starts a new run")
{
    azl::EngineHarness harness(kSize);
    Client client(harness.document());

    CHECK(clock(client, {{"op", "tap"}})["bpm"].is_null());
    harness.step(30);
    client.send({{"type", "clock"}, {"op", "tap"}});
    const Message change = client.next();
    CHECK(change["op"] == "setTempo");
    CHECK(change["cause"] == "tap");
    CHECK(change["bpm"].get<double>() == doctest::Approx(120.0));
    CHECK(client.next()["bpm"].get<double>() == doctest::Approx(120.0));

    // Taps at 0, 0.5, 0.9 and 1.4 s: three intervals in 1.4 s, however uneven.
    harness.step(24);
    client.send({{"type", "clock"}, {"op", "tap"}});
    CHECK(client.next()["bpm"].get<double>() == doctest::Approx(60.0 * 2 / 0.9));
    client.next();
    harness.step(30);
    client.send({{"type", "clock"}, {"op", "tap"}});
    CHECK(client.next()["bpm"].get<double>() == doctest::Approx(60.0 * 3 / 1.4));
    client.next();

    harness.step(150);
    CHECK(clock(client, {{"op", "tap"}})["bpm"].is_null());
    harness.step(25);
    client.send({{"type", "clock"}, {"op", "tap"}});
    CHECK(client.next()["bpm"].get<double>() == doctest::Approx(144.0));
    client.next();

    harness.step();
    CHECK(clockState(client)["bpm"].get<double>() == doctest::Approx(144.0));
    client.send(msg::undo());
    CHECK(client.next()["type"] == "error");
}

TEST_CASE("a nudge bends the tempo while held and leaves the phase moved")
{
    TempDir media;
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    playMedia(client, copyMedia("count_709_tv.mp4", media.path / "count.mp4"));
    harness.step(30);

    // +10% for one second at 120 BPM gains 0.2 beats: 3 clip frames of a 4-beat, 60-frame clip.
    CHECK(nudge(client, 10)["percent"] == 10.0);
    harness.step(60);
    Message state = clockState(client);
    CHECK(state["tempo"].get<double>() == doctest::Approx(132.0));
    CHECK(state["bpm"] == 120.0);
    CHECK(state["nudge"] == 10.0);
    nudge(client, 0);
    CHECK(frameAfterStep(harness) == 48);
    CHECK(frameAfterStep(harness, 30) == 3);
    state = clockState(client);
    CHECK(state["tempo"] == 120.0);
    CHECK(state["beat"].get<double>() == doctest::Approx(4.2));

    nudge(client, -50);
    harness.step(60);
    nudge(client, 0);
    CHECK(clockState(client)["beat"].get<double>() == doctest::Approx(4.2 + 61.0 / 60.0));

    CHECK(nudge(client, 51)["type"] == "error");
}

TEST_CASE("a nudge ends when the client holding it disconnects; another client's nudge stays")
{
    azl::EngineHarness harness(kSize);
    Client stays(harness.document());
    {
        Client quits(harness.document());
        CHECK(nudge(quits, 10)["percent"] == 10.0);
        CHECK(stays.next()["percent"] == 10.0);
        harness.step(10);
    }
    const Message ended = stays.next();
    CHECK(ended["op"] == "nudge");
    CHECK(ended["percent"] == 0.0);
    harness.step();
    CHECK(clockState(stays)["nudge"] == 0.0);
    CHECK(clockState(stays)["tempo"] == 120.0);

    {
        Client quits(harness.document());
        nudge(quits, 10);
        CHECK(stays.next()["percent"] == 10.0);
        CHECK(nudge(stays, -20)["percent"] == -20.0);
        CHECK(quits.next()["percent"] == -20.0);
    }
    harness.step();
    const Message state = clockState(stays);
    CHECK(state["nudge"] == -20.0);
    CHECK(state["tempo"].get<double>() == doctest::Approx(96.0));
}

TEST_CASE("resync puts the downbeat now: BPM-synced clips restart on it, timeline clips run on")
{
    TempDir media;
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    playMedia(client, copyMedia("count_709_tv.mp4", media.path / "count.mp4"));

    CHECK(frameAfterStep(harness, 45) == 22);
    CHECK(clock(client, {{"op", "resync"}})["op"] == "resync");
    CHECK(frameAfterStep(harness) == 0);
    CHECK(clockState(client)["beat"] == 0.0);
    CHECK(frameAfterStep(harness, 30) == 15);

    REQUIRE(edit(client, azl::SetClipTransport{1, SyncMode::Timeline, {}, {}})["type"] == "change");
    CHECK(frameAfterStep(harness, 10) == 20);
    clock(client, {{"op", "resync"}});
    CHECK(frameAfterStep(harness) == 20);
    CHECK(frameAfterStep(harness, 2) == 21);
}

TEST_CASE("play modes at their boundaries")
{
    TempDir media;
    azl::EngineHarness harness(kSize, 30.0);
    Client client(harness.document());
    const fs::path count = copyMedia("count_709_tv.mp4", media.path / "count.mp4");

    SUBCASE("loop wraps to the first frame")
    {
        playMedia(client, count, timeline(PlayMode::Loop));
        harness.step(58);
        CHECK(nextFrames(harness, 4) == std::vector{58, 59, 0, 1});
    }

    SUBCASE("bounce turns at either end, each end frame shown once per direction")
    {
        playMedia(client, count, timeline(PlayMode::Bounce));
        harness.step(58);
        CHECK(nextFrames(harness, 4) == std::vector{58, 59, 59, 58});
        CHECK(frameAfterStep(harness, 29) == 29);
        harness.step(27);
        CHECK(nextFrames(harness, 4) == std::vector{1, 0, 0, 1});
    }

    SUBCASE("bounce reads backward through several decode chunks and keyframes")
    {
        playMedia(client, copyMedia("count_100.mp4", media.path / "count100.mp4"), timeline(PlayMode::Bounce));
        harness.step(98);
        CHECK(nextFrames(harness, 4) == std::vector{98, 99, 99, 98});
        std::vector<int> backward = nextFrames(harness, 98);
        for (int i = 0; i < 98; ++i)
            CHECK(backward[size_t(i)] == 97 - i);
        CHECK(nextFrames(harness, 2) == std::vector{0, 1});
    }

    SUBCASE("play once and hold keeps the last frame")
    {
        playMedia(client, count, timeline(PlayMode::OnceHold));
        harness.step(58);
        CHECK(nextFrames(harness, 4) == std::vector{58, 59, 59, 59});
        CHECK(frameAfterStep(harness, 100) == 59);
    }

    SUBCASE("play once and clear shows nothing after the last frame")
    {
        playMedia(client, count, timeline(PlayMode::OnceClear));
        harness.step(58);
        CHECK(nextFrames(harness, 2) == std::vector{58, 59});
        harness.step();
        CHECK(blank(harness.readScreen()));
        CHECK_FALSE(clockState(client)["clips"][0]["visible"].get<bool>());
        harness.step(30);
        CHECK(blank(harness.readScreen()));
    }

    SUBCASE("a BPM-synced bounce turns on the beat grid")
    {
        playMedia(client, count, {SyncMode::Bpm, PlayMode::Bounce, 2});
        // 2 beats at 120 BPM: the clip runs at double speed and turns after one second.
        harness.step(29);
        CHECK(nextFrames(harness, 3) == std::vector{58, 59, 57});
    }
}

TEST_CASE("clip transport edits are undoable and keep the picture where it is")
{
    TempDir media;
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    playMedia(client, copyMedia("count_709_tv.mp4", media.path / "count.mp4"));
    CHECK(client.snapshot["composition"]["bpm"] == 120.0);

    CHECK(frameAfterStep(harness, 30) == 14);
    Message change = edit(client, azl::SetClipTransport{1, {}, {}, 8});
    CHECK(change["op"] == "setClipTransport");
    CHECK(change["beats"] == 8);
    CHECK(frameAfterStep(harness) == 15);
    CHECK(frameAfterStep(harness, 30) == 22);
    CHECK(clockState(client)["clips"][0]["beats"] == 8);

    client.send(msg::undo());
    change = client.next();
    CHECK(change["beats"] == 0);
    CHECK(change["cause"] == "undo");
    CHECK(frameAfterStep(harness) == 22);
    CHECK(frameAfterStep(harness, 4) == 24);

    // On the timeline the tempo no longer matters.
    edit(client, azl::SetClipTransport{1, SyncMode::Timeline, PlayMode::Bounce, {}});
    setTempo(client, 60);
    CHECK(frameAfterStep(harness, 2) == 25);
    CHECK(frameAfterStep(harness, 2) == 26);

    Client late(harness.document());
    const Message& transport = late.snapshot["composition"]["layers"][0]["clip"]["transport"];
    CHECK(transport["sync"] == "timeline");
    CHECK(transport["playMode"] == "bounce");
    CHECK(transport["beats"] == 0);
    CHECK(late.snapshot["composition"]["bpm"] == 60.0);

    client.send(msg::undo());
    CHECK(client.next()["bpm"] == 120.0);
}

TEST_CASE("invalid tempo and transport edits are refused")
{
    TempDir media;
    azl::EngineHarness harness(kSize);
    Client client(harness.document());

    CHECK(edit(client, azl::SetClipTransport{1, SyncMode::Timeline, {}, {}})["type"] == "error");
    playMedia(client, copyMedia("count_709_tv.mp4", media.path / "count.mp4"));
    CHECK(setTempo(client, 19.9)["type"] == "error");
    CHECK(setTempo(client, 501)["type"] == "error");
    CHECK(setTempo(client, 0)["type"] == "error");
    CHECK(setTempo(client, -120)["type"] == "error");
    CHECK(setTempo(client, std::nan(""))["type"] == "error");
    CHECK(nudge(client, std::nan(""))["type"] == "error");
    CHECK(edit(client, azl::SetClipTransport{1, {}, {}, -1})["type"] == "error");
    client.send({{"type", "command"}, {"op", "setClipTransport"}, {"layer", 1}, {"playMode", "pingpong"}});
    CHECK(client.next()["type"] == "error");
    CHECK(clock(client, {{"op", "stop"}})["type"] == "error");
    harness.step();
    CHECK(clockState(client)["bpm"] == 120.0);
}

TEST_CASE("absurd tap sequences clamp the tempo and never corrupt the clock")
{
    azl::EngineHarness harness(kSize, 1000.0);
    Client client(harness.document());

    clock(client, {{"op", "tap"}});
    CHECK(clock(client, {{"op", "tap"}})["bpm"].is_null());
    harness.step();
    client.send({{"type", "clock"}, {"op", "tap"}});
    CHECK(client.next()["bpm"] == 500.0);
    client.next();
    harness.step();
    const Message state = clockState(client);
    CHECK(state["bpm"] == 500.0);
    CHECK(std::isfinite(state["beat"].get<double>()));
}

TEST_CASE("a clip whose file breaks while bouncing holds its last frame; the other layers keep running")
{
    TempDir media;
    azl::Composition two = azl::defaultComposition();
    two.layers.push_back({.id = 2, .name = "Layer 2", .opacity = 0.0});
    azl::EngineHarness harness(kSize, 30.0, {}, two);
    Client client(harness.document());
    const fs::path broken = copyMedia("count_100.mp4", media.path / "breaks.mp4");
    const Message entry = addMedia(client, broken)["entry"];
    const Message good = addMedia(client, copyMedia("count_709_tv.mp4", media.path / "count.mp4"))["entry"];
    edit(client, azl::SetLayerClip{1, azl::ClipRef{entry["hash"], "breaks", timeline(PlayMode::Bounce)}});
    edit(client, azl::SetLayerClip{2, azl::ClipRef{good["hash"], "count", timeline(PlayMode::Loop)}});

    // Cut the file down to its first bytes, as an unplugged drive or an overwritten copy would. The frames already
    // decoded or buffered still play; turning back needs a new read, which fails.
    CHECK(frameAfterStep(harness, 40) == 39);
    fs::resize_file(broken, 64);
    std::vector<int> shown;
    for (int i = 0; i < 200; ++i) {
        harness.step();
        const azl::Image image = harness.readScreen();
        REQUIRE_FALSE(blank(image));
        shown.push_back(shownFrame(image));
    }
    CHECK(std::all_of(shown.end() - 60, shown.end(), [&](int frame) { return frame == shown.back(); }));
    CHECK(client.next()["hash"] == entry["hash"]);
    CHECK(clockState(client)["clips"][1]["frame"] == 239 % 60);
    // Once broken, the clip holds its picture without counting misses.
    const uint64_t misses = harness.stats().videoMisses;
    harness.step(30);
    CHECK(harness.stats().videoMisses == misses);

    client.send({{"type", "library"}, {"op", "list"}});
    const Message list = client.next();
    CHECK(std::any_of(list["entries"].begin(), list["entries"].end(),
                      [&](const Message& e) { return e["hash"] == entry["hash"] && e["status"] == "corrupt"; }));
}
