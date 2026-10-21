#pragma once

#include "app/engine_config.hpp"
#include "engine/machine_actions.hpp"

namespace azl {

// What the supervisor runs after too many crashes or when a client asks for it: black (or the fallback image) on every
// display with the reason along the bottom, and nothing else of the engine, so nothing that crashed it runs again. The
// protocol serves only the machine status ("mode": "safe", "reason"), Restart engine, which leaves safe mode, and the
// Show lock (machine_actions.hpp); the document is empty and read-only. Returns on SIGINT/SIGTERM with 0, or with the
// restart request's exit code.
int runSafeMode(const app::EngineConfig& opt);

// How an engine run by the supervisor asks it for a restart: a moment later, for the reply to go out, `*exitCode` is
// set to the request's exit code and `quit` is called on the main thread. Null without a supervisor.
MachineActions::Restart restartRequest(int* exitCode, void (*quit)());

} // namespace azl
