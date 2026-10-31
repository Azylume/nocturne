// Windows outputs: WinDisplays' windows presenting through their Vulkan swapchains, each on its own present thread
// (ADR 0003). The window cases need the desktop session (win.sh gui) and a GPU.
#include "gpu/gpu.hpp"
#include "gpu/gpu_vulkan.hpp"
#include "platform/main_loop.hpp"
#include "platform/thread_priority.hpp"
#include "platform/win_displays.hpp"

#include <doctest.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using OutputKey = azl::Displays::OutputKey;

// What one output did, counted the way DisplayOutput counts its stats.
struct Counts {
    std::atomic<uint64_t> refreshes{0};
    std::atomic<uint64_t> presents{0};
    std::atomic<uint64_t> noTarget{0};
    std::atomic<uint64_t> shown{0};
    std::atomic<uint64_t> unshown{0};
    std::atomic<uint64_t> hitches{0};
    std::atomic<double> maxIntervalMs{0.0};
    // Seconds after the first shown picture.
    std::atomic<double> lastHitchAt{-1.0};
    double firstShown = 0.0;
    double lastShown = 0.0;
    double period = 0.0;

    void shownAt(double at)
    {
        if (at <= 0.0) {
            ++unshown;
            return;
        }
        ++shown;
        if (lastShown > 0.0) {
            const double interval = at - lastShown;
            maxIntervalMs = std::max(maxIntervalMs.load(), interval * 1000.0);
            if (interval > 1.5 * period) {
                ++hitches;
                lastHitchAt = at - firstShown;
            }
        } else {
            firstShown = at;
        }
        lastShown = at;
    }
};

// A present thread drawing a new colour into every refresh of its surface.
class Presenter {
public:
    Presenter(azl::Surface& surface, const azl::gpu::Device& device)
        : surface_(surface), queue_(device.makeQueue()), thread_([this] { run(); })
    {
        counts_.period = 1.0 / surface.refreshHz();
    }
    // Before its surface goes: the surface may report shown pictures until then.
    void stop()
    {
        running_ = false;
        thread_.join();
    }
    Counts& counts() { return counts_; }

private:
    // As DisplayOutput's present thread runs.
    void run()
    {
        azl::setThreadRole(azl::ThreadRole::Frames);
        surface_.runRefreshes(running_, [this](std::chrono::steady_clock::time_point) {
            azl::gpu::Scope scope;
            ++counts_.refreshes;
            const azl::gpu::Texture* target = surface_.nextTarget();
            if (!target) {
                ++counts_.noTarget;
                return;
            }
            const double shade = double(counts_.presents % 64) / 63.0;
            azl::gpu::CommandBuffer cmd = queue_.commandBuffer();
            const azl::gpu::RenderTarget clear{
                .texture = target, .load = azl::gpu::Load::Clear, .clear = {shade, 0.2, 0.4, 1.0}};
            azl::gpu::RenderPass pass = cmd.begin(clear);
            pass.end();
            surface_.present(cmd, [this](double at) { counts_.shownAt(at); });
            cmd.commit();
            ++counts_.presents;
        });
    }

    azl::Surface& surface_;
    azl::gpu::Queue queue_;
    Counts counts_;
    std::atomic<bool> running_{true};
    std::thread thread_;
};

double outputSeconds()
{
    const char* seconds = std::getenv("NOCTURNE_OUTPUT_SECONDS");
    return seconds ? std::atof(seconds) : 5.0;
}

void pump(azl::MainLoop& loop, std::chrono::milliseconds duration)
{
    loop.after(duration, [&loop] { loop.quit(); });
    loop.run();
}

} // namespace

TEST_CASE("displays: Escape alone releases an output's display and Alt+F4 quits the engine")
{
    CHECK(azl::outputKeyFor({.virtualKey = VK_ESCAPE}) == OutputKey::release);
    CHECK(azl::outputKeyFor({.virtualKey = VK_F4, .alt = true}) == OutputKey::quit);
    CHECK_FALSE(azl::outputKeyFor({.virtualKey = VK_ESCAPE, .shift = true}));
    CHECK_FALSE(azl::outputKeyFor({.virtualKey = VK_F4}));
    CHECK_FALSE(azl::outputKeyFor({.virtualKey = VK_F4, .alt = true, .control = true}));
    CHECK_FALSE(azl::outputKeyFor({.virtualKey = 'Q', .control = true}));
}

TEST_CASE("displays: virtual displays are window-1..N at 1920x1080 and open only by those ids")
{
    azl::MainLoop loop;
    azl::WinDisplays displays(false, 2);
    const std::vector<azl::DisplayInfo> attached = displays.attached();
    REQUIRE(attached.size() == 2);
    CHECK(attached[0].id == "window-1");
    CHECK(attached[1].id == "window-2");
    CHECK(attached[1].width == 1920);
    CHECK(attached[1].height == 1080);
    const azl::gpu::Device device = azl::gpu::Device::systemDefault();
    REQUIRE(device);
    CHECK_FALSE(displays.open("window-3", device));
    CHECK_FALSE(displays.unmirror({"window-1"}));
}

TEST_CASE("displays: the panel and a window present at the panel's rate, each on its own thread, without hitches")
{
    azl::MainLoop loop;
    azl::WinDisplays monitors(false, 0);
    azl::WinDisplays windows(false, 1);
    const std::vector<azl::DisplayInfo> attached = monitors.attached();
    if (attached.empty()) {
        MESSAGE("no monitor in this session: run in the desktop session");
        return;
    }
    const azl::gpu::Device device = azl::gpu::Device::systemDefault();
    REQUIRE(device);
    std::unique_ptr<azl::Surface> panel = monitors.open(attached.front().id, device);
    std::unique_ptr<azl::Surface> window = windows.open("window-1", device);
    REQUIRE(panel);
    REQUIRE(window);
    CHECK_FALSE(monitors.open("no-such-display", device));
    const double seconds = outputSeconds();
    Presenter onPanel(*panel, device);
    Presenter onWindow(*window, device);
    pump(loop, std::chrono::milliseconds(int64_t(seconds * 1000.0)));
    onPanel.stop();
    onWindow.stop();
    window.reset();
    panel.reset();
    for (auto [name, counts] : {std::pair{std::string("panel"), &onPanel.counts()},
                                std::pair{std::string("window-1"), &onWindow.counts()}}) {
        const Counts& c = *counts;
        MESSAGE(name << ": " << c.refreshes.load() / seconds << " refreshes/s, " << c.presents.load() / seconds
                     << " presents/s, " << c.shown.load() / seconds << " shown/s, " << c.noTarget.load()
                     << " without a target, " << c.unshown.load() << " never shown, max interval "
                     << c.maxIntervalMs.load() << " ms, " << c.hitches.load() << " hitches (the last "
                     << c.lastHitchAt.load() << " s in) at "
                     << attached.front().refreshHz << " Hz over " << seconds << " s");
        CHECK(double(c.presents) >= 0.97 * attached.front().refreshHz * seconds);
        CHECK(c.hitches == 0);
        CHECK(c.noTarget == 0);
    }
    if (azl::gpu::vulkan::validationEnabled())
        CHECK(azl::gpu::vulkan::validationErrors() == 0);
}
