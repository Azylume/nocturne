#include "app/engine_main.hpp"

#include "platform/win_monitors.hpp"

#include <cstdio>
#include <stdexcept>

namespace azl::app {

// The engine's renderer does not build on Windows yet: it moves onto the GPU interface's Vulkan backend (ADR 0016).
// Until then this nocturne runs safe mode, --mic-check and --displays, and refuses anything else with the reason.
int runEngine(const EngineConfig& opt)
{
    for (const win::Monitor& m : win::activeMonitors()) {
        const DisplayInfo& d = m.info;
        std::printf("display %s (%s) %ux%u at %.0f Hz%s%s\n", d.id.c_str(), d.name.c_str(), d.width, d.height,
                    d.refreshHz, d.mirrorOf.empty() ? "" : ", mirrors ", d.mirrorOf.c_str());
    }
    if (opt.listDisplays)
        return 0;
    throw std::runtime_error("this build cannot render on Windows yet: the engine's renderer is not built here "
                             "(it moves onto the Vulkan backend). --safe-mode, --mic-check and --displays run");
}

} // namespace azl::app
