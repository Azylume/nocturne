#include "ui/app/tooltips.hpp"

namespace azl::ui {

void TipTiming::pressed(juce::uint32 now)
{
    down_ = true;
    quietSince_ = now;
}

void TipTiming::released(juce::uint32 now)
{
    down_ = false;
    quietSince_ = now;
}

juce::String TipTiming::filter(const juce::String& tip, juce::uint32 now)
{
    if (tip != last_) {
        last_ = tip;
        restSince_ = now;
    }
    if (down_ || now < std::max(restSince_, quietSince_) + kDelayMs)
        return {};
    return tip;
}

Tooltips::Tooltips(juce::Component* parent) : juce::TooltipWindow(parent, 0)
{
    juce::Desktop::getInstance().addGlobalMouseListener(&clicks_);
}

Tooltips::~Tooltips()
{
    juce::Desktop::getInstance().removeGlobalMouseListener(&clicks_);
}

juce::String Tooltips::getTipFor(juce::Component& c)
{
    return timing_.filter(juce::TooltipWindow::getTipFor(c), juce::Time::getMillisecondCounter());
}

void Tooltips::Clicks::mouseDown(const juce::MouseEvent&)
{
    timing.pressed(juce::Time::getMillisecondCounter());
}

void Tooltips::Clicks::mouseUp(const juce::MouseEvent&)
{
    timing.released(juce::Time::getMillisecondCounter());
}

juce::String PointerTips::getTooltip()
{
    auto* component = dynamic_cast<juce::Component*>(this);
    return component ? tipAt(component->getMouseXYRelative()) : juce::String();
}

} // namespace azl::ui
