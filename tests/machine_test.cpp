#include "audio/audio_analysis.hpp"
#include "net/websocket.hpp"
#include "platform/thermal.hpp"
#include "protocol/build_info.hpp"
#include "protocol/bundle_push.hpp"
#include "test_client.hpp"
#include "test_paths.hpp"

#include <atomic>
#include <fstream>
#include <thread>

namespace {

constexpr azl::EngineConfig kSize{.width = 480, .height = 270};

azl::Composition stage(const std::vector<std::string>& displays)
{
    azl::Composition composition = azl::defaultComposition();
    uint64_t id = 0;
    for (const std::string& display : displays) {
        ++id;
        composition.screens.push_back({.id = id,
                                       .name = "Screen " + std::to_string(id),
                                       .display = display,
                                       .slices = {{.id = id, .name = "Slice " + std::to_string(id)}}});
    }
    return composition;
}

azl::DisplayInfo display(const std::string& id)
{
    return {id, "Epson " + id, kSize.width, kSize.height, 60.0};
}

Message status(Client& client)
{
    client.send({{"type", "machine"}, {"op", "status"}});
    for (;;)
        if (Message m = client.next(); m["type"] == "machine" || m["type"] == "error")
            return m;
}

const Message& output(const Message& status, uint64_t screen)
{
    for (const Message& o : status["outputs"])
        if (o["screen"] == screen)
            return o;
    FAIL("no output of screen " << screen);
    return status;
}

// Counts the machine status messages a client gets, without blocking the server.
struct Watcher {
    explicit Watcher(azl::DocumentServer& server) : server(server)
    {
        id = server.connect([this](const Message& m) {
            if (m.value("type", "") == "machine" && m.value("op", "") == "status")
                ++statuses;
            if (m.value("type", "") == "error")
                ++errors;
        });
        server.receive(id, msg::hello());
    }
    ~Watcher() { leave(); }
    void send(Message m) { server.receive(id, m); }
    void leave()
    {
        if (id)
            server.disconnect(std::exchange(id, 0));
    }
    int during(double seconds)
    {
        const int before = statuses;
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
        return statuses - before;
    }

    azl::DocumentServer& server;
    azl::ClientId id = 0;
    std::atomic<int> statuses{0};
    std::atomic<int> errors{0};
};

bool waitFor(const std::function<bool()>& done, double seconds = 5.0)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (!done()) {
        if (std::chrono::steady_clock::now() > end)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

} // namespace

TEST_CASE("the machine status names each output's state as the status bar does: ok, low rate, mirrored, missing")
{
    azl::EngineHarness harness(kSize, 60.0, {}, stage({"proj-a", "proj-b", "proj-c", "proj-d"}));
    for (const char* id : {"laptop", "proj-a", "proj-b", "proj-c", "proj-d"})
        harness.displays().attach(display(id));
    harness.outputs().setDelay("proj-b", 60);
    // The laptop carries no screen, so macOS may keep proj-c mirroring it.
    harness.displays().mirror("proj-c", "laptop");
    REQUIRE(harness.displays().unmirrored().empty());
    harness.displays().detach("proj-d");
    harness.setThermal({.cpuSpeedLimit = 70.0, .temperatureC = 88.5, .state = "serious"});
    Client client(harness.document());
    status(client);
    harness.runLive(1.5);

    const Message s = status(client);
    INFO(s.dump());
    CHECK(s["version"] == azl::buildId());
    CHECK(s["render"]["fps"].get<double>() > 30.0);
    CHECK(s["render"]["gpuMs"].is_number());
    CHECK(s["render"]["guard"] == 0);
    CHECK(s["thermal"] == Message{{"cpuSpeedLimit", 70.0}, {"temperatureC", 88.5}, {"state", "serious"}});
    REQUIRE(s["outputs"].size() == 4);

    const Message& ok = output(s, 1);
    CHECK(ok["state"] == "ok");
    CHECK(ok["screenName"] == "Screen 1");
    CHECK(ok["display"] == "proj-a");
    CHECK(ok["displayName"] == "Epson proj-a");
    CHECK(ok["hz"] == 60.0);
    CHECK(ok["presents"].get<double>() > azl::MachineService::kLowRate * 60.0);
    CHECK(ok["hitches"].is_number());

    const Message& starved = output(s, 2);
    CHECK(starved["state"] == "low rate");
    CHECK(starved["presents"].get<double>() < 30.0);

    CHECK(output(s, 3)["state"] == "mirrored");

    const Message& missing = output(s, 4);
    CHECK(missing["state"] == "missing");
    CHECK(missing["displayName"] == "Epson proj-d");
    CHECK(missing["presents"].is_null());
    CHECK(missing["hz"].is_null());

    CHECK(s["nextCue"].is_null());
    CHECK(s["audio"].is_null());
    CHECK(s["clock"]["djlink"].is_null());
}

TEST_CASE("the machine status names the next Cue and moves on once the beat clock passes it")
{
    azl::EngineHarness harness(kSize);
    Client client(harness.document());
    CHECK(status(client)["nextCue"].is_null());
    apply(client, azl::AddCue{std::nullopt, azl::Cue{.name = "Headliner", .bar = 2}});
    const uint64_t closing = apply(client, azl::AddCue{std::nullopt, azl::Cue{.bar = 3}})["cue"]["id"];
    harness.step();

    Message s = status(client);
    CHECK(s["nextCue"]["name"] == "Headliner");
    CHECK(s["nextCue"]["bar"] == 2);
    CHECK(s["nextCue"]["beat"] == 4.0);
    CHECK(s["nextCue"]["seconds"].get<double>() == doctest::Approx(2.0).epsilon(0.02));
    CHECK(s["clock"]["bpm"] == 120.0);
    CHECK(s["clock"]["bar"] == 1);

    harness.step(150);
    s = status(client);
    CHECK(s["nextCue"]["cue"] == closing);
    CHECK(s["nextCue"]["name"] == "Cue " + std::to_string(closing));
    CHECK(s["nextCue"]["bar"] == 3);
    CHECK(s["clock"]["bar"] == 2);
    CHECK(s["clock"]["beatInBar"] == 2);
}

TEST_CASE("the machine status reports the audio analysis's gain, level and liveness")
{
    azl::EngineHarness harness(kSize);
    azl::AudioAnalysis analysis([] { return 0.0; }, {.thread = false});
    azl::AudioSettings settings;
    settings.autoGain = false;
    settings.gainDb = 6.0;
    analysis.setSettings(settings);
    harness.setAudio(&analysis);
    Client client(harness.document());
    const Message audio = status(client)["audio"];
    harness.setAudio(nullptr);
    INFO(audio.dump());
    CHECK(audio["device"].is_null());
    CHECK(audio["level"].is_number());
    CHECK(audio["live"] == false);
}

TEST_CASE("a machine subscriber gets status at its rate until it unsubscribes or leaves; with none, nothing is sent")
{
    azl::EngineHarness harness(kSize);
    Watcher watcher(harness.document());
    CHECK(watcher.during(0.3) == 0);

    watcher.send({{"type", "machine"}, {"op", "subscribe"}, {"rate", 50}});
    REQUIRE(waitFor([&] { return watcher.errors > 0; }));
    CHECK(harness.machine().subscribers() == 0);

    watcher.send({{"type", "machine"}, {"op", "subscribe"}, {"rate", 5}});
    CHECK(harness.machine().subscribers() == 1);
    const int sent = watcher.during(1.0);
    CHECK(sent >= 3);
    CHECK(sent <= 7);

    watcher.send({{"type", "machine"}, {"op", "unsubscribe"}});
    CHECK(harness.machine().subscribers() == 0);
    watcher.during(0.1);
    CHECK(watcher.during(0.6) == 0);

    watcher.send({{"type", "machine"}, {"op", "subscribe"}, {"rate", 5}});
    REQUIRE(waitFor([&] { return watcher.statuses > 0 && harness.machine().subscribers() == 1; }));
    watcher.leave();
    CHECK(waitFor([&] { return harness.machine().subscribers() == 0; }));
}

TEST_CASE("the CPU speed limit comes from pmset, 100 while macOS has recorded none")
{
    CHECK(azl::parseCpuSpeedLimit("2026-10-16 03:12:44 +0200 CPU Power notify\n\tCPU_Scheduler_Limit \t= 100\n"
                                  "\tCPU_Available_CPUs \t= 6\n\tCPU_Speed_Limit \t= 82\n") == 82.0);
    CHECK(azl::parseCpuSpeedLimit("Note: No thermal warning level has been recorded\n"
                                  "Note: No CPU power status has been recorded\n") == 100.0);
    CHECK_FALSE(azl::parseCpuSpeedLimit(""));
}

namespace {

Message ask(Client& client, Message request)
{
    request["type"] = "machine";
    client.send(request);
    for (;;)
        if (Message m = client.next(); (m["type"] == "machine" && m["op"] == request["op"]) || m["type"] == "error")
            return m;
}

std::pair<int, std::string> runCtl(uint16_t port, const std::string& args)
{
    const fs::path binary = ctlPath();
    const std::string command = "'" + binary.string() + "' --port " + std::to_string(port) + " " + args + " 2>&1";
    std::string out;
    FILE* pipe = popen(command.c_str(), "r");
    REQUIRE(pipe);
    char buffer[512];
    while (fgets(buffer, sizeof buffer, pipe))
        out += buffer;
    const int status = pclose(pipe);
    return {WEXITSTATUS(status), out};
}

bool identifying(azl::EngineHarness& harness, uint64_t screen)
{
    const azl::Composition composition = harness.document().document();
    return harness.outputs().view(composition, composition.screens.at(screen - 1)).identify >
           std::chrono::steady_clock::now();
}

} // namespace

TEST_CASE("Test card, Identify and Fix mirroring act on the whole machine through the protocol")
{
    azl::EngineHarness harness(kSize, 60.0, {}, stage({"proj-a", "proj-b"}));
    harness.knownDisplays().setUnmirror(false);
    for (const char* id : {"proj-a", "proj-b"})
        harness.displays().attach(display(id));
    harness.displays().mirror("proj-b", "proj-a");
    Client client(harness.document());

    CHECK(ask(client, {{"op", "testcard"}, {"on", true}})["on"] == true);
    CHECK(harness.document().document().testCard);
    CHECK(status(client)["testCard"] == true);
    CHECK(ask(client, {{"op", "testcard"}, {"on", false}})["on"] == false);
    CHECK_FALSE(harness.document().document().testCard);
    CHECK(ask(client, {{"op", "testcard"}})["type"] == "error");

    CHECK_FALSE(identifying(harness, 1));
    CHECK(ask(client, {{"op", "identify"}, {"seconds", 3}})["seconds"] == 3.0);
    CHECK(identifying(harness, 1));
    CHECK(identifying(harness, 2));
    ask(client, {{"op", "identify"}, {"seconds", 0}});
    CHECK_FALSE(identifying(harness, 1));

    REQUIRE(output(status(client), 2)["state"] == "mirrored");
    CHECK(ask(client, {{"op", "unmirror"}})["ended"] == 1);
    CHECK(output(status(client), 2)["state"] != "mirrored");
    CHECK(harness.displays().unmirrored() == std::vector<std::vector<std::string>>{{"proj-a", "proj-b"}});
    CHECK(ask(client, {{"op", "unmirror"}})["ended"] == 0);
}

TEST_CASE("the Show lock refuses restart and safe mode for every client, and the next engine finds it on")
{
    TempDir root;
    std::vector<bool> restarts;
    std::optional<azl::EngineHarness> harness(std::in_place, kSize, 60.0, root.path);
    harness->onRestart([&](bool safeMode) { restarts.push_back(safeMode); });
    {
        Client director(harness->document());
        Client other(harness->document());
        CHECK(status(director)["showLock"] == false);
        CHECK(ask(director, {{"op", "lock"}, {"on", true}})["on"] == true);
        CHECK(status(other)["showLock"] == true);
        for (const char* op : {"restart", "safeMode"}) {
            CAPTURE(op);
            const Message refused = ask(other, {{"op", op}});
            CHECK(refused["type"] == "error");
            CHECK(refused["message"].get<std::string>().find("Show lock is on") != std::string::npos);
            CHECK(ask(director, {{"op", op}})["type"] == "error");
        }
        CHECK(harness->machine().actions().refusal("Update"));
        CHECK(restarts.empty());
    }

    harness.emplace(kSize, 60.0, root.path);
    harness->onRestart([&](bool safeMode) { restarts.push_back(safeMode); });
    Client client(harness->document());
    CHECK(status(client)["showLock"] == true);
    CHECK(ask(client, {{"op", "restart"}})["type"] == "error");
    CHECK(ask(client, {{"op", "lock"}, {"on", false}})["on"] == false);
    CHECK(ask(client, {{"op", "restart"}}) == Message{{"type", "machine"}, {"op", "restart"}});
    CHECK(ask(client, {{"op", "safeMode"}}) == Message{{"type", "machine"}, {"op", "safeMode"}});
    CHECK(restarts == std::vector<bool>{false, true});
    CHECK_FALSE(harness->machine().actions().refusal("Update"));
}

TEST_CASE("without a supervisor restart is refused, and a lock file that cannot be read counts as on")
{
    TempDir dir;
    azl::MachineActions unsupervised({}, nullptr);
    Message reply;
    const auto sink = [&](const Message& m) { reply = m; };
    CHECK(unsupervised.handle({{"op", "restart"}}, sink));
    CHECK(reply["message"].get<std::string>().find("supervisor") != std::string::npos);
    CHECK_FALSE(unsupervised.handle({{"op", "status"}}, sink));

    std::ofstream(dir.path / "show-lock.json") << "{\"on\": tr";
    CHECK(azl::MachineActions(dir.path / "show-lock.json", nullptr).locked());
    CHECK_FALSE(azl::MachineActions(dir.path / "missing.json", nullptr).locked());
}

TEST_CASE("nocturne-ctl machine runs every machine action and prints the Show lock's refusal")
{
    TempDir root;
    azl::EngineHarness harness(kSize, 60.0, root.path, stage({"proj-a", "proj-b"}));
    harness.knownDisplays().setUnmirror(false);
    for (const char* id : {"proj-a", "proj-b"})
        harness.displays().attach(display(id));
    harness.displays().mirror("proj-b", "proj-a");
    std::atomic<int> restarts{0}, safeModes{0};
    harness.onRestart([&](bool safeMode) {
        if (safeMode)
            ++safeModes;
        else
            ++restarts;
    });
    azl::WebSocketServer server(harness.document(), 0);
    const uint16_t port = server.port();

    auto [code, out] = runCtl(port, "machine testcard on");
    CHECK(code == 0);
    CHECK(harness.document().document().testCard);
    CHECK(runCtl(port, "machine identify 2").first == 0);
    CHECK(identifying(harness, 1));
    std::tie(code, out) = runCtl(port, "machine unmirror");
    CHECK(code == 0);
    CHECK(out.find("\"ended\":1") != std::string::npos);

    CHECK(runCtl(port, "machine lock on").first == 0);
    std::tie(code, out) = runCtl(port, "machine restart");
    CHECK(code == 1);
    CHECK(out.find("Show lock is on") != std::string::npos);
    CHECK(runCtl(port, "machine safemode").first == 1);
    CHECK(restarts + safeModes == 0);
    CHECK(runCtl(port, "machine lock off").first == 0);
    CHECK(runCtl(port, "machine restart").first == 0);
    CHECK(runCtl(port, "machine safemode").first == 0);
    CHECK(restarts == 1);
    CHECK(safeModes == 1);
    CHECK(runCtl(port, "machine lock sideways").first == 2);
}

namespace {

constexpr const char* kNewBuild = "20261018120000-abc123";

// An Nocturne.app whose engine carries `build`, with enough incompressible bytes to travel in several parts.
fs::path fakeBundle(const fs::path& dir, const std::string& build)
{
    const fs::path app = dir / "Nocturne.app";
    fs::create_directories(app / "Contents/MacOS");
    fs::create_directories(app / "Contents/Resources");
    std::ofstream(app / "Contents/MacOS/nocturne") << "#!/bin/bash\n# nocturne-build " << build << "\n";
    std::ofstream payload(app / "Contents/Resources/payload", std::ios::binary);
    uint32_t x = 2463534242u;
    for (int i = 0; i < (3 << 20) / 4; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        payload.write(reinterpret_cast<const char*>(&x), sizeof x);
    }
    return app;
}

void writeResult(const fs::path& file, const std::string& result, const std::string& reason = {})
{
    const fs::path tmp = file.string() + ".tmp";
    std::ofstream(tmp) << Message{{"result", result}, {"build", kNewBuild}, {"previous", "20261017000000-aaa"},
                                  {"reason", reason}, {"at", 1792300000}}
                              .dump();
    fs::rename(tmp, file);
}

bool refusedWith(const Message& reply, const std::string& text)
{
    INFO(reply.dump());
    return reply["type"] == "error" && reply["message"].get<std::string>().find(text) != std::string::npos;
}

} // namespace

TEST_CASE("Update is refused under the Show lock and inside a Gig window from any client; outside, the pushed bundle "
          "reaches the installer")
{
    TempDir root, laptop;
    const fs::path app = fakeBundle(laptop.path, kNewBuild);
    azl::EngineHarness harness(kSize, 60.0, root.path);
    const azl::UtcOffset utc = [](int64_t) { return 0; };
    const int64_t day = *azl::dateDays("2026-10-16");
    harness.setWallClock(double(azl::unixAt(day, 21 * 60, utc)), utc);
    std::vector<fs::path> installed;
    harness.onInstall([&](const fs::path& unpacked) -> std::optional<std::string> {
        installed.push_back(unpacked);
        return std::nullopt;
    });
    Client director(harness.document());
    Client other(harness.document());

    azl::BundlePush locked(app);
    CHECK(ask(director, {{"op", "lock"}, {"on", true}})["on"] == true);
    CHECK(refusedWith(ask(other, locked.start()), "Show lock is on"));
    CHECK(refusedWith(ask(director, locked.start()), "Show lock is on"));
    CHECK(status(other)["update"]["refused"].get<std::string>().find("Show lock") != std::string::npos);
    CHECK(ask(director, {{"op", "lock"}, {"on", false}})["on"] == false);

    apply(director, azl::SetGigWindow{"2026-10-16", azl::GigWindow{"2026-10-16", 20 * 60, 3 * 60}});
    CHECK(refusedWith(ask(other, locked.start()), "Gig window 2026-10-16 20:00-03:00 is on"));
    CHECK(status(director)["update"]["refused"].get<std::string>().find("Gig window") != std::string::npos);

    harness.setWallClock(double(azl::unixAt(day + 1, 3 * 60, utc)), utc);
    CHECK(status(director)["update"]["refused"].is_null());
    CHECK(status(director)["update"]["last"].is_null());

    SUBCASE("a lock turned on during the transfer refuses the install")
    {
        CHECK(ask(other, locked.start())["state"] == "receiving");
        CHECK(ask(other, *locked.next())["state"] == "receiving");
        CHECK(ask(director, {{"op", "lock"}, {"on", true}})["on"] == true);
        Message reply;
        while (std::optional<Message> part = locked.next())
            reply = ask(other, *part);
        CHECK(refusedWith(reply, "Show lock is on"));
        CHECK(installed.empty());
    }
    SUBCASE("accepted outside, in parts, unpacked for the installer")
    {
        azl::BundlePush push(app);
        Message reply = ask(other, push.start());
        CHECK(reply == Message{{"type", "machine"}, {"op", "update"}, {"state", "receiving"}, {"received", 0},
                               {"size", push.size()}});
        int parts = 0;
        while (reply["state"] == "receiving") {
            const std::optional<Message> part = push.next();
            REQUIRE(part);
            reply = ask(other, *part);
            ++parts;
        }
        CHECK(parts > 1);
        CHECK(reply == Message{{"type", "machine"}, {"op", "update"}, {"state", "installing"}, {"build", kNewBuild}});
        REQUIRE(installed.size() == 1);
        CHECK(installed[0].filename() == "Nocturne.app");
        CHECK(azl::bundleBuildId(installed[0]) == kNewBuild);
        CHECK(fs::file_size(installed[0] / "Contents/Resources/payload") ==
              fs::file_size(app / "Contents/Resources/payload"));

        writeResult(root.path / "update.json", "rolled back", "the engine does not answer within 60 s");
        const Message last = status(director)["update"]["last"];
        CHECK(last["result"] == "rolled back");
        CHECK(last["reason"] == "the engine does not answer within 60 s");
    }
}

TEST_CASE("a damaged or out-of-order Update is dropped with an error, and nothing is installed")
{
    TempDir root, laptop;
    const fs::path app = fakeBundle(laptop.path, kNewBuild);
    azl::EngineHarness harness(kSize, 60.0, root.path);
    int installs = 0;
    harness.onInstall([&](const fs::path&) -> std::optional<std::string> {
        ++installs;
        return std::nullopt;
    });
    Client client(harness.document());
    azl::BundlePush push(app);
    CHECK(ask(client, push.start())["state"] == "receiving");
    Message first = *push.next();
    first["offset"] = 7;
    CHECK(ask(client, first)["type"] == "error");
    CHECK(ask(client, {{"op", "update"}, {"offset", 0}, {"data", Message::binary({1, 2, 3})}})["type"] == "error");

    const std::vector<uint8_t> junk(1000, 0x5a);
    CHECK(ask(client, {{"op", "update"}, {"size", junk.size()}})["state"] == "receiving");
    const Message damaged = ask(client, {{"op", "update"}, {"offset", 0}, {"data", Message::binary(junk)}});
    CHECK(refusedWith(damaged, "damaged"));
    CHECK(damaged["op"] == "update");
    CHECK(installs == 0);
}

TEST_CASE("nocturne-ctl machine update sends the bundle, waits for the installer's result and exits 1 unless updated")
{
    TempDir root, laptop;
    const fs::path app = fakeBundle(laptop.path, kNewBuild);
    azl::EngineHarness harness(kSize, 60.0, root.path);
    std::atomic<bool> rollBack{false};
    harness.onInstall([&](const fs::path&) -> std::optional<std::string> {
        if (rollBack)
            writeResult(root.path / "update.json", "rolled back", "the engine renders no frames");
        else
            writeResult(root.path / "update.json", "updated");
        return std::nullopt;
    });
    azl::WebSocketServer server(harness.document(), 0);
    const uint16_t port = server.port();

    auto [code, out] = runCtl(port, "machine update '" + app.string() + "'");
    INFO(out);
    CHECK(code == 0);
    CHECK(out.find("sent ") != std::string::npos);
    CHECK(out.find("\"state\":\"installing\"") != std::string::npos);
    CHECK(out.find("\"result\":\"updated\"") != std::string::npos);

    rollBack = true;
    std::tie(code, out) = runCtl(port, "machine update '" + app.string() + "'");
    CHECK(code == 1);
    CHECK(out.find("the engine renders no frames") != std::string::npos);

    CHECK(runCtl(port, "machine lock on").first == 0);
    std::tie(code, out) = runCtl(port, "machine update '" + app.string() + "'");
    CHECK(code == 1);
    CHECK(out.find("Show lock is on") != std::string::npos);
    CHECK(runCtl(port, "machine update '" + laptop.path.string() + "'").first == 1);
}
