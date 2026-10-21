#include "ui/app/venue_menu.hpp"

#include "ui/app/look.hpp"

#include <algorithm>

namespace azl::ui {

namespace {

constexpr int kPanelWidth = 440;
constexpr int kRowHeight = 40;
constexpr int kMaxRows = 8;

void drawDot(juce::Graphics& g, juce::Point<float> centre, VenueDot dot)
{
    g.setColour(dotColour(dot));
    g.fillEllipse(juce::Rectangle<float>(9.0f, 9.0f).withCentre(centre));
}

juce::String alertWords(const std::vector<VenueAlert>& alerts)
{
    juce::StringArray words;
    for (const VenueAlert& alert : alerts)
        words.add(utf8(alert.text));
    return words.isEmpty() ? juce::String("ok") : words.joinIntoString(juce::String::fromUTF8(" \xc2\xb7 "));
}

std::string offlineReason(const VenueEntry& entry)
{
    if (!entry.venue.online)
        return "offline in Tailscale";
    return entry.probe.engineError.empty() ? entry.probe.error : entry.probe.engineError;
}

} // namespace

juce::Colour dotColour(VenueDot dot)
{
    const Palette& p = Look::palette();
    switch (dot) {
    case VenueDot::Ok:
        return p.live;
    case VenueDot::Warning:
        return p.accent;
    case VenueDot::Fault:
        return p.fault;
    case VenueDot::Offline:
        return juce::Colour(0xff555555);
    }
    return p.muted;
}

VenueButton::VenueButton(Session& session) : session_(session)
{
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    session_.addView(this);
    sessionChanged(Session::Venues);
}

VenueButton::~VenueButton()
{
    session_.removeView(this);
}

juce::String VenueButton::label() const
{
    if (const auto venue = session_.currentVenue())
        return utf8(venue->venue.name);
    return utf8(session_.host());
}

VenueDot VenueButton::dot() const
{
    if (!session_.client().connected())
        return VenueDot::Offline;
    const auto venue = session_.currentVenue();
    return venue ? venue->dot : VenueDot::Ok;
}

void VenueButton::sessionChanged(int changes)
{
    if (!(changes & (Session::Venues | Session::Connection)))
        return;
    const auto venue = session_.currentVenue();
    setTooltip("Venue " + label() + ": " + (venue ? alertWords(venue->alerts) : juce::String("ok")) +
               ". Click to pick another venue.");
    repaint();
}

void VenueButton::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    g.fillAll(isMouseOver() ? p.buttonHover : p.button);
    auto area = getLocalBounds().reduced(9, 0);
    drawDot(g, {float(area.getX() + 5), float(area.getCentreY())}, dot());
    area.removeFromLeft(16);
    drawChevron(g, area.removeFromRight(12), true, p.muted);
    g.setColour(p.white);
    g.setFont(Look::font(true));
    drawText(g, label(), area, juce::Justification::centredLeft, true);
}

void VenueButton::mouseUp(const juce::MouseEvent& e)
{
    if (onClick && getLocalBounds().contains(e.getPosition()))
        onClick();
}

VenueMenu::VenueMenu(Session& session) : session_(session)
{
    list_.setModel(this);
    list_.setRowHeight(kRowHeight);
    list_.setColour(juce::ListBox::backgroundColourId, Look::palette().field);
    list_.setTooltip("Click a venue to show it in every tab");
    host_.setTextToShowWhenEmpty("Or type a host name or address", Look::palette().muted);
    host_.setTooltip("Connect to this host, e.g. a machine's Tailscale name; it stays in the list");
    host_.onReturnKey = [this] { connectTyped(); };
    host_.onEscapeKey = [this] { close(); };
    token_.setTextToShowWhenEmpty("Pairing token", Look::palette().muted);
    token_.setTooltip("The venue machine's pairing token (nocturne-ctl token on that machine), kept for the host you "
                      "connect to next; machines on this computer need none");
    token_.setPasswordCharacter(juce::juce_wchar(0x2022));
    token_.onReturnKey = [this] { connectTyped(); };
    token_.onEscapeKey = [this] { close(); };
    connect_.setTooltip("Connect the UI to the typed host");
    connect_.onClick = [this] { connectTyped(); };
    for (juce::Component* c : std::initializer_list<juce::Component*>{&list_, &host_, &token_, &connect_})
        addAndMakeVisible(c);
    setWantsKeyboardFocus(true);
    session_.addView(this);
    if (VenueMonitor* venues = session_.venues())
        venues->refresh();
    sessionChanged(Session::Venues);
}

VenueMenu::~VenueMenu()
{
    session_.removeView(this);
}

void VenueMenu::sessionChanged(int changes)
{
    if (!(changes & (Session::Venues | Session::Connection)))
        return;
    VenueMonitor* venues = session_.venues();
    rows_ = venues ? venues->entries() : std::vector<VenueEntry>{};
    note_ = venues ? utf8(venues->tailscaleError()) : juce::String("No venue list in this run: type a host");
    if (venues && !note_.isEmpty())
        note_ = "Tailscale: " + note_;
    list_.updateContent();
    list_.repaint();
    resized();
    repaint();
}

void VenueMenu::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool)
{
    if (row < 0 || row >= int(rows_.size()))
        return;
    const Palette& p = Look::palette();
    const VenueEntry& entry = rows_[size_t(row)];
    const bool current = juce::String(entry.venue.host).equalsIgnoreCase(utf8(session_.host())) &&
                         entry.venue.port == session_.port();
    g.fillAll(current ? p.liveRow : p.field);
    auto area = juce::Rectangle<int>(width, height).reduced(8, 3);
    drawDot(g, {float(area.getX() + 5), float(area.getY() + height / 4)}, entry.dot);
    area.removeFromLeft(18);
    auto top = area.removeFromTop(height / 2 - 3);
    g.setFont(Look::font());
    g.setColour(p.muted);
    const juce::String where = utf8(entry.venue.host) + ":" + juce::String(entry.venue.port) + "  " +
                               utf8(entry.venue.source);
    drawText(g, where, top, juce::Justification::centredRight);
    g.setColour(p.white);
    g.setFont(Look::font(true));
    drawText(g, utf8(entry.venue.name), top.withTrimmedRight(220), juce::Justification::centredLeft, true);
    g.setFont(Look::font());
    g.setColour(entry.dot == VenueDot::Ok ? p.muted : dotColour(entry.dot));
    const bool offline = !entry.alerts.empty() && entry.alerts[0].kind == AlertKind::Offline;
    drawText(g, offline ? utf8(offlineReason(entry)) : alertWords(entry.alerts), area, juce::Justification::centredLeft,
             true);
}

juce::String VenueMenu::getTooltipForRow(int row)
{
    if (row < 0 || row >= int(rows_.size()))
        return {};
    return "Show " + utf8(rows_[size_t(row)].venue.name) + " in every tab; its show plays on as it is";
}

void VenueMenu::pick(int row)
{
    if (row < 0 || row >= int(rows_.size()))
        return;
    const Venue venue = rows_[size_t(row)].venue;
    close();
    keepToken(venue.host);
    session_.connectTo(venue.host, venue.port);
}

void VenueMenu::connectTyped()
{
    const std::string host = host_.getText().trim().toStdString();
    if (host.empty())
        return;
    close();
    keepToken(hostAndPort(host).first);
    session_.connectToTyped(host);
}

void VenueMenu::keepToken(const std::string& host)
{
    const std::string token = token_.getText().trim().toStdString();
    if (!token.empty())
        session_.rememberToken(host, token);
    token_.clear();
}

void VenueMenu::close()
{
    setVisible(false);
}

juce::Rectangle<int> VenueMenu::panel() const
{
    const int rows = std::clamp(int(rows_.size()), 1, kMaxRows);
    const int height = 12 + (note_.isEmpty() ? 0 : 24) + rows * kRowHeight + 6 + 26 + 12;
    const int width = std::min(kPanelWidth, getWidth() - 16);
    return {8, Look::metrics().transportHeight + 2, width, height};
}

void VenueMenu::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black.withAlpha(0.35f));
    const Palette& p = Look::palette();
    const auto box = panel();
    g.setColour(p.panel);
    g.fillRect(box);
    g.setColour(p.mapFrame);
    g.drawRect(box);
    if (note_.isNotEmpty()) {
        g.setColour(p.accent);
        g.setFont(Look::font());
        drawText(g, note_, box.reduced(12, 0).withTrimmedTop(12).withHeight(20), juce::Justification::centredLeft,
                 true);
    }
    if (rows_.empty()) {
        g.setColour(p.muted);
        drawText(g, "No venues found", list_.getBounds(), juce::Justification::centred);
    }
}

void VenueMenu::resized()
{
    auto area = panel().reduced(12);
    if (note_.isNotEmpty())
        area.removeFromTop(24);
    auto fields = area.removeFromBottom(26);
    connect_.setBounds(fields.removeFromRight(80));
    fields.removeFromRight(6);
    token_.setBounds(fields.removeFromRight(fields.getWidth() * 2 / 5));
    fields.removeFromRight(6);
    host_.setBounds(fields);
    area.removeFromBottom(6);
    list_.setBounds(area);
    list_.setVisible(!rows_.empty());
}

void VenueMenu::mouseDown(const juce::MouseEvent& e)
{
    if (!panel().contains(e.getPosition()))
        close();
}

bool VenueMenu::keyPressed(const juce::KeyPress& key)
{
    if (key != juce::KeyPress::escapeKey)
        return false;
    close();
    return true;
}

} // namespace azl::ui
