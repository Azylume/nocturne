// nocturne-supervisor around the fake engine (tests/fake_engine.cpp), on every platform: restarts, hangs, crash
// counting, safe mode, the skip list and stopping, with no GPU or display. supervisor_test.cpp runs the real engine;
// on Windows the last case here does, offscreen.
#include "kill_process.hpp"
#include "scoped_env.hpp"
#include "temp_folder.hpp"

#include "document/composition_file.hpp"
#include "platform/process.hpp"
#include "supervisor/crash_guard.hpp"

#include <doctest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

std::string readFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
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

size_t count(const std::string& text, const std::string& part)
{
    size_t n = 0;
    for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1))
        ++n;
    return n;
}

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

// nocturne-supervisor running the fake engine, its state and logs in a folder of their own, no desk unless `extra`
// names one.
class Supervised {
public:
    explicit Supervised(const std::vector<std::string>& extra = {}, azl::Stdio stdio = azl::Stdio::null(),
                        bool waitForFirstFrame = true)
    {
        std::vector<std::string> args = {"--engine", NOCTURNE_FAKE_ENGINE, "--state", state().string(), "--logs",
                                         (root.path / "logs").string(), "--desk", "off"};
        args.insert(args.end(), extra.begin(), extra.end());
        std::string error;
        supervisor_ = azl::spawnProcess({.program = NOCTURNE_SUPERVISOR,
                                         .args = args,
                                         .input = stdio,
                                         .output = stdio,
                                         .error = stdio},
                                        &error);
        REQUIRE_MESSAGE(supervisor_, error);
        const bool ready = !waitForFirstFrame || waitForRunning(1);
        REQUIRE_MESSAGE(ready, log());
    }

    ~Supervised() { stop(); }

    // Asks the supervisor to stop, as the autostart job does, and returns its exit status.
    int stop()
    {
        supervisor_->terminate();
        return supervisor_->wait();
    }

    fs::path state() const { return root.path / "state"; }

    nlohmann::json status() const
    {
        const nlohmann::json s = nlohmann::json::parse(readFile(state() / "status.json"), nullptr, false);
        return s.is_object() ? s : nlohmann::json::object();
    }

    int engine() const { return status().value("engine", 0); }
    int starts() const { return status().value("starts", 0); }
    std::string mode() const { return status().value("mode", ""); }
    nlohmann::json retry() const { return status().value("retry", nlohmann::json()); }
    std::string log() const { return readFile(root.path / "logs" / "supervisor.log"); }
    std::string engineLog() const { return readFile(root.path / "logs" / "engine.log"); }

    // Waits until the engine has beaten for the first time `runs` times.
    bool waitForRunning(int runs, double seconds = 15.0) const
    {
        return eventually([&] { return count(log(), "engine running") >= size_t(runs) && engine() > 0; }, seconds);
    }

    // The next command the fake engine runs.
    void command(const std::string& line) const { azl::writeFileAtomically(state() / "command", line + "\n", false); }

    azl::Process& process() { return *supervisor_; }

    TempFolder root;

private:
    std::optional<azl::Process> supervisor_;
};

} // namespace

TEST_CASE("supervisor: a killed engine is restarted, and the log says how it ended")
{
    Supervised supervised;
    INFO(supervised.log());
    const int first = supervised.engine();
    killProcess(first);
    REQUIRE(supervised.waitForRunning(2));
    CHECK(supervised.engine() != first);
    CHECK(contains(supervised.log(), std::string("engine ") + kKilled));
    CHECK(supervised.mode() == "normal");
    CHECK(supervised.starts() == 2);
    CHECK(supervised.status()["crashes"].size() == 1);
}

TEST_CASE("supervisor: an engine that stops beating is killed after the hang timeout and restarted")
{
    Supervised supervised;
    INFO(supervised.log());
    supervised.command("hang");
    REQUIRE(supervised.waitForRunning(2, 10.0));
    CHECK(contains(supervised.log(), "engine hung: no frame for 2."));
    CHECK(supervised.mode() == "normal");
}

TEST_CASE("supervisor: three crashes within a minute start safe mode with the reason")
{
    Supervised supervised;
    INFO(supervised.log());
    for (int crash = 1; crash <= 3; ++crash) {
        CAPTURE(crash);
        REQUIRE(eventually([&] { return supervised.waitForRunning(crash, 0.0) && supervised.starts() == crash; }, 15.0));
        killProcess(supervised.engine());
        REQUIRE(eventually([&] { return supervised.starts() == crash + 1 || supervised.mode() == "safe"; }, 10.0));
    }
    REQUIRE(eventually([&] { return supervised.mode() == "safe"; }, 5.0));
    CHECK(contains(supervised.status()["reason"].get<std::string>(), "3 crashes within 60 s"));
    CHECK(supervised.starts() == 3);
    REQUIRE(eventually([&] { return contains(supervised.engineLog(), "safe mode: 3 crashes"); }, 5.0));
    CHECK(contains(supervised.log(), "safe mode started"));
    std::this_thread::sleep_for(std::chrono::seconds(1));
    CHECK(supervised.starts() == 3);
    CHECK(supervised.retry().is_null());
}

namespace {

// The retry cool-down of the cases below: 0.6 s, then 1.2 s and 2.4 s.
const std::vector<std::string> kQuickRetry = {"--safe-mode-retry", "0.01"};

bool retryIs(const Supervised& supervised, const char* key, const nlohmann::json& value)
{
    return supervised.retry().value(key, nlohmann::json()) == value;
}

} // namespace

TEST_CASE("supervisor: safe mode that crashes caused ends after the cool-down and the engine runs on")
{
    ScopedEnv crashes("NOCTURNE_FAKE_ENGINE_DO", "crash-first 3");
    Supervised supervised(kQuickRetry, azl::Stdio::null(), false);
    INFO(supervised.log());
    REQUIRE(eventually([&] { return supervised.mode() == "normal" && retryIs(supervised, "attempts", 1) &&
                                    supervised.engine() > 0; }, 20.0));
    CHECK(contains(supervised.log(), "safe mode: retry in 0.01 min (attempt 1 of 3)"));
    CHECK(contains(supervised.log(), "safe mode: retrying the engine"));
    CHECK(contains(supervised.engineLog(), "safe retry at "));
    CHECK(supervised.starts() == 4);
    CHECK(retryIs(supervised, "limit", 3));
    CHECK(supervised.retry()["nextAt"].is_null());
    CHECK(supervised.retry()["startedAt"].size() == 1);
}

TEST_CASE("supervisor: a retry that crashes goes straight back to safe mode, the cool-down doubles, three retries end it")
{
    ScopedEnv crashes("NOCTURNE_FAKE_ENGINE_DO", "crash-first 100");
    Supervised supervised(kQuickRetry, azl::Stdio::null(), false);
    INFO(supervised.log());
    REQUIRE(eventually([&] { return contains(supervised.log(), "retry cap reached"); }, 30.0));
    CHECK(contains(supervised.log(), "safe mode: retry failed, back in safe mode, next in 0.02 min (attempt 2 of 3)"));
    CHECK(contains(supervised.log(), "safe mode: retry failed, back in safe mode, next in 0.04 min (attempt 3 of 3)"));
    CHECK(count(supervised.log(), "safe mode: retrying the engine") == 3);
    CHECK(contains(supervised.status()["reason"].get<std::string>(), "crashed within 10 min of leaving safe mode"));
    CHECK(supervised.mode() == "safe");
    CHECK(supervised.starts() == 6);
    CHECK(retryIs(supervised, "attempts", 3));
    CHECK(retryIs(supervised, "last", "failed"));
    CHECK(supervised.retry()["nextAt"].is_null());
    std::this_thread::sleep_for(std::chrono::seconds(3));
    CHECK(supervised.starts() == 6);
}

TEST_CASE("supervisor: safe mode a client asked for is never retried")
{
    Supervised supervised(kQuickRetry);
    INFO(supervised.log());
    supervised.command("exit 76");
    REQUIRE(eventually([&] { return supervised.mode() == "safe" && supervised.engine() > 0; }, 10.0));
    CHECK(supervised.retry()["nextAt"].is_null());
    std::this_thread::sleep_for(std::chrono::seconds(3));
    CHECK(supervised.mode() == "safe");
    CHECK(supervised.starts() == 1);
    CHECK_FALSE(contains(supervised.log(), "retrying the engine"));
}

TEST_CASE("supervisor: stopping it during a cool-down exits cleanly, and the status says when the retry is due")
{
    ScopedEnv crashes("NOCTURNE_FAKE_ENGINE_DO", "crash-first 100");
    Supervised supervised({"--safe-mode-retry", "10"}, azl::Stdio::null(), false);
    INFO(supervised.log());
    REQUIRE(eventually([&] { return supervised.mode() == "safe" && supervised.engine() > 0; }, 10.0));
    const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    REQUIRE(supervised.retry()["nextAt"].is_number());
    CHECK(supervised.retry()["nextAt"].get<double>() == doctest::Approx(now + 600.0).epsilon(0.05));
    CHECK(contains(supervised.engineLog(), "safe retry at " + std::to_string(supervised.retry()["nextAt"].get<long long>())));
    CHECK(supervised.stop() == 0);
    CHECK(contains(supervised.log(), "supervisor stopped"));
}

TEST_CASE("supervisor: the retries of a supervisor that was restarted still count against the daily cap")
{
    ScopedEnv crashes("NOCTURNE_FAKE_ENGINE_DO", "crash-first 100");
    Supervised first(kQuickRetry, azl::Stdio::null(), false);
    REQUIRE_MESSAGE(eventually([&] { return contains(first.log(), "retry cap reached"); }, 30.0), first.log());
    CHECK(first.stop() == 0);

    Supervised second({"--safe-mode-retry", "0.01", "--state", first.state().string()}, azl::Stdio::null(), false);
    INFO(second.log());
    // Both supervisors write the state folder of the first.
    REQUIRE(eventually([&] { return contains(second.log(), "retry cap reached") && first.mode() == "safe"; }, 15.0));
    CHECK_FALSE(contains(second.log(), "safe mode: retry in"));
    CHECK(retryIs(first, "attempts", 3));
    CHECK(first.retry()["nextAt"].is_null());
}

TEST_CASE("supervisor: a clip the engine faulted on while opening it goes on the skip list")
{
    Supervised supervised;
    INFO(supervised.log());
    supervised.command("fault");
    REQUIRE(supervised.waitForRunning(2));
    const std::vector<azl::crash::Skip> skips = azl::crash::readSkips(supervised.state() / "skip.json");
    REQUIRE(skips.size() == 1);
    CHECK(skips[0].mark.key == "poison");
    CHECK(skips[0].mark.name == "poison.mp4");
    CHECK(contains(supervised.log(), "while opening clip poison.mp4"));
#ifdef _WIN32
    CHECK(contains(supervised.log(), "engine crashed with exception 0xC0000005"));
#endif
}

TEST_CASE("supervisor: a fault on another thread while a clip opens skip-lists nothing")
{
    Supervised supervised;
    INFO(supervised.log());
    supervised.command("fault-elsewhere");
    REQUIRE(supervised.waitForRunning(2));
    CHECK(azl::crash::readSkips(supervised.state() / "skip.json").empty());
    CHECK(contains(supervised.log(), "skip list: not adding opening clip poison.mp4"));
}

TEST_CASE("supervisor: the engine's exit status asks for a restart, safe mode, leaving it, or a stop")
{
    Supervised supervised;
    INFO(supervised.log());
    supervised.command("exit 75");
    REQUIRE(supervised.waitForRunning(2));
    CHECK(contains(supervised.log(), "engine restart asked for from a client"));
    CHECK(supervised.status()["crashes"].empty());

    supervised.command("exit 76");
    REQUIRE(eventually([&] { return supervised.mode() == "safe" && supervised.engine() > 0; }, 10.0));
    CHECK(supervised.status()["reason"] == "asked for from a client");

    supervised.command("exit 75");
    REQUIRE(supervised.waitForRunning(3));
    CHECK(contains(supervised.log(), "leaving safe mode, asked for from a client"));
    CHECK(supervised.mode() == "normal");

    supervised.command("exit 77");
    REQUIRE(eventually([&] { return supervised.process().poll().has_value(); }, 10.0));
    CHECK(supervised.process().poll() == 0);
    CHECK(contains(supervised.log(), "stop asked for from a client; stopping"));
}

TEST_CASE("supervisor: an engine that finds no usable GPU is not restarted and its line goes into the status once")
{
    Supervised supervised;
    INFO(supervised.log());
    supervised.command("no-gpu");
    REQUIRE(eventually([&] { return supervised.process().poll().has_value(); }, 10.0));
    CHECK(supervised.process().poll() == 0);
    const std::string line = "nocturne: no usable GPU: fake has no Vulkan 1.3 device.";
    CHECK(supervised.status()["error"] == line);
    CHECK(supervised.mode() == "normal");
    CHECK(supervised.starts() == 1);
    CHECK(supervised.status()["crashes"].empty());
    CHECK(count(supervised.log(), line) == 1);
    CHECK(contains(supervised.engineLog(), line));
}

TEST_CASE("supervisor: stopping the supervisor asks the engine to quit, and both exit")
{
    Supervised supervised;
    INFO(supervised.log());
    CHECK(supervised.stop() == 0);
    CHECK(contains(supervised.engineLog(), "fake engine stopping"));
    CHECK(contains(supervised.log(), "engine exited with status 0"));
    CHECK(contains(supervised.log(), "supervisor stopped"));
    CHECK(supervised.engine() == 0);
}

TEST_CASE("supervisor: the venue desk runs beside the engine; killed, it comes back and the engine runs on")
{
    Supervised supervised({"--desk", NOCTURNE_DESK, "--desk-port", "0", "--desk-loopback"});
    INFO(supervised.log());
    const fs::path deskPid = supervised.state() / "desk.pid";
    REQUIRE(eventually([&] { return std::atoi(readFile(deskPid).c_str()) > 0; }, 10.0));
    const int engine = supervised.engine();
    killProcess(std::atoi(readFile(deskPid).c_str()));
    REQUIRE(eventually([&] { return count(supervised.log(), "desk started") >= 2; }, 10.0));
    CHECK(contains(supervised.log(), std::string("desk ") + kKilled + ", restarting"));
    CHECK(supervised.engine() == engine);
    CHECK(count(supervised.log(), "engine started") == 1);
}

#ifdef _WIN32
namespace {

WORD subsystem(const fs::path& binary)
{
    const std::string image = readFile(binary);
    LONG offset = 0;
    REQUIRE(image.size() > 0x40);
    std::memcpy(&offset, image.data() + 0x3C, sizeof offset);
    IMAGE_NT_HEADERS64 headers{};
    REQUIRE(image.size() >= size_t(offset) + sizeof headers);
    std::memcpy(&headers, image.data() + offset, sizeof headers);
    return headers.OptionalHeader.Subsystem;
}

// What `program --help` prints, read through a pipe as the supervisor reads the engine's and the desk's output.
std::string helpThroughPipe(const char* program)
{
    const std::optional<azl::PipeEnds> pipe = azl::makePipe();
    REQUIRE(pipe);
    std::string error;
    std::optional<azl::Process> child = azl::spawnProcess(
        {.program = program, .args = {"--help"}, .input = azl::Stdio::null(), .output = azl::Stdio::to(pipe->write)},
        &error);
    azl::closeHandle(pipe->write);
    REQUIRE_MESSAGE(child, error);
    std::string out;
    char buffer[4096];
    for (long n; (n = azl::readHandle(pipe->read, buffer, sizeof buffer)) > 0;)
        out.append(buffer, size_t(n));
    azl::closeHandle(pipe->read);
    CHECK(child->wait() == 0);
    return out;
}

} // namespace

TEST_CASE("supervisor: on Windows the supervisor, the desk and the engine open no console window, and their output "
          "reaches a pipe")
{
    for (const char* program : {NOCTURNE_SUPERVISOR, NOCTURNE_DESK, NOCTURNE_ENGINE}) {
        INFO(program);
        CHECK(subsystem(program) == IMAGE_SUBSYSTEM_WINDOWS_GUI);
        CHECK(contains(helpThroughPipe(program), "usage: nocturne"));
    }
}

TEST_CASE("supervisor: on Windows it runs and stops with no standard handles, as the autostart task starts it")
{
    // An invalid handle leaves the child's standard handle unset.
    Supervised supervised({"--desk", NOCTURNE_DESK, "--desk-port", "0", "--desk-loopback"},
                          azl::Stdio::to(INVALID_HANDLE_VALUE));
    INFO(supervised.log());
    REQUIRE(eventually([&] { return contains(supervised.log(), "desk started"); }, 10.0));
    CHECK(supervised.stop() == 0);
    CHECK(contains(supervised.log(), "supervisor stopped"));
}

TEST_CASE("supervisor: on Windows the real engine runs under it with no standard handles and logs to engine.log")
{
    Supervised supervised({"--engine", NOCTURNE_ENGINE, "--", "--offscreen", "--port", "7492", "--loopback"},
                          azl::Stdio::to(INVALID_HANDLE_VALUE));
    INFO(supervised.log());
    CHECK(supervised.stop() == 0);
    CHECK(contains(supervised.log(), "engine exited with status 0"));
    const std::string engineLog = supervised.engineLog();
    CHECK(contains(engineLog, "stopped"));
    CHECK_FALSE(contains(engineLog, "\r\r"));
    CHECK(count(engineLog, "\r\n") == count(engineLog, "\n"));
}
#endif

namespace {

// The arguments the fake engine got at its latest start.
std::string lastArgs(const Supervised& supervised)
{
    const std::string log = supervised.engineLog();
    const size_t at = log.rfind("fake engine args:");
    return at == std::string::npos ? "" : log.substr(at, log.find('\n', at) - at);
}

bool waitForArgs(const Supervised& supervised, size_t starts)
{
    return eventually([&] { return count(supervised.engineLog(), "fake engine args:") >= starts; }, 15.0);
}

} // namespace

TEST_CASE("supervisor: saved machine settings are read at each start and win over the launch flags")
{
    TempFolder home;
    fs::create_directories(home.path / "Nocturne" / "shows");
    ScopedEnv env("HOME", home.path.string().c_str());
    const std::string show = (home.path / "Nocturne" / "shows" / "Friday.json").string();
    Supervised supervised({"--", "--composition", "/launch/show.json", "--audio-input", "launchmic"});
    INFO(supervised.log());
    CHECK(lastArgs(supervised).find("--composition /launch/show.json --audio-input launchmic --state ") !=
          std::string::npos);
    CHECK(lastArgs(supervised).find(" --logs " + (supervised.root.path / "logs").string()) != std::string::npos);
    CHECK(supervised.status()["applied"].empty());
    CHECK(supervised.status()["startedAt"].get<double>() > 1.0e9);

    azl::writeFileAtomically(supervised.state() / "machine-settings.json",
                             nlohmann::json{{"composition", show}, {"audioInput", "savedmic"}}.dump());
    supervised.command("exit 0");
    REQUIRE(waitForArgs(supervised, 2));
    CHECK(lastArgs(supervised)
              .find("--composition /launch/show.json --audio-input launchmic --composition " + show +
                    " --audio-input savedmic --state ") != std::string::npos);
    CHECK(supervised.status()["applied"] == nlohmann::json::array({"composition", "audioInput"}));
    CHECK(contains(supervised.log(), "settings: composition " + show));

    azl::writeFileAtomically(supervised.state() / "machine-settings.json",
                             nlohmann::json{{"composition", show}, {"mediaRoot", "/etc"}}.dump());
    supervised.command("exit 0");
    REQUIRE(waitForArgs(supervised, 3));
    CHECK(lastArgs(supervised).find("--composition /launch/show.json --audio-input launchmic --state ") !=
          std::string::npos);
    CHECK(lastArgs(supervised).find("/etc") == std::string::npos);
    CHECK(contains(supervised.log(), "settings: ignored"));
    CHECK(supervised.status()["applied"].empty());

    azl::writeFileAtomically(supervised.state() / "machine-settings.json", "{ not json");
    supervised.command("exit 0");
    REQUIRE(waitForArgs(supervised, 4));
    CHECK(count(supervised.log(), "settings: ignored") == 2);
    CHECK(lastArgs(supervised).find("--composition /launch/show.json --audio-input launchmic --state ") !=
          std::string::npos);
}

TEST_CASE("supervisor: a saved setting that a path outside the shows folder names is never passed to the engine")
{
    TempFolder home;
    fs::create_directories(home.path / "Nocturne" / "shows");
    fs::create_directories(home.path / "outside");
    fs::create_symlink(home.path / "outside", home.path / "Nocturne" / "shows" / "escape");
    ScopedEnv env("HOME", home.path.string().c_str());
    Supervised supervised;
    for (const std::string& path : {(home.path / "Nocturne" / "shows" / "escape" / "a.json").string(),
                                    (home.path / "Nocturne" / "shows" / ".." / ".." / "outside" / "a.json").string(),
                                    std::string("relative.json"), std::string(3000, '/')}) {
        azl::writeFileAtomically(supervised.state() / "machine-settings.json",
                                 nlohmann::json{{"composition", path}}.dump());
        const size_t starts = count(supervised.engineLog(), "fake engine args:");
        supervised.command("exit 0");
        REQUIRE(waitForArgs(supervised, starts + 1));
        CHECK(lastArgs(supervised).find("--composition") == std::string::npos);
    }
    CHECK(count(supervised.log(), "settings: ignored") == 4);
}
