#include "ui/app/mid_tabs.hpp"

#include "ui/app/look.hpp"

namespace azl::ui {

namespace {

constexpr int kRow = 20;
constexpr int kNameWidth = 56;
constexpr int kButton = 24;
const int kBars[] = {1, 2, 4, 8, 16, 32, 64};
const double kPaces[] = {0.5, 1.0, 2.0, 4.0, 8.0};
const char* const kPaceNames[] = {"x0.5", "x1", "x2", "x4", "x8"};
const char* const kModes[] = {"Off", "Next", "Random"};

juce::String barsText(int bars)
{
    return juce::String(bars) + (bars == 1 ? " bar" : " bars");
}

} // namespace

AutopilotView::AutopilotView(Session& session) : session_(session)
{
    session_.addView(this);
}

AutopilotView::~AutopilotView()
{
    session_.removeView(this);
}

void AutopilotView::sessionChanged(int changes)
{
    const bool running = session_.composition().columnAutopilot.mode != AutopilotMode::Off;
    if ((changes & Session::Document) || (running && (changes & Session::Clock)))
        repaint();
}

std::vector<uint64_t> AutopilotView::layers() const
{
    std::vector<uint64_t> ids;
    const auto& top = session_.composition().layers;
    for (auto it = top.rbegin(); it != top.rend(); ++it) {
        if (!it->group)
            ids.push_back(it->id);
        for (auto inner = it->layers.rbegin(); inner != it->layers.rend(); ++inner)
            ids.push_back(inner->id);
    }
    return ids;
}

juce::Rectangle<int> AutopilotView::buttonArea(uint64_t layer, int button) const
{
    const auto ids = layers();
    const auto it = std::find(ids.begin(), ids.end(), layer);
    if (it == ids.end())
        return {};
    const int y = int(it - ids.begin() + 2) * (kRow + 1) - scroll_;
    if (button < 3)
        return {kNameWidth + button * 47, y + 1, 46, kRow - 2};
    return {kNameWidth + 3 * 47 + 38 + (button - 3) * (kButton + 1), y + 1, kButton, kRow - 2};
}

juce::Rectangle<int> AutopilotView::paceArea(int pace) const
{
    return {kNameWidth + pace * 37, 1 - scroll_, 36, kRow - 2};
}

juce::Rectangle<int> AutopilotView::columnArea(int button) const
{
    const int y = kRow + 1 - scroll_;
    if (button < 3)
        return {kNameWidth + button * 47, y + 1, 46, kRow - 2};
    return {kNameWidth + 3 * 47 + 38 + (button - 3) * (kButton + 1), y + 1, kButton, kRow - 2};
}

juce::String AutopilotView::columnStatus() const
{
    const int held = session_.client().clock().columns.held;
    return held > 0 && session_.composition().columnAutopilot.mode != AutopilotMode::Off
               ? "manual " + juce::String(held)
               : juce::String("every");
}

void AutopilotView::paintModes(juce::Graphics& g, std::function<juce::Rectangle<int>(int)> area,
                               const Autopilot& autopilot, bool paused, const juce::String& status) const
{
    const Palette& p = Look::palette();
    for (int b = 0; b < 10; ++b) {
        const bool on = b < 3 ? int(autopilot.mode) == b : autopilot.bars == kBars[b - 3];
        const juce::Colour fill = !on ? p.button : paused ? p.liveRow : p.live;
        fillWithText(g, area(b), fill, on && !paused ? p.liveInk : p.text,
                     b < 3 ? juce::String(kModes[b]) : juce::String(kBars[b - 3]), juce::Justification::centred, 0);
    }
    g.setColour(paused ? p.accent : p.muted);
    drawText(g, status, area(2).translated(47, 0).withWidth(38), juce::Justification::centred);
}

void AutopilotView::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    g.setFont(Look::font());
    g.setColour(p.text);
    drawText(g, "Pace", juce::Rectangle<int>{0, -scroll_, kNameWidth, kRow}.reduced(6, 0), juce::Justification::centredLeft);
    for (int i = 0; i < 5; ++i) {
        const bool on = session_.composition().pace == kPaces[i];
        fillWithText(g, paceArea(i), on ? p.live : p.button, on ? p.liveInk : p.text, kPaceNames[i],
                     juce::Justification::centred, 0);
    }
    const Autopilot& columns = session_.composition().columnAutopilot;
    const bool paused = columns.mode != AutopilotMode::Off;
    const juce::Rectangle<int> columnRow{0, columnArea(0).getY() - 1, getWidth(), kRow};
    g.setColour(p.cell);
    g.fillRect(columnRow);
    g.setColour(p.text);
    drawText(g, "Columns", columnRow.withWidth(kNameWidth).reduced(6, 0), juce::Justification::centredLeft, true);
    paintModes(g, [this](int b) { return columnArea(b); }, columns, false, columnStatus());
    for (uint64_t id : layers()) {
        const Layer* layer = session_.layer(id);
        if (!layer)
            continue;
        const auto first = buttonArea(id, 0);
        const juce::Rectangle<int> row{0, first.getY() - 1, getWidth(), kRow};
        g.setColour(p.cell);
        g.fillRect(row);
        g.setColour(p.text);
        drawText(g, utf8(layer->name), row.withWidth(kNameWidth).reduced(6, 0), juce::Justification::centredLeft, true);
        const Autopilot autopilot = deckAutopilot(*layer, session_.composition().deck);
        const bool layerPaused = paused && autopilot.mode != AutopilotMode::Off;
        paintModes(g, [this, id](int b) { return buttonArea(id, b); }, autopilot, layerPaused,
                   layerPaused ? "paused" : "every");
    }
    g.setColour(p.muted);
    drawText(g,
             paused ? "Columns on: the layers' Autopilots pause until it is off."
                    : "Autopilot plays clips of the shown deck every N bars times the pace, on the bar.",
             getLocalBounds().withTop(int(layers().size() + 2) * (kRow + 1) + 6 - scroll_).withHeight(18),
             juce::Justification::topLeft);
}

void AutopilotView::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    scroll_ = wheelScrolled(scroll_, wheel, int(layers().size() + 2) * (kRow + 1) + 24, getHeight());
    repaint();
}

void AutopilotView::mouseUp(const juce::MouseEvent& e)
{
    for (int i = 0; i < 5; ++i)
        if (paceArea(i).contains(e.getPosition())) {
            session_.client().edit(SetAutopilotPace{kPaces[i]});
            return;
        }
    for (int b = 0; b < 10; ++b)
        if (columnArea(b).contains(e.getPosition())) {
            if (b < 3)
                session_.client().edit(SetColumnAutopilot{AutopilotMode(b), std::nullopt});
            else
                session_.client().edit(SetColumnAutopilot{std::nullopt, kBars[b - 3]});
            return;
        }
    for (uint64_t id : layers())
        for (int b = 0; b < 10; ++b)
            if (buttonArea(id, b).contains(e.getPosition())) {
                if (b < 3)
                    session_.client().edit(autopilotEdit(session_.composition(), id, AutopilotMode(b), std::nullopt));
                else
                    session_.client().edit(autopilotEdit(session_.composition(), id, std::nullopt, kBars[b - 3]));
                return;
            }
}

juce::String AutopilotView::tipAt(juce::Point<int> point)
{
    for (int i = 0; i < 5; ++i)
        if (paceArea(i).contains(point))
            return "Pace: multiplies every Autopilot step and hold";
    for (int b = 0; b < 10; ++b)
        if (columnArea(b).contains(point)) {
            if (b == 0)
                return "Column Autopilot off: the layers' own Autopilots play";
            if (b < 3)
                return juce::String("Trigger the ") + (b == 1 ? "next" : "a random") +
                       " column of the shown deck on every layer each step; the layers' Autopilots pause";
            return "The column Autopilot moves on every " + barsText(kBars[b - 3]) + " (times the pace)";
        }
    for (uint64_t id : layers())
        for (int b = 0; b < 10; ++b)
            if (buttonArea(id, b).contains(point)) {
                if (b == 0)
                    return "Autopilot off: this layer plays what you trigger";
                if (b < 3)
                    return juce::String("Autopilot plays this layer's ") + (b == 1 ? "next" : "a random") +
                           " clip of the shown deck on each step";
                return "Autopilot moves on every " + barsText(kBars[b - 3]) + " (times the pace)";
            }
    return {};
}

HistoryView::HistoryView(Session& session) : session_(session)
{
    setTooltip(juce::String::fromUTF8("Every edit, newest nearest Now: âZ undoes, â§âZ redoes"));
    session_.addView(this);
}

HistoryView::~HistoryView()
{
    session_.removeView(this);
}

void HistoryView::sessionChanged(int changes)
{
    if (changes & Session::Document)
        repaint();
}

juce::Rectangle<int> HistoryView::rowArea(size_t index) const
{
    return {0, int(index) * (kRow + 1) - scroll_, getWidth(), kRow};
}

size_t HistoryView::rowCount() const
{
    return session_.futureEdits().size() + 1 + session_.pastEdits().size();
}

void HistoryView::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    scroll_ = wheelScrolled(scroll_, wheel, int(rowCount()) * (kRow + 1), getHeight());
    repaint();
}

void HistoryView::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    g.setFont(Look::font());
    const auto& past = session_.pastEdits();
    const auto& future = session_.futureEdits();
    size_t index = 0;
    auto line = [&](const juce::String& mark, const juce::String& text, juce::Colour fill, juce::Colour ink) {
        const juce::Rectangle<int> row = rowArea(index++);
        if (row.intersects(getLocalBounds()))
            fillWithText(g, row, fill, ink, mark + "  " + text);
    };
    for (const std::string& op : future)
        line(juce::String::fromUTF8("\xe2\x86\xb7"), utf8(op), p.cell, p.muted);
    line(juce::String::fromUTF8("\xe2\x97\x8f"), "Now", juce::Colour(0xff2c2c2c), p.white);
    for (auto it = past.rbegin(); it != past.rend() && rowArea(index).getY() < getHeight(); ++it)
        line(juce::String::fromUTF8("\xe2\x86\xb6"), utf8(*it), p.cell, p.text);
    if (past.empty() && future.empty()) {
        g.setColour(p.muted);
        g.drawFittedText(juce::String::fromUTF8("Every edit lands here; playback (triggers, Autopilot) does not. "
                                                "\xe2\x8c\x98Z / \xe2\x87\xa7\xe2\x8c\x98Z."),
                         getLocalBounds().withTop(rowArea(index).getY() + 4).reduced(4, 0), juce::Justification::topLeft, 3);
    }
}

} // namespace azl::ui
