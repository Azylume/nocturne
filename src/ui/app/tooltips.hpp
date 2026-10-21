#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace azl::ui {

// When a tooltip may show: once the pointer has rested on the same tip for a second, never while a button is down,
// and not within a second after a press or a release.
class TipTiming {
public:
    static constexpr juce::uint32 kDelayMs = 1000;

    void pressed(juce::uint32 now);
    void released(juce::uint32 now);
    // The tip to show for `tip` under the pointer at `now`: the tip, or nothing yet.
    juce::String filter(const juce::String& tip, juce::uint32 now);

private:
    juce::String last_;
    juce::uint32 restSince_ = 0;
    juce::uint32 quietSince_ = 0;
    bool down_ = false;
};

// The one tooltip window of the UI, timed by TipTiming.
class Tooltips : public juce::TooltipWindow {
public:
    explicit Tooltips(juce::Component* parent);
    ~Tooltips() override;

    juce::String getTipFor(juce::Component&) override;

private:
    struct Clicks : juce::MouseListener {
        TipTiming& timing;
        explicit Clicks(TipTiming& t) : timing(t) {}
        void mouseDown(const juce::MouseEvent&) override;
        void mouseUp(const juce::MouseEvent&) override;
    };

    TipTiming timing_;
    Clicks clicks_{timing_};
};

// A component whose tooltip depends on the part under the pointer.
class PointerTips : public juce::TooltipClient {
public:
    virtual juce::String tipAt(juce::Point<int> local) = 0;
    juce::String getTooltip() override;
};

} // namespace azl::ui
