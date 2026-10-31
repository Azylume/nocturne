#pragma once

#include "gpu/gpu.hpp"
#include "output/display_info.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace azl {

// Every surface takes this format.
inline constexpr gpu::Format kSurfaceFormat = gpu::Format::BGRA8;

// What an output draws on: a display's window, or a simulated display in the harness.
class Surface {
public:
    // Called with the time the picture reached the display, or 0 when it never did.
    using Shown = std::function<void(double presentedTime)>;

    virtual ~Surface() = default;
    virtual double refreshHz() const = 0;
    // When the display refreshed, on the steady clock: the vsync itself, not when its callback got to run.
    using Refresh = std::function<void(std::chrono::steady_clock::time_point vsync)>;

    // Runs on the calling thread until `running` turns false, calling `onRefresh` once per refresh of the display.
    virtual void runRefreshes(const std::atomic<bool>& running, const Refresh& onRefresh) = 0;
    // Present thread: the texture to draw this refresh's picture into, valid until present; null when none is free.
    virtual const gpu::Texture* nextTarget() = 0;
    // Present thread: schedules the target from nextTarget for display once `cmd` completes. The caller commits.
    virtual void present(gpu::CommandBuffer& cmd, Shown shown) = 0;
};

// The machine's displays as outputs see them. Only the thread that owns the displays (the main thread in the app)
// calls it, and the change callback runs on that thread too.
class Displays {
public:
    virtual ~Displays() = default;
    virtual std::vector<DisplayInfo> attached() const = 0;
    // Null when no display with that id is attached.
    virtual std::unique_ptr<Surface> open(const std::string& id, const gpu::Device& device) = 0;
    // Ends mirroring among these displays, so each shows its own picture again. False when the system refused.
    virtual bool unmirror(const std::vector<std::string>& ids) = 0;
    // Called after displays were attached, detached or reconfigured (mirrored, moved).
    void onChange(std::function<void()> callback) { changed_ = std::move(callback); }

    // A key pressed in an output's window: Escape asks to release that display (its screen goes to no display, the
    // show runs on), the platform's quit key (Cmd+Q) asks to end the engine. Called on the thread that owns the
    // displays with the display's id; a platform without windows never calls it.
    enum class OutputKey { release, quit };
    using OutputKeyHandler = std::function<void(OutputKey key, const std::string& display)>;
    void onOutputKey(OutputKeyHandler handler) { outputKey_ = std::move(handler); }

protected:
    void changed() const
    {
        if (changed_)
            changed_();
    }
    void outputKey(OutputKey key, const std::string& display) const
    {
        if (outputKey_)
            outputKey_(key, display);
    }

private:
    std::function<void()> changed_;
    OutputKeyHandler outputKey_;
};

inline std::vector<std::string> displayIds(const Displays& displays)
{
    std::vector<std::string> ids;
    for (const DisplayInfo& d : displays.attached())
        ids.push_back(d.id);
    return ids;
}

} // namespace azl
