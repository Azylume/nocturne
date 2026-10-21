#pragma once

#include "ui/app/widgets.hpp"

#include <vector>

namespace azl::ui {

// The composition's Autopilot pace, the column Autopilot, then Autopilot per layer: Off, Next or Random, and every 1 to
// 64 bars. While the column Autopilot runs the layers' Autopilots show as paused.
class AutopilotView : public juce::Component, public PointerTips, private Session::View {
public:
    explicit AutopilotView(Session& session);
    ~AutopilotView() override;
    void paint(juce::Graphics&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    juce::String tipAt(juce::Point<int> point) override;

    // Buttons 0..2 are the modes, 3..9 the bars.
    juce::Rectangle<int> buttonArea(uint64_t layer, int button) const;
    // Paces 0..4 are x0.5 to x8.
    juce::Rectangle<int> paceArea(int pace) const;
    // The column Autopilot's buttons, numbered as a layer's.
    juce::Rectangle<int> columnArea(int button) const;
    // What the column Autopilot's row says after its modes: "every", or "manual N" while a trigger holds it.
    juce::String columnStatus() const;

private:
    void sessionChanged(int changes) override;
    std::vector<uint64_t> layers() const;
    void paintModes(juce::Graphics& g, std::function<juce::Rectangle<int>(int)> area, const Autopilot& autopilot,
                    bool paused, const juce::String& status) const;

    Session& session_;
    int scroll_ = 0;
};

// The undo history as this UI saw it: steps to redo greyed above "Now", steps to undo below, the latest first.
class HistoryView : public juce::Component, public juce::SettableTooltipClient, private Session::View {
public:
    explicit HistoryView(Session& session);
    ~HistoryView() override;
    void paint(juce::Graphics&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    // The `index`th line from the top, scrolled.
    juce::Rectangle<int> rowArea(size_t index) const;
    size_t rowCount() const;

private:
    void sessionChanged(int changes) override;

    Session& session_;
    int scroll_ = 0;
};

} // namespace azl::ui
