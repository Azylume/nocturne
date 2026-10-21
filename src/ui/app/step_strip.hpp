#pragma once

#include "ui/app/session.hpp"
#include "ui/app/widgets.hpp"

#include <optional>

namespace azl::ui {

// An effect card's Autopilot badge in its header: "AP" while off, else the mode and the bars ("N4", "R8"). A click
// cycles Off, Next and Random; right-click sets Every, Hold and the transition into a step. With kPaletteEffect it is
// the palette Autopilot's.
class PilotBadge : public juce::Component, public juce::SettableTooltipClient {
public:
    PilotBadge(Session& session, uint64_t effect);

    juce::String text() const;
    void cycle();
    Menu menu();

    void paint(juce::Graphics&) override;
    void mouseUp(const juce::MouseEvent&) override;

    static constexpr int kWidth = 30;

private:
    Session& session_;
    const uint64_t effect_;
};

// An effect's steps, under its card's header: a thin row that opens with a click (closed at first) onto a chip per
// step, lit while the effect is on it and outlined when its Autopilot moves there next. A click on a chip moves the
// effect to that step; right-click updates, renames, moves or removes it; + adds a step of the current values or of a
// preset.
class StepStrip : public juce::Component, public PointerTips {
public:
    StepStrip(Session& session, uint64_t effect);

    int preferredHeight(int width) const;
    // The step whose chip is at `point`, kAdd for the + chip; none elsewhere.
    std::optional<size_t> chipAt(juce::Point<int> point) const;
    Menu chipMenu(size_t step);
    Menu addMenu();
    // A step of the values the effect shows now: its steps' and the document's.
    void addCurrent();
    // Repaints when the step shown or the next one changed.
    void refreshClock();

    void paint(juce::Graphics&) override;
    void mouseUp(const juce::MouseEvent&) override;
    juce::String tipAt(juce::Point<int> local) override;

    static constexpr size_t kAdd = SIZE_MAX;
    static constexpr int kRow = 16;
    static constexpr int kChip = 20;

private:
    const Effect* effect() const;
    std::map<std::string, double> currentValues() const;
    void setSteps(std::vector<EffectStep> steps);
    juce::Rectangle<int> chipBounds(size_t index, int width) const;
    std::pair<std::optional<size_t>, std::optional<size_t>> shown() const;

    Session& session_;
    const uint64_t effect_;
    std::pair<std::optional<size_t>, std::optional<size_t>> painted_;
};

} // namespace azl::ui
