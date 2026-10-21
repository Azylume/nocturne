#include "clips.hpp"

#include "audio/audio_analysis.hpp"
#include "document/modulation_edits.hpp"
#include "effects/effect_registry.hpp"
#include "media/composition_media.hpp"
#include "net/websocket.hpp"
#include "supervisor/state_saver.hpp"
#include "techno_loop.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <malloc/malloc.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <thread>

// Fault injection: each failure of the spec's resilience table, injected while the show runs. Rows covered elsewhere:
// decoder error mid-stream (jump_reverse_test), a display unplugged and back (screens_test), engine crash or hang
// (supervisor_test), Autopilot skipping a broken clip (autopilot_test).
namespace {

using azl::ClipRef;
using azl::SyncMode;
using Clock = std::chrono::steady_clock;

constexpr azl::EngineConfig kSize{.width = 960, .height = 540};
const azl::Transport kTimeline{SyncMode::Timeline};

// Two layers, each shown on its own screen, so either picture can be read alone.
azl::Composition twoLayers()
{
    azl::Composition c;
    c.layers = {{.id = 1, .name = "Bottom"}, {.id = 2, .name = "Top"}};
    c.screens = {{.id = 1, .name = "Bottom", .slices = {{.id = 1, .input = 1}}},
                 {.id = 2, .name = "Top", .slices = {{.id = 2, .input = 2}}}};
    return c;
}

// The next message the predicate accepts, skipping the others.
Message awaitMessage(Client& client, const std::function<bool(const Message&)>& match)
{
    for (int i = 0; i < 10000; ++i)
        if (Message m = client.next(); match(m))
            return m;
    FAIL("no such message");
    return {};
}

Message clipError(Client& client, const std::string& hash)
{
    return awaitMessage(client, [&](const Message& m) { return m["type"] == "error" && m.value("hash", "") == hash; });
}

std::string libraryStatus(Client& client, const std::string& hash)
{
    client.send({{"type", "library"}, {"op", "list"}});
    const Message list = awaitMessage(client, [](const Message& m) { return m["type"] == "library"; });
    for (const Message& entry : list["entries"])
        if (entry["hash"] == hash)
            return entry["status"];
    return "absent";
}

std::string play(Client& client, uint64_t layer, const fs::path& file)
{
    const Message added = addMedia(client, file);
    REQUIRE(added["type"] == "library");
    const std::string hash = added["entry"]["hash"];
    REQUIRE(apply(client, azl::SetLayerClip{layer, ClipRef{hash, file.filename().string(), kTimeline}})["type"] ==
            "change");
    return hash;
}

int shown(azl::EngineHarness& harness, uint64_t screen)
{
    return shownFrame(harness.readScreen(screen));
}

void halve(const fs::path& file)
{
    fs::resize_file(file, fs::file_size(file) / 2);
}

template <typename F>
bool eventually(F&& done, double seconds)
{
    const auto until = Clock::now() + std::chrono::duration<double>(seconds);
    while (Clock::now() < until) {
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return done();
}

} // namespace

TEST_CASE("a clip truncated or deleted while it plays holds its layer's last frame and is flagged; the rest plays on")
{
    TempDir media;
    azl::EngineHarness harness(kSize, 60.0, {}, twoLayers());
    Client client(harness.document());
    const fs::path cut = copyMedia("count_100.mp4", media.path / "cut.mp4");
    const fs::path gone = copyMedia("count_240.mp4", media.path / "gone.mp4");
    const std::string cutHash = play(client, 1, cut);
    const std::string goneHash = play(client, 2, gone);
    harness.step(20);
    CHECK(shown(harness, 1) == 9);
    CHECK(shown(harness, 2) == 9);

    // The decoder may read on from what it has buffered; by the loop at the latest it finds the file shorter.
    halve(cut);
    harness.step(240);
    const int held = shown(harness, 1);
    int other = shown(harness, 2);
    harness.step(30);
    CHECK(shown(harness, 1) == held);
    checkPatches(harness.readScreen(1));
    CHECK(clipError(client, cutHash)["layer"] == 1);
    CHECK(libraryStatus(client, cutHash) == "corrupt");
    CHECK(shown(harness, 2) == other + 15);

    // Deleted, the open file plays to its end; then it is gone.
    fs::remove(gone);
    harness.step(300);
    const int last = shown(harness, 2);
    harness.step(30);
    CHECK(shown(harness, 2) == last);
    CHECK(clipError(client, goneHash)["layer"] == 2);
    CHECK(libraryStatus(client, goneHash) == "missing");
    CHECK(shown(harness, 1) == held);
    const uint64_t misses = harness.stats().videoMisses;
    harness.step(30);
    CHECK(harness.stats().videoMisses == misses);
}

TEST_CASE("a half-copied clip is flagged and never plays; once the copy completes and is added again it plays")
{
    TempDir media;
    azl::EngineHarness harness(kSize, 60.0, {}, twoLayers());
    Client client(harness.document());
    play(client, 1, copyMedia("count_709_tv.mp4", media.path / "good.mp4"));
    harness.step(10);
    CHECK(shown(harness, 1) == 4);

    const fs::path copying = media.path / "copying.mp4";
    copyMedia("count_240.mp4", copying);
    halve(copying);
    const Message added = addMedia(client, copying);
    CHECK(added["entry"]["status"] == "corrupt");
    const std::string half = added["entry"]["hash"];
    REQUIRE(apply(client, azl::SetLayerClip{1, ClipRef{half, "copying.mp4", kTimeline}})["type"] == "change");
    harness.step(5);
    CHECK(shown(harness, 1) == 4);
    clipError(client, half);

    copyMedia("count_240.mp4", copying);
    const std::string complete = play(client, 1, copying);
    harness.step(1);
    CHECK(shown(harness, 1) == 0);
    CHECK(libraryStatus(client, complete) == "ok");
}

TEST_CASE("a removed media drive: its clips hold their last frame as missing files and play again when it returns")
{
    TempDir root;
    const fs::path drive = root.path / "Volumes/Show";
    const fs::path unplugged = root.path / "Volumes/.unplugged";
    azl::EngineHarness harness(kSize, 60.0, {}, twoLayers());
    Client client(harness.document());
    const std::string onDrive = play(client, 1, copyMedia("count_100.mp4", drive / "clips/loop.mp4"));
    play(client, 2, copyMedia("count_240.mp4", root.path / "local/long.mp4"));
    harness.step(10);

    fs::rename(drive, unplugged);
    // The decoder keeps the file it has open, as a cached read would; it notices at the loop.
    harness.step(220);
    CHECK(clipError(client, onDrive)["layer"] == 1);
    CHECK(libraryStatus(client, onDrive) == "missing");
    const int held = shown(harness, 1);
    const int other = shown(harness, 2);
    harness.step(30);
    CHECK(shown(harness, 1) == held);
    CHECK(shown(harness, 2) == other + 15);

    fs::rename(unplugged, drive);
    int now = held;
    CHECK(eventually(
        [&] {
            harness.step(2);
            now = shown(harness, 1);
            return now != held;
        },
        10.0));
    harness.step(4);
    CHECK(shown(harness, 1) != now);
    CHECK(libraryStatus(client, onDrive) == "ok");
}

TEST_CASE("a media drive force-detached while it plays, then attached again at the same place, plays again")
{
    TempDir root;
    const fs::path source = root.path / "source";
    const fs::path image = root.path / "show.dmg";
    const fs::path drive = root.path / "Show";
    copyMedia("count_240.mp4", source / "long.mp4");
    copyMedia("count_709_tv.mp4", source / "next.mp4");
    fs::create_directories(drive);
    const std::string attach = "hdiutil attach -quiet -nobrowse -mountpoint '" + drive.string() + "' '" +
                               image.string() + "'";
    const std::string detach = "hdiutil detach -quiet -force '" + drive.string() + "'";
    REQUIRE(std::system(("hdiutil create -quiet -ov -size 8m -fs HFS+ -volname Show -srcfolder '" + source.string() +
                         "' '" + image.string() + "'").c_str()) == 0);
    REQUIRE(std::system(attach.c_str()) == 0);

    {
        azl::EngineHarness harness(kSize, 60.0, {}, twoLayers());
        Client client(harness.document());
        const std::string hash = play(client, 1, drive / "long.mp4");
        play(client, 2, copyMedia("count_100.mp4", root.path / "local.mp4"));
        // Another clip on the drive, only preloaded: its decoder never reads, so it never notices the drive is gone.
        const Message next = addMedia(client, drive / "next.mp4");
        apply(client, azl::SetSlot{2, 3, ClipRef{next["entry"]["hash"], "next.mp4", kTimeline}});
        client.send({{"type", "trigger"}, {"op", "arm"}, {"layer", 2}, {"column", 3}, {"armed", true}});
        client.next();
        harness.step(20);
        CHECK(shown(harness, 1) == 9);

        REQUIRE(std::system(detach.c_str()) == 0);
        // A clip this small is read whole at once; the loop needs the file again.
        harness.step(480);
        CHECK(clipError(client, hash)["layer"] == 1);
        CHECK(libraryStatus(client, hash) == "missing");
        const int held = shown(harness, 1);
        harness.step(30);
        CHECK(shown(harness, 1) == held);

        REQUIRE(std::system(attach.c_str()) == 0);
        CHECK(eventually(
            [&] {
                harness.step(2);
                return shown(harness, 1) != held;
            },
            10.0));
        CHECK(libraryStatus(client, hash) == "ok");
    }
    std::system(detach.c_str());
}

TEST_CASE("an unplugged audio device releases audio modulation smoothly to neutral; it modulates again on return")
{
    azl::Composition c;
    c.layers = {{.id = 1, .name = "Card", .opacity = 0.0}};
    azl::EngineHarness harness({.width = 320, .height = 180}, 60.0, {}, c);
    Client client(harness.document());
    azl::AudioAnalysis analysis([&] { return harness.time(); }, {.thread = false});
    harness.setAudio(&analysis);
    azl::Modulator level;
    level.source = azl::ModulatorSource::Audio;
    level.feature = "level";
    level.target = {0, {1, 0, "opacity"}};
    apply(client, azl::AddModulator{std::nullopt, level});

    const std::vector<float> samples = TechnoLoop{.bars = 8}.render();
    size_t pushed = 0;
    auto opacity = [&] {
        client.send({{"type", "modulation"}, {"op", "state"}});
        const Message state = awaitMessage(client, [](const Message& m) { return m["type"] == "modulation"; });
        for (const Message& p : state["params"])
            if (p["param"] == "opacity")
                return p["value"].get<double>();
        return -1.0;
    };
    // Plugged in: the device delivers audio up to now. Unplugged: nothing arrives, and after the starve time the
    // analysis runs on silence, as its thread does.
    auto run = [&](int frames, bool plugged) {
        std::vector<double> values;
        for (int f = 0; f < frames; ++f) {
            if (plugged) {
                const size_t until = std::min(samples.size(), size_t(harness.time() * TechnoLoop::kRate));
                for (; pushed < until; pushed += std::min<size_t>(512, until - pushed))
                    analysis.push(samples.data() + pushed, std::min<size_t>(512, until - pushed),
                                  double(pushed) / TechnoLoop::kRate, TechnoLoop::kRate);
                analysis.pump();
            } else {
                pushed = size_t(harness.time() * TechnoLoop::kRate);
                analysis.pump(harness.time() - 0.1);
            }
            harness.step();
            values.push_back(opacity());
        }
        return values;
    };

    const std::vector<double> live = run(180, true);
    CHECK(*std::max_element(live.begin() + 60, live.end()) > 0.3);
    const uint64_t rendered = harness.stats().rendered;
    const std::vector<double> gone = run(180, false);
    CHECK(harness.stats().rendered == rendered + 180);
    double steepest = 0.0;
    for (size_t i = 1; i < gone.size(); ++i)
        steepest = std::max(steepest, std::abs(gone[i] - gone[i - 1]));
    INFO("opacity " << live.back() << " when unplugged, " << gone.back() << " 3 s later; steepest step " << steepest);
    CHECK(gone.back() < 0.01);
    CHECK(steepest < 0.15);
    const std::vector<double> back = run(180, true);
    CHECK(*std::max_element(back.begin() + 30, back.end()) > 0.3);
    harness.setAudio(nullptr);
}

TEST_CASE("a projector power-cycled five times re-attaches its screen; the other outputs never drop a frame")
{
    azl::Composition c;
    c.layers = {{.id = 1, .name = "Card"}};
    for (uint64_t id : {1, 2, 3})
        c.screens.push_back({.id = id, .name = "Screen", .display = "proj-" + std::to_string(id),
                             .slices = {{.id = id, .corners = {0.1, 0.0, 0.9, 0.1, 1.0, 1.0, 0.0, 0.9}}}});
    azl::EngineHarness harness({.width = 480, .height = 270}, 60.0, {}, c);
    harness.step();
    for (const char* id : {"proj-1", "proj-2", "proj-3"}) {
        harness.displays().attach({id, id, 480, 270, 60.0});
        std::this_thread::sleep_for(std::chrono::microseconds(5300));
    }
    auto counter = [&](const char* id, std::atomic<uint64_t> azl::OutputStats::*field) {
        return (harness.outputs().find(id)->stats().*field).load();
    };

    harness.startLive();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    harness.stats().maxGapMicros = 0;
    const uint64_t rendered = harness.stats().rendered;
    std::map<std::string, uint64_t> dropped, fresh;
    for (const char* id : {"proj-2", "proj-3"}) {
        dropped[id] = counter(id, &azl::OutputStats::dropped);
        fresh[id] = counter(id, &azl::OutputStats::newFrames);
    }
    const auto started = Clock::now();
    for (int cycle = 0; cycle < 5; ++cycle) {
        // The main output goes away and comes back, so the render clock re-locks each time.
        harness.displays().detach("proj-1");
        CHECK_FALSE(harness.outputs().find("proj-1")->attached());
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        harness.displays().attach({"proj-1", "proj-1", 480, 270, 60.0});
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        CHECK(harness.outputs().find("proj-1")->attached());
    }
    const uint64_t presentsBefore = counter("proj-1", &azl::OutputStats::presents);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
    const uint64_t made = harness.stats().rendered - rendered;
    const double gapMs = harness.stats().maxGapMicros / 1000.0;
    harness.stopLive();

    INFO(made << " frames in " << seconds << " s, longest render gap " << gapMs << " ms");
    CHECK(made / seconds > 55.0);
    CHECK(gapMs < 50.0);
    CHECK(counter("proj-1", &azl::OutputStats::presents) > presentsBefore + 10);
    for (const char* id : {"proj-2", "proj-3"}) {
        INFO(id);
        CHECK(counter(id, &azl::OutputStats::dropped) == dropped[id]);
        CHECK(counter(id, &azl::OutputStats::newFrames) - fresh[id] + 3 >= made);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    harness.step();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const azl::Image picture = harness.displays().capture("proj-1");
    REQUIRE(picture.width == 480);
    const azl::Image expected = harness.readScreen(1);
    size_t differing = 0;
    for (size_t i = 0; i < picture.rgba.size(); i += 4)
        differing += std::abs(int(picture.rgba[i]) - int(expected.rgba[i])) > 3;
    CHECK(differing < picture.rgba.size() / 4 / 500);
}

namespace {

// A WebSocket client that opens the connection and then never reads again, as a hung UI; it can vanish without a
// closing handshake, as a crashed one.
class HungClient {
public:
    explicit HungClient(uint16_t port)
    {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        const int small = 4096;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
        const std::string upgrade = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
        REQUIRE(::write(fd_, upgrade.data(), upgrade.size()) == ssize_t(upgrade.size()));
        char reply[512];
        REQUIRE(::read(fd_, reply, sizeof(reply)) > 0);
        REQUIRE(std::string(reply, 12) == "HTTP/1.1 101");
        // A masked text frame: {"type":"hello"} after the handshake makes it a full client.
        const std::string hello = R"({"type":"hello"})";
        std::string frame{char(0x81), char(0x80 | hello.size()), 0, 0, 0, 0};
        frame += hello;
        REQUIRE(::write(fd_, frame.data(), frame.size()) == ssize_t(frame.size()));
    }

    // Gone without a close frame: the kernel resets the connection.
    void crash()
    {
        linger hard{1, 0};
        ::setsockopt(fd_, SOL_SOCKET, SO_LINGER, &hard, sizeof(hard));
        ::close(std::exchange(fd_, -1));
    }

    ~HungClient()
    {
        if (fd_ >= 0)
            ::close(fd_);
    }

private:
    int fd_ = -1;
};

} // namespace

TEST_CASE("a hung or crashed UI leaves rendering and the other clients untouched")
{
    azl::Composition c;
    c.layers = {{.id = 1, .name = "Card"}};
    azl::EngineHarness harness({.width = 320, .height = 180}, 60.0, {}, c);
    azl::WebSocketServer server(harness.document(), 0);
    harness.step();
    std::vector<std::unique_ptr<HungClient>> hung;
    for (int i = 0; i < 3; ++i)
        hung.push_back(std::make_unique<HungClient>(server.port()));
    Client client(harness.document());

    harness.startLive();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    harness.stats().maxGapMicros = 0;
    const uint64_t rendered = harness.stats().rendered;
    const auto started = Clock::now();
    double slowest = 0.0;
    for (int i = 0; i < 2000; ++i) {
        const auto sent = Clock::now();
        REQUIRE(apply(client, azl::SetLayerOpacity{1, (i % 100) / 100.0})["type"] == "change");
        slowest = std::max(slowest, std::chrono::duration<double>(Clock::now() - sent).count());
        if (i == 1000)
            hung[0]->crash();
    }
    hung[1]->crash();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
    const uint64_t made = harness.stats().rendered - rendered;
    const double gapMs = harness.stats().maxGapMicros / 1000.0;
    harness.stopLive();

    INFO("2000 edits in " << seconds << " s, slowest reply " << slowest * 1000 << " ms; " << made / seconds
                          << " fps, longest render gap " << gapMs << " ms");
    CHECK(slowest < 0.25);
    CHECK(made / seconds > 55.0);
    CHECK(gapMs < 50.0);
    Message seen;
    azl::WebSocketClient fresh("127.0.0.1", server.port(), azl::Encoding::Json, [&](const Message& m) {
        if (m["type"] == "snapshot")
            seen = m;
    });
    fresh.send(msg::hello());
    CHECK(eventually([&] { return seen.is_object(); }, 5.0));
}

namespace {

// Costs GPU time in proportion to `loops`, changing the picture by less than a step of 8 bits.
void registerHeavy()
{
    if (azl::findEffect("testHeavy"))
        return;
    azl::registerEffect({.type = "testHeavy",
                         .kind = azl::StageKind::Pointwise,
                         .params = {{"loops", 0.0, 1e6, 0.0}},
                         .metal = R"(
    float4 a = c;
    for (int i = 0; i < int(p[0]); ++i)
        a = fract(a * 1.0001 + sin(a.yzwx) * 0.5);
    return mix(c, a, 0.0001);
)"});
}

} // namespace

TEST_CASE("under GPU overload the render rate degrades evenly; outputs keep presenting the latest frame, reported")
{
    registerHeavy();
    azl::Composition c;
    c.layers = {{.id = 1, .name = "Card", .effects = {{1, "testHeavy", {{"loops", 50.0}}}}}};
    for (uint64_t id : {1, 2})
        c.screens.push_back(
            {.id = id, .name = "Screen", .display = "proj-" + std::to_string(id), .slices = {{.id = id}}});
    azl::EngineHarness harness({.width = 1920, .height = 1080}, 60.0, {}, c);
    Client client(harness.document());
    harness.step();
    for (const char* id : {"proj-1", "proj-2"})
        harness.displays().attach({id, id, 640, 360, 60.0});

    auto rate = [&](double seconds) {
        const uint64_t before = harness.stats().rendered;
        harness.runLive(seconds);
        return (harness.stats().rendered - before) / seconds;
    };
    // The loop count that brings this GPU down to about 30 fps.
    double loops = 50.0;
    while (rate(0.5) > 40.0 && loops < 5e5) {
        loops *= 2.0;
        apply(client, azl::SetEffectParam{1, "loops", loops});
    }
    REQUIRE(loops < 5e5);

    std::map<std::string, uint64_t> presents, fresh, dropped;
    for (azl::DisplayOutput* output : harness.outputs().all()) {
        presents[output->display()] = output->stats().presents;
        fresh[output->display()] = output->stats().newFrames;
    }
    const uint64_t skipped = harness.stats().skipped;
    const uint64_t rendered = harness.stats().rendered;
    harness.stats().maxGapMicros = 0;
    constexpr double kSeconds = 2.0;
    harness.runLive(kSeconds);
    const double fps = (harness.stats().rendered - rendered) / kSeconds;
    const double gapMs = harness.stats().maxGapMicros / 1000.0;
    INFO(loops << " loops: " << fps << " fps, " << harness.stats().skipped - skipped
               << " renders skipped, longest render gap " << gapMs << " ms");
    CHECK(fps < 45.0);
    CHECK(fps > 5.0);
    // Reported: the frame rate itself, and renders skipped while every frame was still on the GPU.
    CHECK(harness.stats().skipped > skipped);
    CHECK(gapMs < 4000.0 / fps);
    for (azl::DisplayOutput* output : harness.outputs().all()) {
        const std::string& id = output->display();
        const double presentRate = (output->stats().presents - presents[id]) / kSeconds;
        const double newRate = (output->stats().newFrames - fresh[id]) / kSeconds;
        INFO(id << ": " << presentRate << " presents/s, " << newRate << " new frames/s");
        CHECK(presentRate > 50.0);
        CHECK(newRate > fps * 0.8);
    }
}

TEST_CASE("a full disk stops state saves with one notice; the show goes on and saving resumes when space returns")
{
    TempDir dir;
    const fs::path state = dir.path / "state";
    fs::create_directories(state);
    fs::permissions(state, fs::perms::owner_read | fs::perms::owner_exec);
    azl::EngineHarness harness({.width = 320, .height = 180});
    Client client(harness.document());
    azl::StateSaver saver(state / "composition.json", [] { return std::nullopt; });

    for (int i = 0; i < 40; ++i) {
        const auto before = Clock::now();
        saver.changed(std::make_shared<const azl::Composition>(harness.document().document()));
        CHECK(Clock::now() - before < std::chrono::milliseconds(5));
        apply(client, azl::SetLayerOpacity{1, i / 40.0});
        harness.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(saver.failures() >= 2);
    CHECK(saver.saves() == 0);
    CHECK(fs::is_empty(state));
    CHECK(harness.stats().rendered == 40);

    fs::permissions(state, fs::perms::owner_all);
    CHECK(eventually([&] { return saver.saves() >= 1; }, 3.0));
    CHECK(azl::loadCompositionFile(state / "composition.json")->composition.layers.size() == 1);
}

TEST_CASE("an old-format or newer composition loads what it can with notices; it never refuses the show")
{
    TempDir dir;
    azl::MediaLibrary library(dir.path / "library");
    const fs::path clip = copyMedia("count_709_tv.mp4", dir.path / "media/count.mp4");
    const std::string hash = library.add(clip).hash;
    auto load = [&](const std::string& name, const Message& j) {
        std::ofstream(dir.path / name) << j.dump(2);
        return azl::loadComposition(dir.path / name, library, {}, {});
    };
    auto mentions = [](const azl::LoadedComposition& loaded, const std::string& text) {
        return std::any_of(loaded.warnings.begin(), loaded.warnings.end(),
                           [&](const std::string& w) { return w.find(text) != std::string::npos; });
    };
    const Message cell = {{"hash", hash}, {"name", "count.mp4"}, {"path", "media/count.mp4"}};

    // An unversioned file holding the bare composition, as a snapshot is.
    const azl::LoadedComposition bare =
        load("bare.json", {{"layers", {{{"id", 1}, {"name", "Old"}, {"clips", {{"1", cell}}}}}}});
    CHECK(mentions(bare, "no format version"));
    REQUIRE(bare.composition.layers.size() == 1);
    CHECK(bare.composition.layers[0].clips.at(1).hash == hash);

    // Values this version does not know: they fall back to defaults, a cell without a hash is left empty.
    Message odd = {{"format", "nocturne.composition"},
                   {"version", 1},
                   {"composition",
                    {{"bpm", 128},
                     {"quantise", "sometimes"},
                     {"layers",
                      {{{"id", 1}, {"name", "A"}, {"blend", "hyperDodge"}, {"clips", {{"1", cell}, {"2", {{"name", "x"}}}}}},
                       {{"id", 2},
                        {"name", "B"},
                        {"clips", {{"1", {{"hash", hash}, {"name", "c"}, {"transport", {{"playMode", "sideways"}}}}}}}},
                       42}}}}};
    const azl::LoadedComposition repaired = load("odd.json", odd);
    CHECK(mentions(repaired, "hyperDodge"));
    CHECK(mentions(repaired, "cell 2"));
    CHECK(mentions(repaired, "sideways"));
    CHECK(mentions(repaired, "sometimes"));
    CHECK(mentions(repaired, "dropped"));
    REQUIRE(repaired.composition.layers.size() == 2);
    CHECK(repaired.composition.bpm == 128.0);
    CHECK(repaired.composition.layers[0].blend == azl::BlendMode::Normal);
    CHECK(repaired.composition.layers[0].clips.size() == 1);
    CHECK(repaired.composition.layers[1].clips.at(1).transport.playMode == azl::PlayMode::Loop);

    // A file from a newer version: what it adds is ignored.
    odd["version"] = 7;
    odd["composition"]["layers"].erase(2);
    odd["composition"]["hologram"] = {{"depth", 3}};
    odd["resume"] = "later";
    const azl::LoadedComposition newer = load("newer.json", odd);
    CHECK(mentions(newer, "newer version"));
    CHECK(newer.composition.layers.size() == 2);

    std::ofstream(dir.path / "torn.json") << "{\"layers\": [";
    CHECK_THROWS(azl::loadComposition(dir.path / "torn.json", library, {}, {}));
}

// The corpus is large and generated, not committed: tests/make_odd_media.sh prints the folder to pass.
TEST_CASE("the odd-media corpus plays or is refused, never crashing or stalling the render thread")
{
    const char* corpus = std::getenv("NOCTURNE_ODD_MEDIA_DIR");
    if (!corpus) {
        MESSAGE("skipped: set NOCTURNE_ODD_MEDIA_DIR to the folder tests/make_odd_media.sh prints");
        return;
    }
    std::ifstream in(fs::path(corpus) / "manifest.json");
    REQUIRE(in);
    const Message manifest = Message::parse(in);
    azl::EngineHarness harness({.width = 1920, .height = 1080});
    Client client(harness.document());
    harness.step();

    for (const auto& [name, entry] : manifest.items()) {
        const std::string expected = entry["expect"];
        const Message added = addMedia(client, fs::path(corpus) / name);
        std::string status = added["type"] == "library" ? added["entry"]["status"].get<std::string>() : "refused";
        uint64_t decoded = 0, errors = 0, seeks = 0;
        double fps = 0.0, gapMs = 0.0;
        if (added["type"] == "library") {
            const std::string hash = added["entry"]["hash"];
            REQUIRE(apply(client, azl::SetLayerClip{1, ClipRef{hash, name, kTimeline}})["type"] == "change");
            const uint64_t framesBefore = harness.decodeStats().frames;
            const uint64_t errorsBefore = harness.decodeStats().errors;
            const uint64_t seeksBefore = harness.decodeStats().seeks;
            const uint64_t rendered = harness.stats().rendered;
            harness.stats().maxGapMicros = 0;
            const auto started = Clock::now();
            harness.startLive();
            std::this_thread::sleep_for(std::chrono::milliseconds(status == "ok" ? 2000 : 500));
            harness.stopLive();
            const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
            decoded = harness.decodeStats().frames - framesBefore;
            errors = harness.decodeStats().errors - errorsBefore;
            seeks = harness.decodeStats().seeks - seeksBefore;
            fps = (harness.stats().rendered - rendered) / seconds;
            gapMs = harness.stats().maxGapMicros / 1000.0;
            status = libraryStatus(client, hash);
        }
        const std::string outcome = status == "ok" && decoded > 0 ? "played" : "refused";
        MESSAGE(name << ": " << outcome << " (" << status << "), " << decoded << " frames decoded, " << errors
                     << " decode errors, " << seeks << " seeks, render " << fps << " fps, longest gap " << gapMs << " ms; expected "
                     << expected);
        INFO(name);
        if (expected == "play")
            CHECK(outcome == "played");
        if (expected == "flag")
            CHECK(outcome == "refused");
        if (added["type"] == "library") {
            CHECK(fps > 45.0);
            CHECK(gapMs < 100.0);
        }
    }
}

// Autopilot opens and closes decoders all show long; each round trip must give back what it took. Without an autorelease
// pool around opening and closing on the decoders' thread, AVFoundation's objects piled up there: 27 MB per 900
// switches on an M2 Pro, about 57 MB an hour on the Mac mini, and CoreMedia's notifications slowed down with them.
TEST_CASE("a thousand clip switches leave memory and asset readers where they were")
{
    TempDir media;
    auto device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    azl::MediaLibrary library(media.path / "library");
    const std::string a = library.add(copyMedia("count_240.mp4", media.path / "a.mp4")).hash;
    const std::string b = library.add(copyMedia("count_100.mp4", media.path / "b.mp4")).hash;
    auto inUse = [] {
        malloc_statistics_t stats{};
        malloc_zone_statistics(nullptr, &stats);
        return double(stats.size_in_use) / 1048576.0;
    };
    const int64_t readers = azl::liveAssetReaders();
    azl::ClipDecoders decoders(device.get(), &library, size_t(64) << 20, size_t(64) << 20);
    double warm = 0.0;
    for (int i = 0; i < 1000; ++i) {
        if (i == 100)
            warm = inUse();
        const azl::ClipKey clip{1, i % 2 ? b : a};
        decoders.want({clip}, {});
        REQUIRE(decoders.frame(clip, 0, true, {}, true));
    }
    const double grown = inUse() - warm;
    MESSAGE("malloc in use grew " << grown << " MB over 900 switches");
    CHECK(grown < 3.0);
    decoders.want({}, {});
    decoders.waitIdle();
    CHECK(eventually([&] { return azl::liveAssetReaders() == readers; }, 5.0));
}

// A column trigger makes the next column's clips preloads and the last column's unwanted. Their decoders open and close
// one at a time, spread out, so the hardware decoder never sets up or tears down five sessions at once; a clip that
// plays does not wait for that.
namespace {

// Polls `done` for up to 5 s. Null when a poll came over 100 ms late, as on a loaded machine: a count read then may
// include an open paced after the moment `done` became true.
std::optional<bool> promptly(const std::function<bool()>& done)
{
    const auto until = Clock::now() + std::chrono::seconds(5);
    for (auto last = Clock::now(); Clock::now() < until;) {
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (Clock::now() - last > std::chrono::milliseconds(100))
            return std::nullopt;
        last = Clock::now();
    }
    return false;
}

} // namespace

TEST_CASE("preloads open and unwanted decoders close spread out; a playing clip opens at once")
{
    TempDir media;
    auto device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    azl::MediaLibrary library(media.path / "library");
    const std::string a = library.add(copyMedia("count_240.mp4", media.path / "a.mp4")).hash;
    std::vector<azl::Preload> column;
    for (uint64_t layer = 1; layer <= 5; ++layer)
        column.push_back({{layer, a}});
    const azl::ClipKey playing{9, a};

    bool seen = false;
    for (int attempt = 1; attempt <= 2 && !seen; ++attempt) {
        azl::ClipDecoders decoders(device.get(), &library, size_t(64) << 20, size_t(64) << 20);
        const azl::DecodeStats& stats = decoders.stats();
        const auto start = Clock::now();
        decoders.want({}, column);
        const std::optional<bool> preloading = promptly([&] { return stats.opens >= 1; });
        REQUIRE(preloading.value_or(true));
        if (!preloading) {
            MESSAGE("UNDECIDED: polled too late to tell, attempt " << attempt);
            continue;
        }
        const uint64_t before = stats.opens;
        decoders.want({playing}, column);
        const std::optional<bool> opened = promptly([&] { return decoders.length(playing, false).has_value(); });
        REQUIRE(opened.value_or(true));
        if (!opened) {
            MESSAGE("UNDECIDED: polled too late to tell, attempt " << attempt);
            continue;
        }
        seen = true;
        // The playing clip, the preload under way when it came, and one paced while the poll slept; not the rest.
        CHECK(stats.opens <= before + 3);
        REQUIRE(eventually([&] { return stats.opens == 6; }, 5.0));
        CHECK(Clock::now() - start >= std::chrono::milliseconds(4 * 150));

        const auto closing = Clock::now();
        decoders.want({}, {});
        REQUIRE(eventually([&] { return stats.closes == 6; }, 5.0));
        CHECK(Clock::now() - closing >= std::chrono::milliseconds(5 * 150));
    }
    REQUIRE_MESSAGE(seen, "undecided twice in a row");
}
