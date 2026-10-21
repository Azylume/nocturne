#include "engine/render_thread.hpp"
#include "net/websocket.hpp"
#include "sync/discovery.hpp"
#include "sync/node_sync.hpp"
#include "test_client.hpp"

#include <unistd.h>

#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <random>
#include <thread>

namespace {

using azl::BeatFollower;
using azl::BeatTimeline;
using azl::ClipRef;
using azl::Role;
using azl::SyncRole;

constexpr azl::EngineConfig kSize{.width = 320, .height = 180};
constexpr double kFps = 30.0;

bool waitUntil(const std::function<bool()>& done, double seconds = 30.0)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (!done()) {
        if (std::chrono::steady_clock::now() > end)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

// Harnesses stepped side by side keep a fixed offset (0 when they start together); the clock estimate itself is tested
// on its own.
class FixedClock : public azl::ClockLink {
public:
    explicit FixedClock(double offset) : offset_(offset) {}
    std::optional<double> offsetAt(double) const override { return offset_; }

private:
    double offset_;
};

// A main engine served on a localhost port, which goes away and comes back as the network does.
struct Main {
    azl::EngineHarness harness;
    std::unique_ptr<SyncRole> role;
    std::unique_ptr<azl::WebSocketServer> server;
    std::atomic<uint16_t> port{0};

    explicit Main(azl::EngineConfig config = kSize, bool fresh = false) : harness(config, kFps)
    {
        role = std::make_unique<SyncRole>(harness.document(), harness.engine(), harness.library(),
                                          [this] { return harness.time(); },
                                          SyncRole::Options{.role = Role::Main, .name = "main", .fresh = fresh});
        up();
    }

    void up(uint16_t at = 0)
    {
        server = std::make_unique<azl::WebSocketServer>(harness.document(), at);
        port = server->port();
    }
    void down() { server.reset(); }
};

struct Node {
    azl::EngineHarness harness;
    std::unique_ptr<SyncRole> role;

    explicit Node(std::atomic<uint16_t>& port, double offset = 0.0, azl::EngineConfig config = kSize)
        : harness(config, kFps)
    {
        role = std::make_unique<SyncRole>(
            harness.document(), harness.engine(), harness.library(), [this] { return harness.time(); },
            SyncRole::Options{.role = Role::Node,
                              .name = "node",
                              .locate = [&port]() -> std::optional<azl::Endpoint> {
                                  if (!port)
                                      return std::nullopt;
                                  return azl::Endpoint{"main", "127.0.0.1", port};
                              },
                              .clock = [offset](const azl::Endpoint&, uint16_t) {
                                  return std::make_unique<FixedClock>(offset);
                              }});
    }

    Message status() { return role->status()["node"]; }
};

void stepBoth(Main& main, Node& node, int frames)
{
    for (int i = 0; i < frames; ++i) {
        main.harness.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        node.harness.step();
    }
}

bool samePicture(const azl::Image& a, const azl::Image& b)
{
    if (a.width != b.width || a.height != b.height)
        return false;
    for (size_t i = 0; i < a.rgba.size(); ++i)
        if (std::abs(int(a.rgba[i]) - int(b.rgba[i])) > 2)
            return false;
    return true;
}

void triggerColumn(Client& client)
{
    client.send({{"type", "trigger"}, {"op", "column"}, {"column", 1}});
    REQUIRE(client.next()["op"] == "triggerColumn");
}

double beatMs(double beats, double bpm)
{
    return beats * 60000.0 / bpm;
}

} // namespace

TEST_CASE("a beat follower bends towards a new target instead of jumping, and jumps on a resync")
{
    BeatFollower follower;
    const double bpm = 120.0;
    follower.target({0.0, 0.0, bpm, 0.0, 0}, 0.0);
    CHECK(follower.beatAt(1.0) == doctest::Approx(2.0));

    // The main engine turns out to be 20 ms ahead.
    follower.target({1.0, 2.0 + 0.04, bpm, 0.0, 0}, 1.0);
    CHECK(follower.beatAt(1.0) == doctest::Approx(2.0));
    double last = follower.beatAt(1.0);
    const double frame = 1.0 / 60.0;
    for (double t = 1.0 + frame; t < 4.0; t += frame) {
        const double step = follower.beatAt(t) - last;
        CHECK(step >= 2.0 * frame * (1.0 - BeatFollower::kMaxBend) - 1e-9);
        CHECK(step <= 2.0 * frame * (1.0 + BeatFollower::kMaxBend) + 1e-9);
        last = follower.beatAt(t);
    }
    CHECK(follower.beatAt(4.0) == doctest::Approx(2.04 + 6.0));

    // A resync of the main engine is a jump there too.
    CHECK(follower.target({4.0, 0.0, bpm, 0.0, 1}, 4.0));
    CHECK(follower.beatAt(4.0) == doctest::Approx(0.0));
}

TEST_CASE("clock sync over a jittery network with drift agrees on the beat within 1 ms")
{
    std::mt19937 rng(7);
    std::exponential_distribution<double> jitter(1.0 / 0.0015);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const double offset = 1234.5;
    const double drift = 40e-6;
    auto mainTime = [&](double local) { return offset + local * (1.0 + drift); };
    auto oneWay = [&] { return 0.0008 + jitter(rng) + (unit(rng) < 0.03 ? 0.03 : 0.0); };

    const BeatTimeline main{mainTime(0.0), 100.0, 128.0, 0.0, 0};
    azl::ClockEstimator estimator;
    BeatFollower follower;
    double worst = 0.0;
    for (double local = 0.0; local < 60.0; local += 0.1) {
        const double sent = local;
        const double remoteReceived = mainTime(sent + oneWay());
        const double remoteSent = remoteReceived + 0.00005;
        const double received = (remoteSent - offset) / (1.0 + drift) + oneWay();
        estimator.add({sent, remoteReceived, remoteSent, received});
        const double now = received;
        follower.target({now, main.beatAt(now + *estimator.offsetAt(now)), main.bpm, 0.0, 0}, now);
        if (local > 10.0)
            worst = std::max(worst, std::abs(beatMs(follower.beatAt(now) - main.beatAt(mainTime(now)), main.bpm)));
    }
    MESSAGE("worst beat disagreement " << worst << " ms, drift estimate " << estimator.drift() * 1e6 << " ppm");
    CHECK(worst < 1.0);
    CHECK(std::abs(estimator.drift() - drift) < 20e-6);
}

TEST_CASE("UDP clock pings over loopback find the other clock's offset")
{
    azl::ClockResponder responder(0, [] { return azl::engineTime() + 500.0; });
    azl::ClockPinger pinger("127.0.0.1", responder.port(), azl::engineTime);
    REQUIRE(waitUntil([&] { return pinger.offsetAt(azl::engineTime()).has_value(); }, 5.0));
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const double error = *pinger.offsetAt(azl::engineTime()) - 500.0;
    MESSAGE("offset error " << error * 1e6 << " us, round trip " << pinger.roundTrip() * 1e6 << " us");
    CHECK(std::abs(error) < 0.0005);
}

TEST_CASE("Bonjour finds an engine and sees its role change")
{
    const std::string name = "nocturne-test-" + std::to_string(getpid());
    azl::Advertiser advertiser(name, 45123, {{"role", "main"}});
    azl::Browser browser;
    auto role = [&]() -> std::string {
        for (const azl::Service& s : browser.services())
            if (s.name == name && s.port == 45123)
                return s.txt.count("role") ? s.txt.at("role") : "";
        return "";
    };
    REQUIRE_MESSAGE(waitUntil([&] { return role() == "main"; }, 10.0), "not discovered (Local Network denied?)");
    advertiser.update({{"role", "node"}});
    CHECK(waitUntil([&] { return role() == "node"; }, 10.0));
}

TEST_CASE("a node replicates the composition and copies only the media it lacks")
{
    TempDir dir;
    Main main;
    Client editor(main.harness.document());
    const Message first = addMedia(editor, copyMedia("count_240.mp4", dir.path / "a" / "count_240.mp4"));
    apply(editor, azl::SetSlot{1, 1, ClipRef{first["entry"]["hash"], "count_240.mp4", {}}});
    triggerColumn(editor);

    Node node(main.port);
    REQUIRE(waitUntil([&] { return node.status()["synced"] == true && node.status()["files"] == 1; }));
    CHECK(node.status()["bytes"].get<uint64_t>() == fs::file_size(dir.path / "a" / "count_240.mp4"));
    CHECK(Message(node.harness.document().document()) == Message(main.harness.document().document()));
    const auto copied = node.harness.library().find(first["entry"]["hash"]);
    REQUIRE(copied);
    CHECK(copied->status == azl::MediaStatus::Ok);

    stepBoth(main, node, 20);
    CHECK(samePicture(main.harness.readScreen(), node.harness.readScreen()));
    CHECK(node.harness.transport().beat == doctest::Approx(main.harness.transport().beat).epsilon(1e-9));

    SUBCASE("a second clip copies only that clip; media the node has already are not copied")
    {
        const uint64_t before = node.status()["bytes"].get<uint64_t>();
        const fs::path second = copyMedia("count_100.mp4", dir.path / "b" / "count_100.mp4");
        const Message added = addMedia(editor, second);
        node.harness.library().add(copyMedia("count_601_pc.mp4", dir.path / "c" / "count_601_pc.mp4"));
        const Message present = addMedia(editor, copyMedia("count_601_pc.mp4", dir.path / "d" / "count_601_pc.mp4"));
        apply(editor, azl::SetSlot{1, 2, ClipRef{added["entry"]["hash"], "count_100.mp4", {}}});
        apply(editor, azl::SetSlot{1, 3, ClipRef{present["entry"]["hash"], "count_601_pc.mp4", {}}});
        REQUIRE(waitUntil([&] { return node.status()["files"] == 2 && node.status()["missing"] == 0; }));
        CHECK(node.status()["bytes"].get<uint64_t>() - before == fs::file_size(second));
        CHECK(Message(node.harness.document().document()) == Message(main.harness.document().document()));
    }

    SUBCASE("edits replicate; the node refuses edits of its own")
    {
        apply(editor, azl::SetLayerOpacity{1, 0.25});
        REQUIRE(waitUntil([&] { return node.harness.document().document().layers[0].opacity == 0.25; }));
        Client local(node.harness.document());
        const Message refused = command(local, azl::SetLayerOpacity{1, 0.5});
        CHECK(refused["type"] == "error");
        CHECK(refused["message"].get<std::string>().find("node") != std::string::npos);
    }
}

TEST_CASE("a node follows the main engine's beat clock through tempo changes and resyncs")
{
    Main main;
    Node node(main.port);
    Client editor(main.harness.document());
    REQUIRE(waitUntil([&] { return node.status()["synced"] == true; }));
    stepBoth(main, node, 5);
    apply(editor, azl::SetTempo{140.0});
    stepBoth(main, node, 5);
    CHECK(node.harness.transport().tempo == doctest::Approx(140.0));
    CHECK(beatMs(node.harness.transport().beat - main.harness.transport().beat, 140.0) == doctest::Approx(0.0).epsilon(1e-6));

    editor.send({{"type", "clock"}, {"op", "resync"}});
    editor.next();
    stepBoth(main, node, 5);
    CHECK(main.harness.transport().beat < 2.0);
    CHECK(beatMs(node.harness.transport().beat - main.harness.transport().beat, 140.0) == doctest::Approx(0.0).epsilon(1e-6));
}

TEST_CASE("when the main engine goes away the node plays on, and picks up again without a jump")
{
    TempDir dir;
    Main main;
    Client editor(main.harness.document());
    const Message clip = addMedia(editor, copyMedia("count_240.mp4", dir.path / "count_240.mp4"));
    apply(editor, azl::SetSlot{1, 1, ClipRef{clip["entry"]["hash"], "count_240.mp4", {}}});
    triggerColumn(editor);
    Node node(main.port);
    REQUIRE(waitUntil([&] { return node.status()["files"] == 1; }));
    stepBoth(main, node, 10);

    const uint16_t port = main.port;
    main.port = 0;
    main.down();
    REQUIRE(waitUntil([&] { return node.status()["connected"] == false; }));

    // Alone, the node keeps its composition, its clip and its beat clock.
    const Message kept = node.harness.document().document();
    double last = node.harness.transport().beat;
    for (int i = 0; i < 30; ++i) {
        main.harness.step();
        node.harness.step();
        const double beat = node.harness.transport().beat;
        CHECK(beat - last == doctest::Approx(2.0 / kFps).epsilon(1e-6));
        last = beat;
    }
    CHECK(Message(node.harness.document().document()) == kept);
    CHECK(node.harness.transport().clips.size() == 1);

    // The main engine moved on meanwhile; the node catches up when it is back.
    apply(editor, azl::SetLayerOpacity{1, 0.5});
    main.up(port);
    main.port = port;
    REQUIRE(waitUntil([&] { return node.status()["connected"] == true && node.status()["synced"] == true; }));
    REQUIRE(waitUntil([&] { return node.harness.document().document().layers[0].opacity == 0.5; }));
    for (int i = 0; i < 10; ++i) {
        main.harness.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        node.harness.step();
        const double beat = node.harness.transport().beat;
        CHECK(std::abs(beat - last - 2.0 / kFps) <= 2.0 / kFps * BeatFollower::kMaxBend + 1e-9);
        last = beat;
    }
    CHECK(samePicture(main.harness.readScreen(), node.harness.readScreen()));
}

TEST_CASE("a promoted node becomes the main engine with what it holds, and other nodes follow it")
{
    TempDir dir;
    Main main;
    Client editor(main.harness.document());
    const Message clip = addMedia(editor, copyMedia("count_100.mp4", dir.path / "count_100.mp4"));
    apply(editor, azl::SetSlot{1, 1, ClipRef{clip["entry"]["hash"], "count_100.mp4", {}}});
    Node node(main.port);
    REQUIRE(waitUntil([&] { return node.status()["files"] == 1; }));
    stepBoth(main, node, 5);
    main.port = 0;
    main.down();

    Client ui(node.harness.document());
    ui.send({{"type", "node"}, {"op", "promote"}});
    const Message promoted = ui.next();
    REQUIRE_MESSAGE(promoted["role"] == "main", promoted.dump());
    const double before = node.harness.transport().beat;
    node.harness.step();
    CHECK(node.harness.transport().beat - before == doctest::Approx(2.0 / kFps).epsilon(1e-6));
    REQUIRE(apply(ui, azl::SetLayerOpacity{1, 0.75})["type"] == "change");

    // The promoted engine serves a node of its own, media included.
    azl::WebSocketServer server(node.harness.document(), 0);
    std::atomic<uint16_t> port{server.port()};
    Node second(port);
    REQUIRE(waitUntil([&] { return second.status()["synced"] == true && second.status()["files"] == 1; }));
    CHECK(second.harness.document().document().layers[0].opacity == 0.75);

    ui.send({{"type", "node"}, {"op", "demote"}});
    CHECK(ui.next()["role"] == "node");
}

TEST_CASE("two live engines over loopback agree on the beat within 1 ms, through a tempo change and a resync")
{
    azl::EngineHarness mainEngine{kSize};
    azl::EngineHarness nodeEngine{kSize};
    azl::ClockResponder responder(0, azl::engineTime);
    SyncRole main(mainEngine.document(), mainEngine.engine(), mainEngine.library(), azl::engineTime,
                  {.role = Role::Main, .name = "main", .clockPort = responder.port()});
    azl::WebSocketServer server(mainEngine.document(), 0);
    const uint16_t port = server.port();
    SyncRole node(nodeEngine.document(), nodeEngine.engine(), nodeEngine.library(), azl::engineTime,
                  {.role = Role::Node,
                   .name = "node",
                   .locate = [port] { return std::optional(azl::Endpoint{"main", "127.0.0.1", port}); },
                   .clock = [](const azl::Endpoint& at, uint16_t clock) -> std::unique_ptr<azl::ClockLink> {
                       return std::make_unique<azl::ClockPinger>(at.host, clock, azl::engineTime);
                   }});
    azl::RenderThread mainRender(mainEngine.engine(), 60.0);
    azl::RenderThread nodeRender(nodeEngine.engine(), 60.0);
    Client editor(mainEngine.document());

    // The node's frame against the main engine's beat function at the node's frame time: both run on this machine's
    // clock, so this is the true disagreement.
    auto worstOver = [&](double seconds) {
        double worst = 0.0;
        const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        while (std::chrono::steady_clock::now() < end) {
            const azl::TransportState n = nodeEngine.transport();
            const azl::TransportState m = mainEngine.transport();
            worst = std::max(worst, std::abs(beatMs(n.beat - m.timeline.beatAt(n.time), m.tempo)));
            std::this_thread::sleep_for(std::chrono::milliseconds(7));
        }
        return worst;
    };

    std::this_thread::sleep_for(std::chrono::seconds(2));
    const double steady = worstOver(2.0);
    apply(editor, azl::SetTempo{140.0});
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const double afterTempo = worstOver(2.0);
    editor.send({{"type", "clock"}, {"op", "resync"}});
    editor.next();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const double afterResync = worstOver(2.0);
    MESSAGE("worst beat disagreement: " << steady << " ms steady, " << afterTempo << " ms after a tempo change, "
                                        << afterResync << " ms after a resync");
    CHECK(steady < 1.0);
    CHECK(afterTempo < 1.0);
    CHECK(afterResync < 1.0);
    CHECK(nodeEngine.transport().tempo == doctest::Approx(140.0));
}

TEST_CASE("engines with different seeds make the same Random Jump and Autopilot choices, also joining mid-show")
{
    TempDir dir;
    azl::EngineConfig mainConfig = kSize;
    mainConfig.seed = 11;
    azl::EngineConfig nodeConfig = kSize;
    nodeConfig.seed = 999;
    Main main(mainConfig);
    Client editor(main.harness.document());
    const azl::Transport jumping{.playMode = azl::PlayMode::RandomJump, .jumpInterval = 0.5, .jumpDistance = 4.0};
    const char* files[] = {"count_240.mp4", "count_100.mp4", "count_601_pc.mp4", "count_709_tv.mp4"};
    for (uint64_t column = 1; column <= 4; ++column) {
        const Message added = addMedia(editor, copyMedia(files[column - 1], dir.path / files[column - 1]));
        apply(editor, azl::SetSlot{1, column, ClipRef{added["entry"]["hash"], files[column - 1], jumping}});
    }
    apply(editor, azl::SetLayerAutopilot{1, azl::AutopilotMode::Random, 1});
    triggerColumn(editor);
    main.harness.step(int(kFps * 6));

    Node node(main.port, main.harness.time(), nodeConfig);
    REQUIRE(waitUntil([&] { return node.status()["synced"] == true && node.status()["files"] == 4; }));
    CHECK(node.harness.engine().seed() == 11);
    auto playing = [](azl::EngineHarness& harness) {
        const azl::TransportState t = harness.transport();
        REQUIRE(t.clips.size() == 1);
        return std::pair(t.clips[0].column, t.clips[0].frame);
    };
    std::set<uint64_t> columns;
    std::set<int> frames;
    for (int i = 0; i < int(kFps * 8); ++i) {
        main.harness.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        node.harness.step();
        REQUIRE(playing(node.harness) == playing(main.harness));
        columns.insert(playing(main.harness).first);
        frames.insert(playing(main.harness).second);
    }
    CHECK(columns.size() >= 3);
    CHECK(frames.size() > 20);

    // A promoted node keeps the seed, so engines following it choose alike too.
    Client ui(node.harness.document());
    ui.send({{"type", "node"}, {"op", "promote"}});
    REQUIRE(ui.next()["role"] == "main");
    CHECK(node.harness.engine().seed() == 11);
}

TEST_CASE("a main engine restarted without its show does not wipe the node's show")
{
    TempDir dir;
    std::atomic<uint16_t> port{0};
    auto first = std::make_unique<Main>();
    {
        Client editor(first->harness.document());
        const Message clip = addMedia(editor, copyMedia("count_240.mp4", dir.path / "count_240.mp4"));
        apply(editor, azl::SetSlot{1, 1, ClipRef{clip["entry"]["hash"], "count_240.mp4", {}}});
        triggerColumn(editor);
    }
    port = first->port.load();
    Node node(port);
    REQUIRE(waitUntil([&] { return node.status()["files"] == 1; }));
    const Message show = node.harness.document().document();

    // The laptop engine restarts with no state: the default composition, nothing edited.
    port = 0;
    first.reset();
    REQUIRE(waitUntil([&] { return node.status()["connected"] == false; }));
    Main restarted(kSize, true);
    port = restarted.port.load();
    REQUIRE(waitUntil([&] { return node.status()["connected"] == true && node.status()["held"] == true; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(Message(node.harness.document().document()) == show);
    node.harness.step(10);
    CHECK(node.harness.transport().clips.size() == 1);

    // Asked to, the node takes the main engine's composition after all.
    Client ui(node.harness.document());
    ui.send({{"type", "node"}, {"op", "adopt"}});
    Message adopted = ui.next();
    while (adopted["type"] != "node")
        adopted = ui.next();
    CHECK(adopted["node"]["held"] == false);
    CHECK(Message(node.harness.document().document()) == Message(restarted.harness.document().document()));
}

TEST_CASE("a node without fetching, as a UI's preview follower, mirrors the composition but copies no media")
{
    TempDir dir;
    Main main;
    Client editor(main.harness.document());
    const std::string hash =
        addMedia(editor, copyMedia("count_240.mp4", dir.path / "a" / "count_240.mp4"))["entry"]["hash"];
    apply(editor, azl::SetSlot{1, 1, ClipRef{hash, "count_240.mp4", {}}});
    triggerColumn(editor);

    azl::EngineHarness follower(kSize, kFps);
    azl::NodeSync node(
        follower.document(), follower.engine(), follower.library(), [&] { return follower.time(); },
        [&]() -> std::optional<azl::Endpoint> { return azl::Endpoint{"main", "127.0.0.1", main.port}; },
        [](const azl::Endpoint&, uint16_t) { return std::make_unique<FixedClock>(0.0); }, false);
    REQUIRE(waitUntil([&] { return node.status().synced && node.status().missing == 1; }));
    CHECK(Message(follower.document().document()) == Message(main.harness.document().document()));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(node.status().files == 0);
    CHECK(node.status().bytes == 0);
    const auto entry = follower.library().find(hash);
    CHECK((!entry || entry->status == azl::MediaStatus::Missing));
}
