#pragma once

#include "ui/app/session.hpp"

#include <functional>

namespace azl::ui {

// A thin strip under the top bar with the alerts of every venue but the one in view, each with its dot; a click on one
// switches the UI to its venue. With nothing wrong it is hidden and takes no space.
class AlertStrip : public juce::Component, public juce::SettableTooltipClient, private Session::View {
public:
    static constexpr int kHeight = 22;

    explicit AlertStrip(Session& session);
    ~AlertStrip() override;

    // Called when the strip appears or goes away, so the window lays out again.
    std::function<void()> onShownChanged;
    const std::vector<StripAlert>& alerts() const { return alerts_; }
    // Each alert as drawn: "club-b: Screen 2 missing".
    juce::StringArray lines() const;
    // Switches the UI to the venue of alert `index`.
    void pick(int index);

    void paint(juce::Graphics&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;

private:
    void sessionChanged(int changes) override;
    std::vector<juce::Rectangle<int>> boxes() const;

    Session& session_;
    std::vector<StripAlert> alerts_;
};

} // namespace azl::ui
