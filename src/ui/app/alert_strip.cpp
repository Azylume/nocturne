#include "ui/app/alert_strip.hpp"

#include "ui/app/look.hpp"
#include "ui/app/venue_menu.hpp"

namespace azl::ui {

namespace {

constexpr int kDotBox = 16;
constexpr int kGap = 14;

juce::String line(const StripAlert& alert)
{
    return utf8(alert.venue.name) + ": " + utf8(alert.alert.text);
}

} // namespace

AlertStrip::AlertStrip(Session& session) : session_(session)
{
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    session_.addView(this);
    sessionChanged(Session::Venues);
}

AlertStrip::~AlertStrip()
{
    session_.removeView(this);
}

void AlertStrip::sessionChanged(int changes)
{
    if (!(changes & (Session::Venues | Session::Connection)))
        return;
    const VenueMonitor* venues = session_.venues();
    alerts_ = venues ? otherVenueAlerts(venues->entries(), session_.host(), session_.port())
                     : std::vector<StripAlert>{};
    const bool shown = !alerts_.empty();
    if (shown != isVisible()) {
        setVisible(shown);
        if (onShownChanged)
            onShownChanged();
    }
    repaint();
}

juce::StringArray AlertStrip::lines() const
{
    juce::StringArray lines;
    for (const StripAlert& alert : alerts_)
        lines.add(line(alert));
    return lines;
}

std::vector<juce::Rectangle<int>> AlertStrip::boxes() const
{
    std::vector<juce::Rectangle<int>> boxes;
    int x = 6;
    for (const StripAlert& alert : alerts_) {
        const int width = kDotBox + juce::GlyphArrangement::getStringWidthInt(Look::font(), line(alert)) + 6;
        boxes.push_back({x, 0, width, getHeight()});
        x += width + kGap;
    }
    return boxes;
}

void AlertStrip::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    g.fillAll(p.panel);
    g.setFont(Look::font());
    const std::vector<juce::Rectangle<int>> areas = boxes();
    for (size_t i = 0; i < alerts_.size(); ++i) {
        auto area = areas[i];
        if (area.getX() >= getWidth())
            break;
        g.setColour(dotColour(alerts_[i].alert.dot));
        g.fillEllipse(juce::Rectangle<float>(8.0f, 8.0f).withCentre(
            {float(area.getX() + kDotBox / 2 - 2), float(area.getCentreY())}));
        area.removeFromLeft(kDotBox);
        g.setColour(p.white);
        drawText(g, line(alerts_[i]), area.getIntersection(getLocalBounds()), juce::Justification::centredLeft, true);
    }
}

void AlertStrip::pick(int index)
{
    if (index < 0 || index >= int(alerts_.size()))
        return;
    const Venue venue = alerts_[size_t(index)].venue;
    session_.connectTo(venue.host, venue.port);
}

void AlertStrip::mouseUp(const juce::MouseEvent& e)
{
    const std::vector<juce::Rectangle<int>> areas = boxes();
    for (size_t i = 0; i < areas.size(); ++i)
        if (areas[i].contains(e.getPosition()))
            return pick(int(i));
}

void AlertStrip::mouseMove(const juce::MouseEvent& e)
{
    const std::vector<juce::Rectangle<int>> areas = boxes();
    for (size_t i = 0; i < areas.size(); ++i)
        if (areas[i].contains(e.getPosition()))
            return setTooltip("Show " + utf8(alerts_[i].venue.name) + " in every tab");
    setTooltip({});
}

} // namespace azl::ui
