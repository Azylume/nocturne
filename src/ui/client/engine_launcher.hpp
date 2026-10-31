#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <string>

namespace azl::ui {

// Starts a local Nocturne engine for the standalone app and is the watchdog for it.
//
// On a machine where the UI runs the UI owns the engine directly: it spawns the `nocturne` binary beside it (not
// nocturne-supervisor, which is only for show machines, started by tools/autostart.sh or the setup script), with the
// engine on 127.0.0.1 and a heartbeat pipe so the engine quits when the UI quits, even on a force-quit. It restarts an
// engine that dies, but at most twice in a minute; after that it gives up and reports a failure. It reuses an engine
// that already answers on the local port and never stops one it did not start. Loopback only. Not thread-safe; use it
// from the message thread.
class EngineLauncher {
public:
    struct Hooks {
        // True when an engine already answers on the local control port.
        std::function<bool()> reachable;
        // Starts the engine; returns its pid, or 0 when it could not start one.
        std::function<int()> spawn;
        // True while the engine with this pid is still running; reaps it (and frees its resources) when it has exited.
        std::function<bool(int)> alive;
        // Stops the running engine this launcher started.
        std::function<void(int)> stop;
        // Seconds from a steady clock, for the restart window.
        std::function<double()> now;
    };

    explicit EngineLauncher(Hooks hooks);

    // Ensures a local engine. Does nothing once it owns one or has given up; reuses a reachable engine; otherwise
    // spawns one. Safe to call repeatedly: it probes before spawning, so it never starts a second engine.
    void ensureRunning();

    // Call periodically. Restarts an owned engine that has died, up to twice in 60 s; after that it gives up, stops
    // trying, and failure() explains why. Does nothing for an engine it did not start.
    void poll();

    // Stops the engine only when this launcher started it. Safe to call when it did not.
    void shutdown();

    // True while this launcher owns a running engine.
    bool startedByUs() const { return ownedPid_ != 0; }

    // Why the watchdog gave up (the engine kept dying, or could not start); empty while all is well.
    const std::string& failure() const { return failure_; }

private:
    static constexpr int kMaxRestarts = 2;
    static constexpr double kWindowSeconds = 60.0;

    Hooks hooks_;
    int ownedPid_ = 0;
    std::deque<double> restarts_;
    std::string failure_;
};

// The hooks the real app uses: probe 127.0.0.1:port; spawn `engine` (by default the `nocturne` beside the running
// executable) with --loopback --port and a heartbeat pipe (so a force-quit of the UI closes the pipe and the engine
// quits); reap or stop it, closing that pipe. `port` is the port the UI connects to, so the engine listens where the UI
// looks.
EngineLauncher::Hooks defaultEngineHooks(uint16_t port, std::filesystem::path engine = {});

}
