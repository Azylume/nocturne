#pragma once

#include "app/engine_config.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace azl {
class Displays;
} // namespace azl

namespace azl::app {

// Runs the engine until the main thread's loop is stopped (stopMain): every service, the render thread, the outputs
// and the stats line. On the main thread, which the platform's main has set up; returns the exit code.
int runEngine(const EngineConfig& opt);

// What each platform's main (main.mm, main_win.cpp) provides to runEngine.

// Something running until it is released.
using Running = std::shared_ptr<void>;

// The output windows on this machine's displays.
std::unique_ptr<Displays> windowDisplays(bool windowed, int virtualDisplays);
// Runs the main thread's loop until stopMain.
void runMain();
// Quits the engine (stopMain) when the system or a supervisor asks this process to stop.
Running quitOnStopRequests();
// The index of the frame the engine published last, UINT64_MAX before the first. Any thread.
using LatestFrame = std::function<uint64_t()>;
// Beats to the supervisor whenever a new frame was published, and quits once the supervisor is gone. Null when no
// supervisor started this process.
Running beatToSupervisor(LatestFrame latest);

// The process's use of the machine, for the stats line.
struct ProcessUse {
    double cpuSeconds = 0.0;
    size_t threads = 0;
    // Mach ports on Apple, kernel handles on Windows.
    size_t handles = 0;
    // What the heap holds, beside the resident size.
    double heapMB = 0.0;
};
ProcessUse processUse();
// The name the stats give `ProcessUse::handles`.
extern const char* const kHandlesStat;

} // namespace azl::app
