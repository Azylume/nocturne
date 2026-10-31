#include "clips.hpp"
#include "crash_marks.hpp"
#include "http_get.hpp"

#include "audio/audio_input.hpp"
#include "document/composition_file.hpp"
#include "net/websocket.hpp"
#include "supervisor/crash_guard.hpp"
#include "supervisor/rotating_log.hpp"
#include "supervisor/state_saver.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fstream>
#include <functional>
#include <future>
#include <optional>
#include <spawn.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace {

using azl::AutopilotMode;
using azl::ClipRef;
using Clock = std::chrono::steady_clock;

constexpr azl::EngineConfig kSize{.width = 960, .height = 540};

std::string readFile(const fs::path& path)
{
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
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

std::string add(Client& client, const fs::path& path)
{
    const Message added = addMedia(client, path);
    REQUIRE(added["type"] == "library");
    return added["entry"]["hash"];
}

void setSlot(Client& client, uint64_t column, const std::string& hash)
{
    REQUIRE(command(client, azl::SetSlot{1, column, ClipRef{hash, "clip", {}}})["op"] == "setSlot");
}

azl::Composition sampleComposition()
{
    azl::Composition c = azl::defaultComposition();
    c.bpm = 128.0;
    c.column = 2;
    azl::Layer& layer = c.layers.front();
    layer.clips[1] = ClipRef{std::string(64, 'b'), "a.mp4", {}, 2};
    layer.clips[2] = ClipRef{std::string(64, 'c'), "b.mp4", {azl::SyncMode::Timeline, azl::PlayMode::Bounce}, 0};
    layer.playing = azl::Play{2, 16.0};
    layer.autopilot = {AutopilotMode::Random, 8};
    layer.effects.push_back({1, "colorize", {{"hue", 0.25}}});
    c.screens.push_back({.id = 1, .name = "Screen 1", .display = "window-1", .slices = {{.id = 1, .name = "Slice 1"}}});
    return c;
}

} // namespace

TEST_CASE("a composition file round-trips the document and where the show was")
{
    TempDir dir;
    azl::CompositionFile file{sampleComposition(),
                              azl::Resume{1700000000.5, 33.25, 128.0, {{1, azl::Play{2, 16.0}, {1, 24.0}, 24.0, 2, true}},
                                          azl::ResumeAudio{{"Scarlett", {1, 2}}, {"off", {}}}}};
    azl::saveCompositionFile(dir.path / "show.json", file);
    const auto loaded = azl::loadCompositionFile(dir.path / "show.json");
    REQUIRE(loaded);
    CHECK(azl::toJson(*loaded) == azl::toJson(file));
    CHECK(loaded->resume->pilots == file.resume->pilots);
    CHECK(loaded->resume->audio == file.resume->audio);
    CHECK(loaded->resume->beatAt(1700000002.5) == doctest::Approx(33.25 + 2 * 128.0 / 60.0));

    CHECK_FALSE(azl::loadCompositionFile(dir.path / "missing.json"));
    std::ofstream(dir.path / "bad.json") << "{\"format\": \"nocturne.composition\", \"version\": 99}";
    CHECK_THROWS_AS(azl::loadCompositionFile(dir.path / "bad.json"), azl::ProtocolError);
    std::ofstream(dir.path / "torn.json") << "{\"format\": \"azylu";
    CHECK_THROWS_AS(azl::loadCompositionFile(dir.path / "torn.json"), azl::ProtocolError);
    CHECK_FALSE(fs::exists(dir.path / "show.json.tmp"));
}

TEST_CASE("a start reopens the audio input picked under the same command line, unless its device is gone")
{
    const std::vector<azl::AudioDeviceInfo> devices = {
        {"AppleUSBAudioEngine:Focusrite:1", "Scarlett 2i2 USB", 2, 48000.0}};
    const azl::AudioChoice commandLine{"off", {}};
    const auto saved = [&](azl::AudioChoice open) { return azl::ResumeAudio{std::move(open), commandLine}; };
    std::string log;

    const azl::AudioChoice chosen{"scarlett", {2}};
    CHECK(azl::startingInput(commandLine, saved(chosen), devices, log) == chosen);
    CHECK(log.find("reopening") != std::string::npos);
    const azl::AudioChoice byUid{"AppleUSBAudioEngine:Focusrite:1", {}};
    CHECK(azl::startingInput(commandLine, saved(byUid), devices, log) == byUid);
    const azl::AudioChoice system{"system", {}};
    CHECK(azl::startingInput(commandLine, saved(system), {}, log) == system);
    CHECK(azl::startingInput({"scarlett", {}}, azl::ResumeAudio{{"off", {}}, {"scarlett", {}}}, devices, log) ==
          azl::AudioChoice{"off", {}});

    CHECK(azl::startingInput(commandLine, saved(chosen), {}, log) == commandLine);
    CHECK(log.find("'scarlett'") != std::string::npos);
    CHECK(log.find("not attached") != std::string::npos);

    CHECK(azl::startingInput({"scarlett", {1}}, saved(chosen), devices, log) == azl::AudioChoice{"scarlett", {1}});
    CHECK(log.empty());
    CHECK(azl::startingInput(commandLine, std::nullopt, devices, log) == commandLine);
    CHECK(azl::startingInput(commandLine, saved(commandLine), devices, log) == commandLine);
    CHECK(log.empty());
}

TEST_CASE("a rotating log appends across reopenings and keeps a fixed number of old files")
{
    TempDir dir;
    const fs::path path = dir.path / "logs" / "audio.jsonl";
    const std::string line(40, 'x');
    {
        azl::RotatingLog log(path, 100, 2);
        log.write(line + "\n");
    }
    {
        azl::RotatingLog log(path, 100, 2);
        log.write(line + "\n");
        CHECK(fs::file_size(path) == 82);
        for (int i = 0; i < 10; ++i)
            log.write(line + "\n");
    }
    CHECK(fs::file_size(path) <= 100);
    CHECK(fs::exists(path.string() + ".1"));
    CHECK(fs::exists(path.string() + ".2"));
    CHECK_FALSE(fs::exists(path.string() + ".3"));
}

TEST_CASE("a real fault inside a chain compile is recorded on the compiling thread and the process still dies of it")
{
    CrashFiles files;
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        azl::crash::setup(files.marks, files.skips, files.crash);
        azl::crash::installCrashHandlers();
        azl::crash::Scope compiling("effect", kChain, kChain);
        int* volatile nowhere = nullptr;
        *nowhere = 1;
        ::_exit(0);
    }
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    REQUIRE(WIFSIGNALED(status));
    CHECK((WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS));
    const std::optional<azl::crash::Crash> crash = azl::crash::readCrash(files.crash);
    REQUIRE(crash);
    CHECK(crash->signal == WTERMSIG(status));
    const std::vector<azl::crash::Mark> culprits = files.culprits();
    REQUIRE(culprits.size() == 1);
    CHECK(culprits[0].key == kChain);
}

TEST_CASE("a crash in a Metal completion handler while a chain compiles on another thread skip-lists nothing")
{
    CrashFiles files;
    HeldMark compiling("effect", kChain, kChain);
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    auto device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    auto queue = NS::TransferPtr(device->newCommandQueue());
    MTL::CommandBuffer* cmd = queue->commandBuffer();
    std::promise<void> completed;
    cmd->addCompletedHandler([&](MTL::CommandBuffer*) {
        azl::crash::recordCrash(SIGSEGV);
        completed.set_value();
    });
    cmd->commit();
    completed.get_future().wait();
    pool->release();
    REQUIRE(azl::crash::readCrash(files.crash));
    REQUIRE(azl::crash::readMarks(files.marks).size() == 1);
    CHECK(files.culprits().empty());
}

TEST_CASE("the state saver writes changes debounced and the beat grid only when it drifts")
{
    TempDir dir;
    const fs::path path = dir.path / "composition.json";
    std::mutex mutex;
    std::optional<azl::Resume> now;
    auto saver = std::make_unique<azl::StateSaver>(path, [&] {
        std::lock_guard lock(mutex);
        return now;
    });
    auto composition = std::make_shared<const azl::Composition>(sampleComposition());
    saver->changed(composition);
    REQUIRE(eventually([&] { return saver->saves() == 1; }, 2.0));
    CHECK_FALSE(azl::loadCompositionFile(path)->resume);

    {
        std::lock_guard lock(mutex);
        now = azl::Resume{1000.0, 8.0, 120.0, {}};
    }
    REQUIRE(eventually([&] { return saver->saves() == 2; }, 2.0));
    {
        std::lock_guard lock(mutex);
        now = azl::Resume{1010.0, 28.0, 120.0, {}};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(saver->saves() == 2);
    {
        std::lock_guard lock(mutex);
        now = azl::Resume{1010.0, 0.0, 120.0, {}};
    }
    REQUIRE(eventually([&] { return saver->saves() == 3; }, 2.0));
    CHECK(azl::loadCompositionFile(path)->resume->beat == 0.0);

    for (int i = 0; i < 20; ++i)
        saver->changed(std::make_shared<const azl::Composition>(sampleComposition()));
    saver.reset();
    CHECK(azl::loadCompositionFile(path)->composition.bpm == 128.0);
}

TEST_CASE("the state saver backs the file up once a minute while it changes and before a load, keeping the last 20")
{
    TempDir dir;
    const fs::path path = dir.path / "composition.json";
    const auto count = [&](const std::string& kind) {
        size_t n = 0;
        for (const auto& entry : fs::directory_iterator(dir.path / "backups"))
            n += entry.path().filename().string().ends_with("-" + kind + ".json");
        return n;
    };
    azl::StateSaver saver(path, [] { return std::nullopt; });
    saver.backup("load");
    const bool nothingToBackUp = !fs::exists(dir.path / "backups") || count("load") == 0;
    CHECK(nothingToBackUp);

    saver.changed(std::make_shared<const azl::Composition>(sampleComposition()));
    REQUIRE(eventually([&] { return saver.saves() == 1; }, 2.0));
    CHECK(count("minute") == 1);
    azl::Composition changed = sampleComposition();
    changed.bpm = 90.0;
    saver.changed(std::make_shared<const azl::Composition>(changed));
    REQUIRE(eventually([&] { return saver.saves() == 2; }, 2.0));
    CHECK(count("minute") == 1);

    for (int i = 0; i < 25; ++i) {
        saver.backup("load");
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    CHECK(count("load") == azl::StateSaver::kBackups);
    CHECK(count("minute") == 1);
    for (const auto& entry : fs::directory_iterator(dir.path / "backups"))
        if (entry.path().filename().string().ends_with("-load.json"))
            CHECK(azl::loadCompositionFile(entry.path())->composition.bpm == 90.0);
}

// Three clips on layer 1 with Autopilot stepping every 2 bars, column 1 triggered on beat 0.
azl::Composition savedShow(const fs::path& media, const fs::path& library)
{
    azl::EngineHarness harness(kSize, 60.0, library);
    Client client(harness.document());
    setSlot(client, 1, add(client, copyMedia("count_709_tv.mp4", media / "a.mp4")));
    setSlot(client, 2, add(client, copyMedia("count_601_pc.mp4", media / "b.mp4")));
    setSlot(client, 3, add(client, copyMedia("count_100.mp4", media / "c.mp4")));
    REQUIRE(command(client, azl::SetLayerAutopilot{1, AutopilotMode::Next, 2})["op"] == "setLayerAutopilot");
    client.send({{"type", "trigger"}, {"op", "column"}, {"column", 1}});
    REQUIRE(client.next()["op"] == "triggerColumn");
    harness.step();
    return harness.document().document();
}

TEST_CASE("a resumed engine continues the beat grid, the clip position and Autopilot's step")
{
    TempDir media;
    TempDir library;
    azl::Composition saved = savedShow(media.path, library.path);
    REQUIRE(saved.layers[0].playing->beat == 0.0);

    SUBCASE("Autopilot resumes on the clip it had moved on to, not the one triggered")
    {
        azl::EngineHarness harness(kSize, 60.0, library.path, saved);
        harness.resume(10.0, {{1, saved.layers[0].playing, azl::Play{2, 8.0}, 8.0, 3}});
        harness.step();
        const azl::TransportState state = harness.transport();
        CHECK(state.beat == doctest::Approx(10.0));
        REQUIRE(state.clips.size() == 1);
        CHECK(state.clips[0].column == 2);
        CHECK(state.clips[0].next == 3);
        CHECK(state.clips[0].frame == 30);
        CHECK(shownFrame(harness.readScreen()) == 30);
        REQUIRE(state.pilots.size() == 1);
        CHECK(state.pilots[0].stepStart == 8.0);

        harness.step(int(6 * 30));
        CHECK(harness.transport().clips[0].column == 3);
    }

    SUBCASE("a layer held by a trigger stays held to the end of its hold, then Autopilot moves on")
    {
        saved.layers[0].autopilot.hold = 4;
        azl::EngineHarness harness(kSize, 60.0, library.path, saved);
        harness.resume(10.0, {{1, saved.layers[0].playing, azl::Play{2, 8.0}, 8.0, 3, true}});
        harness.step();
        REQUIRE(harness.transport().clips.size() == 1);
        CHECK(harness.transport().clips[0].column == 2);
        CHECK(harness.transport().clips[0].held == 4);
        harness.step(int(6 * 30));
        CHECK(harness.transport().clips[0].column == 2);
        REQUIRE(harness.transport().pilots.size() == 1);
        CHECK(harness.transport().pilots[0].held);
        harness.step(int(8 * 30));
        CHECK(harness.transport().clips[0].column == 3);
        CHECK(harness.transport().clips[0].held == 0);
        CHECK_FALSE(harness.transport().pilots[0].held);
    }

    SUBCASE("without Autopilot the triggered clip continues from its beat")
    {
        saved.layers[0].autopilot.mode = AutopilotMode::Off;
        azl::EngineHarness harness(kSize, 60.0, library.path, saved);
        harness.resume(6.0, {});
        harness.step();
        const azl::TransportState state = harness.transport();
        REQUIRE(state.clips.size() == 1);
        CHECK(state.clips[0].column == 1);
        CHECK(state.clips[0].frame == 30);
        CHECK(state.pilots.empty());
    }
}

TEST_CASE("a resumed show is held black until its clip is decoded mid-way, then shows where the beat clock has got to")
{
    TempDir media;
    TempDir library;
    azl::Composition saved = savedShow(media.path, library.path);
    saved.layers[0].autopilot.mode = AutopilotMode::Off;
    azl::EngineConfig config = kSize;
    config.holdStart = true;
    azl::EngineHarness harness(config, 60.0, library.path, saved);
    harness.holdClipOpens(true);
    harness.resume(6.0, {});

    harness.step(30);
    CHECK(harness.stats().starting);
    CHECK(harness.transport().starting);
    const azl::Image held = harness.readScreen();
    for (const auto& [x, y] : {std::pair{60u, kBitRow}, {240u, 405u}, {720u, 405u}})
        CHECK(pixel(held, x, y)[0] == 0);

    // The other columns' clips are preloaded too before the hold ends.
    harness.holdClipOpens(false);
    const auto until = Clock::now() + std::chrono::seconds(20);
    while (harness.stats().starting && Clock::now() < until) {
        harness.step();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const azl::TransportState state = harness.transport();
    REQUIRE_FALSE(state.starting);
    CHECK(state.startedAt == doctest::Approx(state.time));
    CHECK(state.beat > 7.0 - 1e-6);
    // The clip loops 60 frames at 30 fps, 15 frames a beat at 120 BPM, from beat 0.
    const int expected = int(std::floor(state.beat * 15.0 + 1e-6)) % 60;
    REQUIRE(state.clips.size() == 1);
    CHECK(state.clips[0].frame == expected);
    CHECK(shownFrame(harness.readScreen()) == expected);
    CHECK(harness.stats().videoMisses == 0);
}

// The supervisor and engine as processes, the way the Mac mini runs them.
namespace {

// Talks to an engine process over the protocol, reconnecting while it starts.
class Remote {
public:
    explicit Remote(uint16_t port)
    {
        REQUIRE_MESSAGE(eventually(
                            [&] {
                                try {
                                    client_ = std::make_unique<azl::WebSocketClient>(
                                        "127.0.0.1", port, azl::Encoding::Json, [this](const Message& m) { push(m); });
                                    return true;
                                } catch (const std::exception&) {
                                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                                    return false;
                                }
                            },
                            15.0),
                        "cannot connect to the engine");
        client_->send(msg::hello());
        snapshot = waitFor([](const Message& m) { return m["type"] == "snapshot"; });
    }

    void send(const Message& message) { client_->send(message); }

    Message request(const Message& message, const std::function<bool(const Message&)>& reply)
    {
        client_->send(message);
        return waitFor(reply);
    }

    Message edit(const azl::Edit& edit)
    {
        return request(msg::command(edit), [](const Message& m) { return m["type"] == "change"; });
    }

    std::string add(const fs::path& path)
    {
        const Message added = request({{"type", "library"}, {"op", "add"}, {"path", path.string()}},
                                      [](const Message& m) { return m["type"] == "library"; });
        return added["entry"]["hash"];
    }

    Message clock()
    {
        return request({{"type", "clock"}, {"op", "state"}},
                       [](const Message& m) { return m["type"] == "clock" && m["op"] == "state"; });
    }

    std::vector<Message> errors()
    {
        std::lock_guard lock(mutex_);
        return errors_;
    }

    Message snapshot;

private:
    Message waitFor(const std::function<bool(const Message&)>& match)
    {
        std::unique_lock lock(mutex_);
        while (true) {
            REQUIRE_MESSAGE(arrived_.wait_for(lock, std::chrono::seconds(10), [&] { return !queue_.empty(); }),
                            "no reply");
            Message m = std::move(queue_.front());
            queue_.pop_front();
            if (match(m))
                return m;
        }
    }

    void push(const Message& message)
    {
        std::lock_guard lock(mutex_);
        if (message["type"] == "error")
            errors_.push_back(message);
        queue_.push_back(message);
        arrived_.notify_all();
    }

    std::mutex mutex_;
    std::condition_variable arrived_;
    std::deque<Message> queue_;
    std::vector<Message> errors_;
    std::unique_ptr<azl::WebSocketClient> client_;
};

std::string supervisorPath()
{
    const char* path = std::getenv("NOCTURNE_SUPERVISOR");
    return path ? path : NOCTURNE_SUPERVISOR;
}

// A port nobody listens on right now, for an engine the test starts.
uint16_t freePort()
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{.sin_family = AF_INET, .sin_addr = {htonl(INADDR_LOOPBACK)}};
    REQUIRE(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
    socklen_t size = sizeof address;
    getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size);
    close(fd);
    return ntohs(address.sin_port);
}

class SupervisedEngine {
public:
    explicit SupervisedEngine(std::vector<std::string> extra = {}, std::string crashEnv = {}) : port(freePort())
    {
        args_ = {supervisorPath(), "--state", (root.path / "state").string(), "--logs", (root.path / "logs").string(),
                 "--desk-port", "0", "--desk-loopback"};
        args_.insert(args_.end(), extra.begin(), extra.end());
        args_.insert(args_.end(), {"--", "--offscreen", "--size", "320x180", "--audio-input", "off", "--port",
                                   std::to_string(port), "--library", (root.path / "library").string(), "--seed", "7"});
        for (char** e = environ; *e; ++e)
            env_.push_back(*e);
        if (!crashEnv.empty())
            env_.push_back(crashEnv);
        spawn();
        REQUIRE(waitForRunning(1));
    }

    ~SupervisedEngine() { stop(); }

    // Stops the supervisor and starts a new one on the same state, as launchd does on a restart or a deploy; `engine`
    // adds flags to the engine's command line.
    void restartSupervisor(const std::vector<std::string>& engine = {})
    {
        stop();
        args_.insert(args_.end(), engine.begin(), engine.end());
        spawn();
    }

    azl::Message status() const
    {
        const Message s = Message::parse(readFile(root.path / "state" / "status.json"), nullptr, false);
        return s.is_object() ? s : Message::object();
    }

    pid_t engine() const { return status().value("engine", 0); }

    bool waitForStart(int starts, double seconds = 15.0) const
    {
        return eventually([&] { return status().value("starts", 0) >= starts && engine() > 0; }, seconds);
    }

    std::string log() const { return readFile(root.path / "logs" / "supervisor.log"); }
    std::string engineLog() const { return readFile(root.path / "logs" / "engine.log"); }

    // Waits until the engine has rendered its first frame `runs` times, so the protocol is up and a connection never
    // waits for a port nobody listens on.
    bool waitForRunning(int runs, double seconds = 15.0) const
    {
        return eventually(
            [&] {
                const std::string text = log();
                int count = 0;
                for (size_t at = text.find("engine running"); at != std::string::npos;
                     at = text.find("engine running", at + 1))
                    ++count;
                return count >= runs;
            },
            seconds);
    }

    TempDir root;
    uint16_t port;

private:
    void spawn()
    {
        std::vector<char*> argv, envp;
        for (std::string& a : args_)
            argv.push_back(a.data());
        argv.push_back(nullptr);
        for (std::string& e : env_)
            envp.push_back(e.data());
        envp.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, 2, (root.path / "stderr.txt").c_str(), O_WRONLY | O_CREAT, 0644);
        REQUIRE(posix_spawn(&supervisor_, argv[0], &actions, nullptr, argv.data(), envp.data()) == 0);
        posix_spawn_file_actions_destroy(&actions);
    }

    void stop()
    {
        ::kill(supervisor_, SIGTERM);
        int status = 0;
        ::waitpid(supervisor_, &status, 0);
    }

    std::vector<std::string> args_;
    std::vector<std::string> env_;
    pid_t supervisor_ = 0;
};

const Message* layerClip(const Message& state)
{
    for (const Message& clip : state["clips"])
        if (clip["layer"] == 1)
            return &clip;
    return nullptr;
}

// Waits until the engine shows layer 1's clip, past the start-up hold, and returns the clock state then.
Message playing(Remote& remote, double seconds = 10.0)
{
    Message state;
    REQUIRE(eventually(
        [&] {
            state = remote.clock();
            const Message* clip = layerClip(state);
            return clip && (*clip)["frame"].get<int>() > 0 && !state["starting"].get<bool>();
        },
        seconds));
    return state;
}

// Kills or stops the engine and checks that its first frame after the start-up hold, the first correct frame, comes
// within `limit` seconds, on the same clip and beat grid.
void checkRestart(SupervisedEngine& engine, int sig, double limit)
{
    std::optional<Remote> remote(std::in_place, engine.port);
    const Message before = playing(*remote);
    const auto at = Clock::now();
    const double beatAt = before["beat"];
    const uint64_t column = (*layerClip(before))["column"];
    const pid_t old = engine.engine();
    REQUIRE(::kill(old, sig) == 0);
    if (sig == SIGKILL)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    remote.reset();

    // A ceiling, not the limit: a slow restart fails on the time it took, below.
    REQUIRE(engine.waitForRunning(2, 30.0));
    remote.emplace(engine.port);
    const Message after = playing(*remote);
    // The reply comes a little after the last frame, so this errs late.
    const double sinceShown = after["time"].get<double>() - after["startedAt"].get<double>();
    const double back = std::chrono::duration<double>(Clock::now() - at).count() - sinceShown;
    MESSAGE("first correct frame " << back << " s after signal " << sig);
    CHECK(back < limit);
    CHECK(::kill(old, 0) != 0);
    CHECK((*layerClip(after))["column"] == column);
    const double expected = beatAt + std::chrono::duration<double>(Clock::now() - at).count() * 2.0;
    CHECK(std::abs(after["beat"].get<double>() - expected) < 0.5);
    CHECK(engine.engineLog().find("trying again") == std::string::npos);
}

void startClips(Remote& remote, const fs::path& media, int clips)
{
    const char* files[] = {"count_240.mp4", "count_709_tv.mp4", "count_601_pc.mp4"};
    for (int i = 0; i < clips; ++i) {
        const fs::path copy = copyMedia(files[i], media / files[i]);
        REQUIRE(remote.edit(azl::SetSlot{1, uint64_t(i + 1), ClipRef{remote.add(copy), files[i], {}}})["op"] ==
                "setSlot");
    }
    remote.request({{"type", "trigger"}, {"op", "column"}, {"column", 1}},
                   [](const Message& m) { return m["type"] == "change" && m["op"] == "triggerColumn"; });
}

} // namespace

TEST_CASE("a killed engine restarts and resumes the same clip on the same beat grid")
{
    SupervisedEngine engine;
    INFO(engine.log());
    {
        Remote remote(engine.port);
        startClips(remote, engine.root.path / "media", 1);
    }
    checkRestart(engine, SIGKILL, 2.0);
    CHECK(engine.log().find("engine killed by SIGKILL") != std::string::npos);
    CHECK(engine.status()["mode"] == "normal");
}

TEST_CASE("a hung engine is killed after 2 s without a frame and resumes")
{
    SupervisedEngine engine;
    INFO(engine.log());
    {
        Remote remote(engine.port);
        startClips(remote, engine.root.path / "media", 1);
    }
    checkRestart(engine, SIGSTOP, 4.0);
    CHECK(engine.log().find("engine hung: no frame for 2.") != std::string::npos);
}

TEST_CASE("Autopilot resumes on the clip it had stepped to")
{
    SupervisedEngine engine;
    INFO(engine.log());
    {
        Remote remote(engine.port);
        startClips(remote, engine.root.path / "media", 3);
        REQUIRE(remote.edit(azl::SetLayerAutopilot{1, AutopilotMode::Next, 2})["op"] == "setLayerAutopilot");
        // Wait until Autopilot has moved on to column 2, early in its step, so the restart lands in the same step.
        REQUIRE(eventually(
            [&] {
                const Message state = remote.clock();
                const Message* clip = layerClip(state);
                return clip && (*clip)["column"] == 2 && (*clip)["frame"].get<int>() > 0;
            },
            15.0));
    }
    checkRestart(engine, SIGKILL, 2.0);
}

TEST_CASE("the audio input picked at runtime is open again after a restart")
{
    SupervisedEngine engine;
    INFO(engine.log());
    const auto input = [](Remote& remote) {
        return remote.request({{"type", "audio"}, {"op", "input"}},
                              [](const Message& m) { return m["type"] == "audio" && m["op"] == "input"; });
    };
    {
        Remote remote(engine.port);
        startClips(remote, engine.root.path / "media", 1);
        // Other channels than the command line's, still "off": a choice that opens no device on a test machine.
        remote.send({{"type", "audio"}, {"op", "select"}, {"device", "off"}, {"channels", Message::array({2})}});
        REQUIRE(eventually([&] { return input(remote)["channels"] == Message::array({2}); }, 5.0));
    }
    checkRestart(engine, SIGKILL, 2.0);
    Remote remote(engine.port);
    CHECK(input(remote)["channels"] == Message::array({2}));
    CHECK(engine.engineLog().find("audio: reopening 'off', the input open before the restart") != std::string::npos);
}

TEST_CASE("the audio input picked at runtime is open again after the supervisor restarts with the same flags")
{
    SupervisedEngine engine;
    INFO(engine.log());
    const auto input = [](Remote& remote) {
        return remote.request({{"type", "audio"}, {"op", "input"}},
                              [](const Message& m) { return m["type"] == "audio" && m["op"] == "input"; });
    };
    const auto savedChannels = [&] {
        const Message file = Message::parse(readFile(engine.root.path / "state" / "composition.json"), nullptr, false);
        const Message::json_pointer channels("/resume/audio/channels");
        return file.is_object() && file.contains(channels) ? file[channels] : Message();
    };
    {
        Remote remote(engine.port);
        startClips(remote, engine.root.path / "media", 1);
        remote.send({{"type", "audio"}, {"op", "select"}, {"device", "off"}, {"channels", Message::array({2})}});
        REQUIRE(eventually([&] { return input(remote)["channels"] == Message::array({2}); }, 5.0));
        REQUIRE(eventually([&] { return savedChannels() == Message::array({2}); }, 5.0));
    }
    engine.restartSupervisor();
    REQUIRE(engine.waitForRunning(2));
    Remote remote(engine.port);
    CHECK(input(remote)["channels"] == Message::array({2}));
    CHECK(engine.engineLog().find("audio: reopening 'off', the input open before the restart") != std::string::npos);
}

TEST_CASE("an engine a test starts keeps its audio input off, even when a device is selected")
{
    if (!azl::AudioInput::disabled())
        return;
    SupervisedEngine engine;
    INFO(engine.log());
    Remote remote(engine.port);
    const auto input = [&] {
        return remote.request({{"type", "audio"}, {"op", "input"}},
                              [](const Message& m) { return m["type"] == "audio" && m["op"] == "input"; });
    };
    CHECK(input()["state"] == "off");
    remote.send({{"type", "audio"}, {"op", "select"}, {"device", ""}});
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const Message after = input();
    CHECK(after["state"] == "off");
    CHECK(after["requested"] == "off");
    CHECK(engine.engineLog().find("NOCTURNE_NO_AUDIO_INPUT is set") != std::string::npos);
}

TEST_CASE("three crashes within a minute start safe mode with the reason")
{
    SupervisedEngine engine;
    INFO(engine.log());
    INFO(engine.engineLog());
    const auto running = [&] {
        size_t n = 0;
        const std::string log = engine.log();
        for (size_t at = log.find("engine running"); at != std::string::npos; at = log.find("engine running", at + 1))
            ++n;
        return n;
    };
    for (int crash = 1; crash <= 3; ++crash) {
        CAPTURE(crash);
        // The crash-th engine has drawn its first frame: its "engine running" line, not an earlier engine's.
        REQUIRE(eventually([&] { return running() >= size_t(crash) && engine.status()["starts"] == crash; }, 15.0));
        const pid_t pid = engine.engine();
        REQUIRE(pid > 0);
        if (::kill(pid, SIGKILL) != 0)
            FAIL("engine " << pid << " of start " << crash << " could not be killed: " << std::strerror(errno)
                           << "\n" << engine.log());
        REQUIRE(eventually([&] { return engine.status()["starts"] == crash + 1 || engine.status()["mode"] == "safe"; },
                           10.0));
    }
    REQUIRE(eventually([&] { return engine.status()["mode"] == "safe"; }, 5.0));
    const Message status = engine.status();
    CHECK(status["reason"].get<std::string>().find("3 crashes within 60 s") != std::string::npos);
    CHECK(status["starts"] == 3);
    CHECK(engine.log().find("safe mode started") != std::string::npos);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    CHECK(engine.status()["starts"] == 3);
    CHECK(readFile(engine.root.path / "logs" / "engine.log").find("safe mode: 3 crashes") != std::string::npos);
}

TEST_CASE("a clip that crashed the engine while opening is on the skip list and skipped after the restart")
{
    SupervisedEngine engine({}, "NOCTURNE_CRASH_ON=clip:poison.mp4");
    INFO(engine.log());
    INFO(engine.engineLog());
    std::string poison;
    {
        Remote remote(engine.port);
        startClips(remote, engine.root.path / "media", 1);
        playing(remote);
        poison = remote.add(copyMedia("count_100.mp4", engine.root.path / "media" / "poison.mp4"));
        remote.send(msg::command(azl::SetSlot{1, 2, ClipRef{poison, "poison.mp4", {}}}));
        REQUIRE(engine.waitForRunning(2, 10.0));
    }
    const std::vector<azl::crash::Skip> skips = azl::crash::readSkips(engine.root.path / "state" / "skip.json");
    REQUIRE(skips.size() == 1);
    CHECK(skips[0].mark.key == poison);
    CHECK(skips[0].mark.name == "poison.mp4");
    CHECK(engine.log().find("while opening clip poison.mp4") != std::string::npos);

    Remote remote(engine.port);
    playing(remote);
    remote.edit(azl::SetSlot{1, 2, ClipRef{poison, "poison.mp4", {}}});
    remote.request({{"type", "trigger"}, {"op", "column"}, {"column", 2}},
                   [](const Message& m) { return m["type"] == "change" && m["op"] == "triggerColumn"; });
    std::this_thread::sleep_for(std::chrono::seconds(3));
    CHECK(engine.status()["starts"] == 2);
    CHECK(engine.status()["crashes"].size() == 1);
    const auto errors = remote.errors();
    CHECK(std::any_of(errors.begin(), errors.end(), [](const Message& e) {
        return e.dump().find("skipped") != std::string::npos;
    }));
}

TEST_CASE("crashes on another thread while a clip opens still start safe mode, and skip-list nothing")
{
    SupervisedEngine engine({}, "NOCTURNE_CRASH_ELSEWHERE_ON=clip:poison.mp4");
    INFO(engine.log());
    INFO(engine.engineLog());
    std::string poison;
    for (int crash = 1; crash <= 3; ++crash) {
        CAPTURE(crash);
        REQUIRE(engine.waitForRunning(crash, 15.0));
        Remote remote(engine.port);
        if (poison.empty())
            poison = remote.add(copyMedia("count_100.mp4", engine.root.path / "media" / "poison.mp4"));
        remote.send(msg::command(azl::SetSlot{1, 1, ClipRef{poison, "poison.mp4", {}}}));
        REQUIRE(eventually([&] { return engine.status()["crashes"].size() >= size_t(crash); }, 10.0));
    }
    REQUIRE(eventually([&] { return engine.status()["mode"] == "safe"; }, 5.0));
    CHECK(engine.status()["reason"].get<std::string>().find("3 crashes within 60 s") != std::string::npos);
    CHECK(azl::crash::readSkips(engine.root.path / "state" / "skip.json").empty());
    CHECK(engine.log().find("skip list: not adding opening clip poison.mp4") != std::string::npos);
    CHECK(engine.log().find("skip list: added") == std::string::npos);
}

namespace {

// Sends a machine action and returns its reply or the error it got.
Message machine(Remote& remote, Message action)
{
    action["type"] = "machine";
    return remote.request(action, [op = action["op"]](const Message& m) {
        return (m["type"] == "machine" && m["op"] == op) || m["type"] == "error";
    });
}

bool refusedByLock(const Message& reply)
{
    return reply["type"] == "error" && reply["message"].get<std::string>().find("Show lock is on") != std::string::npos;
}

} // namespace

TEST_CASE("a restart asked for over the protocol comes back with the same show, and is not a crash")
{
    SupervisedEngine engine;
    INFO(engine.log());
    uint64_t column = 0;
    {
        Remote remote(engine.port);
        startClips(remote, engine.root.path / "media", 2);
        remote.request({{"type", "trigger"}, {"op", "column"}, {"column", 2}},
                       [](const Message& m) { return m["type"] == "change" && m["op"] == "triggerColumn"; });
        column = (*layerClip(playing(remote)))["column"];
        CHECK(machine(remote, {{"op", "restart"}}) == Message{{"type", "machine"}, {"op", "restart"}});
    }
    REQUIRE(engine.waitForRunning(2));
    Remote remote(engine.port);
    CHECK((*layerClip(playing(remote)))["column"] == column);
    CHECK(engine.log().find("engine restart asked for from a client") != std::string::npos);
    CHECK(engine.status()["mode"] == "normal");
    CHECK(engine.status()["crashes"].empty());
}

TEST_CASE("safe mode asked for over the protocol is entered, and left again by a restart")
{
    SupervisedEngine engine;
    INFO(engine.log());
    {
        Remote remote(engine.port);
        startClips(remote, engine.root.path / "media", 1);
        playing(remote);
        CHECK(machine(remote, {{"op", "safeMode"}})["op"] == "safeMode");
    }
    REQUIRE(eventually([&] { return engine.status()["mode"] == "safe" && engine.engine() > 0; }, 10.0));
    CHECK(engine.status()["reason"] == "asked for from a client");
    {
        Remote safe(engine.port);
        CHECK(safe.snapshot["type"] == "snapshot");
        const Message status = machine(safe, {{"op", "status"}});
        CHECK(status["mode"] == "safe");
        CHECK(status["reason"] == "asked for from a client");
        CHECK(machine(safe, {{"op", "safeMode"}})["type"] == "error");
        CHECK(machine(safe, {{"op", "restart"}})["op"] == "restart");
    }
    REQUIRE(engine.waitForRunning(2, 20.0));
    CHECK(engine.status()["mode"] == "normal");
    CHECK(engine.log().find("leaving safe mode, asked for from a client") != std::string::npos);
    Remote remote(engine.port);
    CHECK(machine(remote, {{"op", "status"}})["mode"] == "normal");
    playing(remote);
}

TEST_CASE("the Show lock refuses restart and safe mode for every client and survives a crash of the engine")
{
    SupervisedEngine engine;
    INFO(engine.log());
    {
        Remote director(engine.port);
        Remote other(engine.port);
        CHECK(machine(director, {{"op", "lock"}, {"on", true}})["on"] == true);
        CHECK(refusedByLock(machine(other, {{"op", "restart"}})));
        CHECK(refusedByLock(machine(other, {{"op", "safeMode"}})));
        CHECK(refusedByLock(machine(director, {{"op", "restart"}})));
    }
    const pid_t old = engine.engine();
    REQUIRE(::kill(old, SIGKILL) == 0);
    REQUIRE(engine.waitForRunning(2));
    {
        Remote remote(engine.port);
        CHECK(machine(remote, {{"op", "status"}})["showLock"] == true);
        CHECK(refusedByLock(machine(remote, {{"op", "restart"}})));
        CHECK(machine(remote, {{"op", "lock"}, {"on", false}})["on"] == false);
        CHECK(machine(remote, {{"op", "restart"}})["op"] == "restart");
    }
    REQUIRE(engine.waitForRunning(3));
    CHECK(engine.status()["mode"] == "normal");
}

TEST_CASE("the recording settings set at runtime are kept across engine and supervisor restarts; a flag overrides them")
{
    SupervisedEngine engine;
    INFO(engine.log());
    const fs::path folder = engine.root.path / "recordings";
    const fs::path other = engine.root.path / "other";
    fs::create_directories(folder);
    fs::create_directories(other);
    const auto record = [](Remote& remote, Message request) {
        request["type"] = "record";
        return remote.request(request, [op = request["op"]](const Message& m) {
            return m["type"] == "error" || (m["type"] == "record" && m["op"] == op);
        });
    };
    {
        Remote remote(engine.port);
        REQUIRE(record(remote, {{"op", "set"}, {"path", folder.string()}, {"codec", "prores"}, {"maxSeconds", 600},
                                {"reserveMB", 1024}, {"audio", false}, {"target", "screen:1"},
                                {"maxTotalSeconds", 3600}, {"gigGraceSeconds", 600}})["type"] == "record");
        // One recording's file is not kept: the folder stays.
        REQUIRE(record(remote, {{"op", "set"}, {"path", (other / "once.mov").string()}})["type"] == "record");
    }
    REQUIRE(::kill(engine.engine(), SIGKILL) == 0);
    REQUIRE(engine.waitForRunning(2));
    {
        Remote remote(engine.port);
        const Message state = record(remote, {{"op", "state"}});
        CHECK(state["path"] == folder.string());
        CHECK(state["folder"] == folder.string());
        CHECK(state["folderProblem"] == "");
        CHECK(state["codec"] == "prores");
        CHECK(state["maxSeconds"] == 600.0);
        CHECK(state["maxTotalSeconds"] == 3600.0);
        CHECK(state["gigGraceSeconds"] == 600.0);
        CHECK(state["reserveMB"] == 1024);
        CHECK(state["audio"] == "off");
        CHECK(state["target"] == "composition");
    }

    engine.restartSupervisor({"--record-path", other.string(), "--record-reserve", "512"});
    REQUIRE(engine.waitForRunning(3));
    Remote remote(engine.port);
    const Message state = record(remote, {{"op", "state"}});
    CHECK(state["path"] == other.string());
    CHECK(state["reserveMB"] == 512);
    CHECK(state["codec"] == "prores");
}

TEST_CASE("the venue desk runs beside the engine; killed, it comes back and the engine runs on")
{
    SupervisedEngine engine;
    INFO(engine.log());
    const fs::path pidFile = engine.root.path / "state" / "desk.pid";
    const auto deskPid = [&] { return pid_t(std::atoi(readFile(pidFile).c_str())); };
    // The port of the newest desk, from its log.
    const auto deskPort = [&] {
        const std::string log = readFile(engine.root.path / "logs" / "desk.log");
        const size_t at = log.rfind("listening on 127.0.0.1:");
        return uint16_t(at == std::string::npos ? 0 : std::atoi(log.c_str() + at + 23));
    };
    REQUIRE(eventually([&] { return deskPid() > 0 && deskPort() > 0; }, 15.0));
    HttpReply reply;
    REQUIRE(eventually([&] { return (reply = httpGet(deskPort(), "/state")).status == 200; }, 10.0));
    CHECK(Message::parse(reply.body)["connected"] == true);

    const pid_t first = deskPid();
    const pid_t engineBefore = engine.engine();
    REQUIRE(::kill(first, SIGKILL) == 0);
    REQUIRE(eventually([&] { return deskPid() > 0 && deskPid() != first; }, 10.0));
    REQUIRE(eventually([&] { return httpGet(deskPort(), "/state").status == 200; }, 10.0));
    CHECK(engine.engine() == engineBefore);
    CHECK(engine.status()["starts"] == 1);
    CHECK(engine.log().find("desk killed by SIGKILL, restarting") != std::string::npos);
}
