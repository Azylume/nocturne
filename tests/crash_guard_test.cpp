#include "crash_marks.hpp"

#include "platform/process.hpp"
#include "supervisor/crash_guard.hpp"

#include <doctest.h>

#include <csignal>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

// SIGBUS where there is one: Windows has none.
#ifdef SIGBUS
constexpr int kBusError = SIGBUS;
#else
constexpr int kBusError = SIGILL;
#endif

// Runs the fake engine on `state` with one command and waits for it to end.
azl::Process runFakeEngine(const std::string& command, const fs::path& state)
{
    std::optional<azl::Process> engine =
        azl::spawnProcess({.program = NOCTURNE_FAKE_ENGINE,
                           .args = {"--do", command, "--state", state.string()},
                           .output = azl::Stdio::null(),
                           .error = azl::Stdio::null()});
    REQUIRE(engine);
    engine->wait();
    return std::move(*engine);
}

bool faultSignal(int signal)
{
    return signal == SIGSEGV || signal == kBusError;
}

} // namespace

TEST_CASE("crash marks name what is being opened, and a skip list entry is never opened again")
{
    TempFolder dir;
    const fs::path marks = dir.path / "inflight.json";
    const fs::path skips = dir.path / "skip.json";
    azl::crash::setup(marks, skips);
    {
        azl::crash::Scope opening("clip", "abc", "poison.mp4");
        const auto now = azl::crash::readMarks(marks);
        REQUIRE(now.size() == 1);
        CHECK(now[0].name == "poison.mp4");
    }
    CHECK(azl::crash::readMarks(marks).empty());

    CHECK(azl::crash::addSkips(skips, {{"clip", "abc", "poison.mp4"}}, "killed by SIGSEGV").size() == 1);
    CHECK(azl::crash::addSkips(skips, {{"clip", "abc", "poison.mp4"}}, "again").empty());
    REQUIRE(azl::crash::readSkips(skips).size() == 1);
    CHECK(azl::crash::readSkips(skips)[0].reason == "killed by SIGSEGV");
    CHECK_FALSE(azl::crash::skipped("clip", "abc"));
    azl::crash::setup(marks, skips);
    CHECK(azl::crash::skipped("clip", "abc"));
    CHECK_FALSE(azl::crash::skipped("effect", "abc"));
    azl::crash::setup({}, {});
}

TEST_CASE("a crash on another thread than a clip being opened skip-lists nothing")
{
    CrashFiles files;
    HeldMark opening("clip", "abc", "red_machine_01.mov");
    std::thread([] { azl::crash::recordCrash(SIGSEGV); }).join();
    const std::vector<azl::crash::Mark> inFlight = azl::crash::readMarks(files.marks);
    REQUIRE(inFlight.size() == 1);
    const std::optional<azl::crash::Crash> crash = azl::crash::readCrash(files.crash);
    REQUIRE(crash);
    CHECK(crash->signal == SIGSEGV);
    CHECK(crash->thread != inFlight[0].thread);
    CHECK(files.culprits().empty());
}

TEST_CASE("a crash inside a chain compile skip-lists that chain and nothing in flight elsewhere")
{
    CrashFiles files;
    HeldMark opening("clip", "def", "smoke_01.mov");
    std::vector<azl::crash::Mark> culprits;
    std::thread([&] {
        azl::crash::Scope compiling("effect", kChain, kChain);
        azl::crash::recordCrash(kBusError);
        culprits = files.culprits();
    }).join();
    REQUIRE(culprits.size() == 1);
    CHECK(culprits[0].kind == "effect");
    CHECK(culprits[0].key == kChain);
    CHECK(azl::crash::addSkips(files.skips, culprits, "killed by SIGBUS").size() == 1);
    azl::crash::setup(files.marks, files.skips, files.crash);
    CHECK(azl::crash::skipped("effect", kChain));
    CHECK_FALSE(azl::crash::skipped("clip", "def"));
}

TEST_CASE("a real fault while a clip opens is recorded on the opening thread, and the process still dies of it")
{
    TempFolder state;
    const azl::Process engine = runFakeEngine("fault", state.path);
#ifdef _WIN32
    CHECK(engine.describeExit() == "crashed with exception 0xC0000005");
#else
    CHECK((engine.describeExit() == "killed by SIGSEGV" || engine.describeExit() == "killed by SIGBUS"));
#endif
    const std::optional<azl::crash::Crash> crash = azl::crash::readCrash(state.path / "crash.json");
    REQUIRE(crash);
    CHECK(faultSignal(crash->signal));
    const std::vector<azl::crash::Mark> culprits =
        azl::crash::culprits(azl::crash::readMarks(state.path / "inflight.json"), crash);
    REQUIRE(culprits.size() == 1);
    CHECK(culprits[0].name == "poison.mp4");
}

TEST_CASE("a real fault on another thread than a clip being opened is recorded there, and skip-lists nothing")
{
    TempFolder state;
    const azl::Process engine = runFakeEngine("fault-elsewhere", state.path);
    CHECK_FALSE(engine.describeExit().empty());
    const std::optional<azl::crash::Crash> crash = azl::crash::readCrash(state.path / "crash.json");
    REQUIRE(crash);
    CHECK(faultSignal(crash->signal));
    const std::vector<azl::crash::Mark> marks = azl::crash::readMarks(state.path / "inflight.json");
    CHECK(marks.size() == 1);
    CHECK(azl::crash::culprits(marks, crash).empty());
}
