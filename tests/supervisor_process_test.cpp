// nocturne-supervisor around the fake engine (tests/fake_engine.cpp), on every platform: restarts, hangs, crash
// counting, safe mode, the skip list and stopping, with no GPU or display. supervisor_test.cpp runs the real engine.
#include "temp_folder.hpp"

#include "document/composition_file.hpp"
#include "platform/process.hpp"
#include "supervisor/crash_guard.hpp"

#include <doctest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#endif

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

// Ends a process from outside, as a user or the system would.
void killProcess(int pid)
{
#ifdef _WIN32
    const HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, DWORD(pid));
    REQUIRE(process);
    TerminateProcess(process, 1);
    CloseHandle(process);
#else
    REQUIRE(::kill(pid, SIGKILL) == 0);
#endif
}

// How the supervisor logs a process that killProcess ended.
#ifdef _WIN32
constexpr const char* kKilled = "exited with status 1";
#else
constexpr const char* kKilled = "killed by SIGKILL";
#endif

// nocturne-supervisor running the fake engine, its state and logs in a folder of their own, no desk unless `extra`
// names one.
class Supervised {
public:
    explicit Supervised(const std::vector<std::string>& extra = {})
    {
        std::vector<std::string> args = {"--engine", NOCTURNE_FAKE_ENGINE, "--state", state().string(), "--logs",
                                         (root.path / "logs").string(), "--desk", "off"};
        args.insert(args.end(), extra.begin(), extra.end());
        std::string error;
        supervisor_ = azl::spawnProcess({.program = NOCTURNE_SUPERVISOR,
                                         .args = args,
                                         .input = azl::Stdio::null(),
                                         .output = azl::Stdio::null(),
                                         .error = azl::Stdio::null()},
                                        &error);
        REQUIRE_MESSAGE(supervisor_, error);
        REQUIRE_MESSAGE(waitForRunning(1), log());
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
