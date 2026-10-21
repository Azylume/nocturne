#pragma once

#include "engine/engine.hpp"
#include "engine/vsync_phase.hpp"

#include <atomic>
#include <thread>

namespace azl {

// Engine time: seconds on the steady clock since the process started. Render threads render at it, and live actions
// are stamped with it.
double engineTime();

// Drives an engine from its own fixed-rate clock, optionally phase-locked to one display's refreshes.
class RenderThread {
public:
    RenderThread(Engine& engine, double fps, const VsyncPhase* vsync = nullptr);
    ~RenderThread();

private:
    void run(double fps, const VsyncPhase* vsync);

    Engine& engine_;
    std::atomic<bool> running_{true};
    std::thread thread_;
};

} // namespace azl
