#include "ui/client/engine_launcher.hpp"

#include "net/socket_compat.hpp"

#include <doctest.h>

#include <chrono>
#include <thread>

using azl::ui::EngineLauncher;

namespace {

// A stand-in engine the launcher drives through its hooks: it can be "up" (reachable), it tracks the pid it spawned,
// and a test can make that pid die to drive the watchdog. A fake clock advances the restart window by hand.
struct FakeEngine {
    bool up = false;          // Something already answers on the port (a reuse case).
    int next = 1000;        // The pid the next spawn hands out.
    int running = 0;        // The pid currently alive, 0 when none.
    int spawns = 0;
    int stops = 0;
    int lastStopped = 0;
    bool spawnFails = false;
    double clock = 0.0;

    EngineLauncher::Hooks hooks()
    {
        EngineLauncher::Hooks h;
        h.reachable = [this] { return up; };
        h.now = [this] { return clock; };
        h.spawn = [this]() -> int {
            ++spawns;
            if (spawnFails)
                return 0;
            running = next++;
            return running;
        };
        h.alive = [this](int pid) { return pid == running; };
        h.stop = [this](int pid) {
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

    const int started = eng.running;
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

TEST_CASE("the default hooks see an engine that answers on the local port, and none once it stops listening")
{
    azl::startSockets();
    const azl::Socket listener = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listener != azl::kNoSocket);
    in_addr loopback{};
    loopback.s_addr = htonl(INADDR_LOOPBACK);
    sockaddr_in address = azl::ipv4Endpoint(loopback, 0);
    REQUIRE(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
    REQUIRE(::listen(listener, 1) == 0);
    socklen_t size = sizeof address;
    ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size);
    const EngineLauncher::Hooks hooks = azl::ui::defaultEngineHooks(ntohs(address.sin_port), NOCTURNE_FAKE_ENGINE);
    CHECK(hooks.reachable());
    azl::closeSocket(listener);
    CHECK_FALSE(hooks.reachable());
}

TEST_CASE("the default hooks start the engine with a heartbeat and ask it to quit, which it does at once")
{
    const EngineLauncher::Hooks hooks = azl::ui::defaultEngineHooks(7499, NOCTURNE_FAKE_ENGINE);
    const int pid = hooks.spawn();
    REQUIRE(pid > 0);
    CHECK(hooks.alive(pid));
    // Time for it to start watching for stop requests.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto asked = std::chrono::steady_clock::now();
    hooks.stop(pid);
    // Killed only after 3 s without quitting.
    CHECK(std::chrono::steady_clock::now() - asked < std::chrono::seconds(2));

    CHECK(azl::ui::defaultEngineHooks(7499, "no-such-engine").spawn() == 0);
}
