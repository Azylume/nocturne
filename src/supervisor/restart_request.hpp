#pragma once

namespace azl::restart {

// The exit codes with which the engine asks nocturne-supervisor to start it again: in normal mode (from safe mode, this
// leaves it), or in safe mode.
constexpr int kNormal = 75;
constexpr int kSafeMode = 76;

// The exit code with which the engine asks nocturne-supervisor to stop it and exit without starting it again (a client
// asked to stop the show). SIGTERM to the supervisor does the same.
constexpr int kStop = 77;

// The exit code of an engine that found no GPU it can run on. Before it exits it prints one line on stderr that starts
// with kNoGpuLine and says what is missing. nocturne-supervisor records that line and stops without restarting it or
// entering safe mode; the UI shows it.
constexpr int kNoGpu = 78;
constexpr const char* kNoGpuLine = "nocturne: no usable GPU: ";

// Set in the engine's environment by the supervisor, so the engine knows someone hears the request.
constexpr const char* kSupervisedVariable = "NOCTURNE_SUPERVISED";

} // namespace azl::restart
