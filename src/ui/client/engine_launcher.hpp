#pragma once

#include <cstdint>
#include <functional>

#include <sys/types.h>

namespace azl::ui {

// Starts a local Nocturne engine for the standalone app and stops only the one it started.
//
// Loopback only: it reuses an engine that already answers on the local control port, and it never kills an engine it
// did not start. In the single-bundle release the engine binaries sit next to the UI binary in Contents/MacOS, so the
// UI finds the supervisor by the running executable's bundle path, not the build directory. Not thread-safe; use it
// from the message thread only.
class EngineLauncher {
public:
    struct Hooks {
        // True when an engine already answers on the local control port.
        std::function<bool()> reachable;
        // Starts the bundled supervisor; returns its pid, or 0 when it could not start one.
        std::function<pid_t()> spawn;
        // Stops the supervisor this launcher started.
        std::function<void(pid_t)> stop;
    };

    explicit EngineLauncher(Hooks hooks);

    // Reuses a reachable engine; otherwise spawns one, once. Calling it again never spawns a second engine.
    void ensureRunning();

    // Stops the engine only when this launcher started it. Safe to call when it did not.
    void shutdown();

    // True once this launcher has started (and so owns) an engine.
    bool startedByUs() const { return ownedPid_ != 0; }

private:
    Hooks hooks_;
    bool ensured_ = false;
    pid_t ownedPid_ = 0;
};

// The hooks the real app uses: probe 127.0.0.1:port, spawn the nocturne-supervisor that sits next to the running
// executable (its default State and Logs directories), and stop it with SIGTERM (then SIGKILL if it lingers).
EngineLauncher::Hooks defaultEngineHooks(uint16_t port);

}
