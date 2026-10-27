#include "ui/client/engine_launcher.hpp"

#include <doctest.h>

using azl::ui::EngineLauncher;

namespace {

// Counts how often each hook ran, so a test can assert spawn happens once and stop only for an engine we started.
struct Recorder {
    int reachableCalls = 0;
    int spawnCalls = 0;
    int stopCalls = 0;
    pid_t stoppedPid = 0;
    bool engineUp = false;
    pid_t spawnPid = 4242;

    EngineLauncher::Hooks hooks()
    {
        EngineLauncher::Hooks h;
        h.reachable = [this] { ++reachableCalls; return engineUp; };
        h.spawn = [this] { ++spawnCalls; return spawnPid; };
        h.stop = [this](pid_t pid) { ++stopCalls; stoppedPid = pid; };
        return h;
    }
};

}

TEST_CASE("the launcher reuses an engine that is already up and never stops it")
{
    Recorder rec;
    rec.engineUp = true;
    EngineLauncher launcher(rec.hooks());

    launcher.ensureRunning();
    CHECK(rec.spawnCalls == 0);
    CHECK_FALSE(launcher.startedByUs());

    launcher.shutdown();
    CHECK(rec.stopCalls == 0); // It did not start this engine, so it leaves it running.
}

TEST_CASE("the launcher spawns an engine when none answers and stops the one it started")
{
    Recorder rec;
    rec.engineUp = false;
    EngineLauncher launcher(rec.hooks());

    launcher.ensureRunning();
    CHECK(rec.spawnCalls == 1);
    CHECK(launcher.startedByUs());

    launcher.shutdown();
    CHECK(rec.stopCalls == 1);
    CHECK(rec.stoppedPid == rec.spawnPid);
    CHECK_FALSE(launcher.startedByUs()); // Shutdown does not stop it twice.
    launcher.shutdown();
    CHECK(rec.stopCalls == 1);
}

TEST_CASE("the launcher spawns at most one engine across repeated ensureRunning calls")
{
    Recorder rec;
    rec.engineUp = false;
    EngineLauncher launcher(rec.hooks());

    launcher.ensureRunning();
    launcher.ensureRunning();
    launcher.ensureRunning();
    CHECK(rec.spawnCalls == 1);
    CHECK(rec.reachableCalls == 1);
}

TEST_CASE("a failed spawn leaves the launcher owning nothing to stop")
{
    Recorder rec;
    rec.engineUp = false;
    rec.spawnPid = 0; // posix_spawn failed, or there is no bundled supervisor.
    EngineLauncher launcher(rec.hooks());

    launcher.ensureRunning();
    CHECK(rec.spawnCalls == 1);
    CHECK_FALSE(launcher.startedByUs());

    launcher.shutdown();
    CHECK(rec.stopCalls == 0);
}
