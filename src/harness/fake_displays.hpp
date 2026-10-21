#pragma once

#include "harness/image.hpp"
#include "output/displays.hpp"

#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace azl {

class FakeSurface;

// Simulated displays for the harness: the test attaches and detaches them, each refreshes on a timer at its rate and
// presents into an offscreen texture the test can read back. The test also reconfigures them as macOS does: mirrors
// one display onto another, or has one come back under a new id, each as one change.
class FakeDisplays : public Displays {
public:
    void attach(DisplayInfo display);
    // `id` shows what `of` shows; an empty `of` ends it.
    void mirror(const std::string& id, const std::string& of);
    // The display with `id` is detached and `display` attached in the same reconfiguration.
    void replace(const std::string& id, DisplayInfo display);
    // Each refresh's callback then runs `min` to `max` after its vsync, as a busy machine delays a display link.
    void setLateness(std::chrono::microseconds min, std::chrono::microseconds max) { lateness_ = {min, max}; }
    void detach(const std::string& id);

    std::vector<DisplayInfo> attached() const override { return attached_; }
    std::unique_ptr<Surface> open(const std::string& id, MTL::Device* device) override;
    bool unmirror(const std::vector<std::string>& ids) override;
    // Every unmirror call so far, with its ids.
    const std::vector<std::vector<std::string>>& unmirrored() const { return unmirrored_; }

    // What the display shows at its next present, as RGBA8; empty when it presents nothing within two seconds.
    Image capture(const std::string& id);

private:
    friend class FakeSurface;

    std::vector<DisplayInfo> attached_;
    std::vector<std::vector<std::string>> unmirrored_;
    std::pair<std::chrono::microseconds, std::chrono::microseconds> lateness_{};
    std::map<std::string, FakeSurface*> open_;
};

} // namespace azl
