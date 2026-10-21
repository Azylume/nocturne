#pragma once

#include "output/display_output.hpp"
#include "output/known_displays.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace azl {

// Gives every Screen bound to a display its output, and follows displays as they come and go: a screen whose display
// is detached or mirrors another keeps its output parked and attaches again, by the display's id, when it returns. Exactly one attached
// output is the main output, whose refreshes set the render clock's phase: the configured display while it is
// attached, otherwise the first attached screen's.
//
// Only the thread that owns the displays calls it (the main thread in the app).
class Outputs {
public:
    // Every display seen attached is added to `known`, when given.
    Outputs(Engine& engine, Displays& displays, std::string mainDisplay = {}, KnownDisplays* known = nullptr);
    ~Outputs();

    void setComposition(std::shared_ptr<const Composition> composition);
    // Called after the outputs followed a change of the displays.
    void onDisplaysChanged(std::function<void()> callback) { displaysChanged_ = std::move(callback); }
    void setMainDisplay(std::string display);
    // Holds every present of that display's output back by `ms`, now and whenever it opens again.
    void setDelay(const std::string& display, int ms);
    // The render clock locks to it.
    const VsyncPhase& vsync() const { return vsync_; }

    // Machine-local overlays for aligning: the composition's guides on every output, and a corner target per display
    // (in its 0..1 space). Not part of the document, so never undone or saved.
    void setGuides(bool shown);
    void setTarget(const std::string& display, std::optional<Point> point);
    // Every output (an empty display) or one shows its screen's name and output large for `seconds`; 0 ends it.
    void identify(const std::string& display, double seconds);
    bool guides() const { return guides_; }
    std::optional<Point> target(const std::string& display) const;
    // What an output of that screen shows, overlays included.
    ScreenView view(const Composition& composition, const Screen& screen) const;

    // In the order of their screens.
    std::vector<DisplayOutput*> all() const;
    // Null when no screen is bound to the display.
    DisplayOutput* find(const std::string& display) const;

private:
    void update();
    void chooseMain(const std::string& keep = {});

    Engine& engine_;
    Displays& displays_;
    std::string mainDisplay_;
    KnownDisplays* known_;
    std::function<void()> displaysChanged_;
    std::map<std::string, int> delays_;
    bool guides_ = false;
    std::map<std::string, Point> targets_;
    std::map<std::string, std::chrono::steady_clock::time_point> identify_;
    VsyncPhase vsync_;
    std::shared_ptr<const Composition> composition_ = std::make_shared<const Composition>();
    std::vector<std::unique_ptr<DisplayOutput>> outputs_;
};

} // namespace azl
