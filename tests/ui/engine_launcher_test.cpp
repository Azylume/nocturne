#include "ui/client/engine_launcher.hpp"

#include <doctest.h>

using azl::ui::EngineLauncher;

namespace {

// A stand-in engine the launcher drives through its hooks: it can be "up" (reachable), it tracks the pid it spawned,
// and a test can make that pid die to drive the watchdog. A fake clock advances the restart window by hand.
struct FakeEngine {
    bool up = false;          // Something already answers on the port (a reuse case).
    pid_t next = 1000;        // The pid the next spawn hands out.
    pid_t running = 0;        // The pid currently alive, 0 when none.
    int spawns = 0;
    int stops = 0;
    pid_t lastStopped = 0;
    bool spawnFails = false;
    double clock = 0.0;

    EngineLauncher::Hooks hooks()
    {
        EngineLauncher::Hooks h;
        h.reachable = [this] { return up; };
        h.now = [this] { return clock; };
        h.spawn = [this]() -> pid_t {
            ++spawns;
            if (spawnFails)
                return 0;
            running = next++;
            return running;
        };
        h.alive = [this](pid_t pid) { return pid == running; };
        h.stop = [this](pid_t pid) {
            ++stops;
            lastStopped = pid;
            if (pid == running)
                running = 0;
        };
        return h;
    }

    void die() { running = 0; } // The engine process exits on its own.
};

}

TEST_CASE("the launcher reuses an engine that is already up and never stops it")
{
    FakeEngine eng;
    eng.up = true;
    EngineLauncher launcher(eng.hooks());

    launcher.ensureRunning();
    CHECK(eng.spawns == 0);
    CHECK_FALSE(launcher.startedByUs());

    launcher.shutdown();
    CHECK(eng.stops == 0); // It did not start this engine, so it leaves it running.
}

TEST_CASE("the launcher spawns an engine when none answers and stops the one it started on quit")
{
    FakeEngine eng;
    EngineLauncher launcher(eng.hooks());

    launcher.ensureRunning();
    CHECK(eng.spawns == 1);
    CHECK(launcher.startedByUs());
    CHECK(launcher.failure().empty());

    const pid_t started = eng.running;
    launcher.shutdown();
    CHECK(eng.stops == 1);
    CHECK(eng.lastStopped == started);
    CHECK_FALSE(launcher.startedByUs());
}

TEST_CASE("the watchdog restarts an engine that dies, up to twice in a minute, then gives up")
{
    FakeEngine eng;
    EngineLauncher launcher(eng.hooks());
    launcher.ensureRunning();
    CHECK(eng.spawns == 1);

    // First death: restarted.
    eng.die();
    launcher.poll();
    CHECK(eng.spawns == 2);
    CHECK(launcher.startedByUs());
    CHECK(launcher.failure().empty());

    // Second death: restarted again (two restarts within the minute).
    eng.clock = 10.0;
    eng.die();
    launcher.poll();
    CHECK(eng.spawns == 3);
    CHECK(launcher.startedByUs());
    CHECK(launcher.failure().empty());

    // Third death within the minute: it gives up and reports a failure.
    eng.clock = 20.0;
    eng.die();
    launcher.poll();
    CHECK(eng.spawns == 3);
    CHECK_FALSE(launcher.startedByUs());
    CHECK_FALSE(launcher.failure().empty());

    // Having given up, it keeps still.
    launcher.poll();
    launcher.ensureRunning();
    CHECK(eng.spawns == 3);
}

TEST_CASE("a death after the window has passed is restarted again, not counted against an old one")
{
    FakeEngine eng;
    EngineLauncher launcher(eng.hooks());
    launcher.ensureRunning();

    eng.die();
    launcher.poll(); // restart 1 at t=0
    eng.clock = 30.0;
    eng.die();
    launcher.poll(); // restart 2 at t=30
    CHECK(eng.spawns == 3);
    CHECK(launcher.failure().empty());

    // The first restart is now more than a minute old, so this death is only the second in the current window.
    eng.clock = 70.0;
    eng.die();
    launcher.poll();
    CHECK(eng.spawns == 4);
    CHECK(launcher.failure().empty());
    CHECK(launcher.startedByUs());
}

TEST_CASE("the watchdog ignores an engine it did not start")
{
    FakeEngine eng;
    eng.up = true;
    EngineLauncher launcher(eng.hooks());
    launcher.ensureRunning(); // reuse, owns nothing

    launcher.poll();
    CHECK(eng.spawns == 0);
    CHECK(launcher.failure().empty());
}

TEST_CASE("a failed spawn gives up with a failure and nothing to stop")
{
    FakeEngine eng;
    eng.spawnFails = true;
    EngineLauncher launcher(eng.hooks());

    launcher.ensureRunning();
    CHECK(eng.spawns == 1);
    CHECK_FALSE(launcher.startedByUs());
    CHECK_FALSE(launcher.failure().empty());

    launcher.shutdown();
    CHECK(eng.stops == 0);
}
