#include "ui/app/look.hpp"

#include <unordered_map>

namespace azl::ui {

namespace {

const Palette kPalette;
const Metrics kMetrics;

// Copies share one font, which keeps its resolved typeface; labels keep their shaped line. Released with JUCE, before
// static destruction, which would outlive the typefaces.
struct TextCache : juce::DeletedAtShutdown {
    ~TextCache() override { current() = nullptr; }
    static TextCache*& current()
    {
        static TextCache* cache = nullptr;
        return cache;
    }
    static TextCache& get()
    {
        if (!current())
            current() = new TextCache;
        return *current();
    }

    juce::Font plain{juce::FontOptions(kMetrics.fontSize, juce::Font::plain)};
    juce::Font bold{juce::FontOptions(kMetrics.fontSize, juce::Font::bold)};
    juce::Font small{juce::FontOptions(kMetrics.smallFontSize)};
    std::unordered_map<juce::String, juce::GlyphArrangement> shaped;
};

} // namespace

const Palette& Look::palette()
{
    return kPalette;
}

const Metrics& Look::metrics()
{
    return kMetrics;
}

juce::Font Look::font(bool bold)
{
    return bold ? TextCache::get().bold : TextCache::get().plain;
}

juce::Font Look::smallFont()
{
    return TextCache::get().small;
}

Look::Look()
{
    const Palette& p = kPalette;
    setColour(juce::ResizableWindow::backgroundColourId, p.background);
    setColour(juce::DocumentWindow::textColourId, p.text);
    setColour(juce::TextButton::buttonColourId, p.button);
    setColour(juce::TextButton::buttonOnColourId, p.live);
    setColour(juce::TextButton::textColourOffId, p.text);
    setColour(juce::TextButton::textColourOnId, p.liveInk);
    setColour(juce::ComboBox::backgroundColourId, p.button);
    setColour(juce::ComboBox::textColourId, p.text);
    setColour(juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
    setColour(juce::ComboBox::arrowColourId, p.muted);
    setColour(juce::PopupMenu::backgroundColourId, p.cell);
    setColour(juce::PopupMenu::textColourId, p.text);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, p.liveRow);
    setColour(juce::PopupMenu::highlightedTextColourId, p.white);
    setColour(juce::Label::textColourId, p.text);
    setColour(juce::TextEditor::backgroundColourId, p.field);
    setColour(juce::TextEditor::textColourId, p.text);
    setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff333333));
    setColour(juce::TextEditor::focusedOutlineColourId, p.live);
    setColour(juce::ScrollBar::thumbColourId, juce::Colour(0xff3a3a3a));
    setColour(juce::ListBox::backgroundColourId, p.panel);
    setColour(juce::TabbedButtonBar::tabTextColourId, p.muted);
    setColour(juce::TabbedButtonBar::frontTextColourId, p.white);
}

void Look::drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour& colour, bool highlighted,
                                bool down)
{
    const juce::Colour on = button.getProperties()["accent"] ? kPalette.accent : kPalette.live;
    juce::Colour fill = button.getToggleState() ? on : colour;
    if (button.getProperties()["fault"])
        fill = kPalette.fault;
    if ((highlighted || down) && !button.getToggleState())
        fill = kPalette.buttonHover;
    g.setColour(fill);
    g.fillRect(button.getLocalBounds());
}

juce::Font Look::getTextButtonFont(juce::TextButton&, int)
{
    return font();
}

void Look::drawComboBox(juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    g.setColour(box.findColour(juce::ComboBox::backgroundColourId));
    g.fillRect(0, 0, width, height);
    drawChevron(g, {width - kMetrics.chevronBox, 0, kMetrics.chevronBox, height}, true,
                box.findColour(juce::ComboBox::arrowColourId));
}

juce::Font Look::getComboBoxFont(juce::ComboBox&)
{
    return font();
}

void Look::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    label.setBounds(1, 0, box.getWidth() - kMetrics.chevronBox, box.getHeight());
    label.setFont(font());
    label.setBorderSize(juce::BorderSize<int>(0, 5, 0, 0));
}

juce::Font Look::getPopupMenuFont()
{
    return font();
}

void Look::drawPopupMenuBackground(juce::Graphics& g, int width, int height)
{
    g.fillAll(kPalette.cell);
    g.setColour(kPalette.line);
    g.drawRect(0, 0, width, height);
}

juce::Font Look::getLabelFont(juce::Label&)
{
    return font();
}

void Look::drawScrollbar(juce::Graphics& g, juce::ScrollBar&, int x, int y, int width, int height, bool vertical,
                         int thumbStart, int thumbSize, bool over, bool)
{
    g.setColour(juce::Colour(0xff111111));
    g.fillRect(x, y, width, height);
    g.setColour(over ? juce::Colour(0xff555555) : juce::Colour(0xff3a3a3a));
    if (vertical)
        g.fillRect(x, thumbStart, width, thumbSize);
    else
        g.fillRect(thumbStart, y, thumbSize, height);
}

void Look::drawTabButton(juce::TabBarButton& button, juce::Graphics& g, bool over, bool)
{
    const bool front = button.isFrontTab();
    g.setColour(front ? kPalette.panel : kPalette.header);
    g.fillRect(button.getLocalBounds().withTrimmedRight(1));
    g.setColour(front ? kPalette.white : over ? kPalette.text : kPalette.muted);
    g.setFont(font());
    drawText(g, button.getButtonText(), button.getLocalBounds(), juce::Justification::centred, true);
}

int Look::getTabButtonBestWidth(juce::TabBarButton& button, int)
{
    return juce::GlyphArrangement::getStringWidthInt(font(), button.getButtonText()) + 22;
}

void drawText(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area,
              juce::Justification justification, bool ellipses)
{
    if (text.isEmpty() || area.isEmpty())
        return;
    auto& shaped = TextCache::get().shaped;
    const juce::Font font = g.getCurrentFont();
    const juce::String key = text + "\x1f" + font.toString() + "\x1f" + juce::String(area.getWidth()) + "x" +
                             juce::String(area.getHeight()) + "\x1f" + juce::String(justification.getFlags()) +
                             (ellipses ? "e" : "");
    auto it = shaped.find(key);
    if (it == shaped.end()) {
        if (shaped.size() > 4000)
            shaped.clear();
        juce::GlyphArrangement line;
        line.addCurtailedLineOfText(font, text, 0.0f, 0.0f, area.getWidth(), ellipses);
        line.justifyGlyphs(0, line.getNumGlyphs(), 0.0f, 0.0f, area.getWidth(), area.getHeight(), justification);
        it = shaped.emplace(key, std::move(line)).first;
    }
    it->second.draw(g, juce::AffineTransform::translation(area.getX(), area.getY()));
}

void drawText(juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area,
              juce::Justification justification, bool ellipses)
{
    drawText(g, text, area.toFloat(), justification, ellipses);
}

void drawText(juce::Graphics& g, const juce::String& text, int x, int y, int width, int height,
              juce::Justification justification, bool ellipses)
{
    drawText(g, text, juce::Rectangle<int>(x, y, width, height), justification, ellipses);
}

void fillWithText(juce::Graphics& g, juce::Rectangle<int> area, juce::Colour fill, juce::Colour ink,
                  const juce::String& text, juce::Justification justification, int indent)
{
    g.setColour(fill);
    g.fillRect(area);
    g.setColour(ink);
    drawText(g, text, area.reduced(indent, 0), justification);
}

void drawBar(juce::Graphics& g, juce::Rectangle<int> area, float fraction, juce::Colour fill)
{
    g.setColour(Look::palette().field);
    g.fillRect(area);
    g.setColour(fill);
    g.fillRect(area.withWidth(juce::roundToInt(float(area.getWidth()) * juce::jlimit(0.0f, 1.0f, fraction))));
}

void drawChevron(juce::Graphics& g, juce::Rectangle<int> box, bool open, juce::Colour colour)
{
    const auto c = box.toFloat().getCentre();
    const float half = 5.0f, depth = 3.0f;
    juce::Path arrow;
    if (open)
        arrow.addTriangle(c.x - half, c.y - depth, c.x + half, c.y - depth, c.x, c.y + depth);
    else
        arrow.addTriangle(c.x - depth, c.y - half, c.x - depth, c.y + half, c.x + depth, c.y);
    g.setColour(colour);
    g.fillPath(arrow);
}

juce::Rectangle<int> chevronBox(juce::Rectangle<int> header)
{
    const int size = kMetrics.chevronBox;
    return {header.getX(), header.getCentreY() - size / 2, size, size};
}

void drawHeader(juce::Graphics& g, juce::Rectangle<int> area, juce::Colour fill, juce::Colour ink,
                const juce::String& label, bool open)
{
    g.setColour(fill);
    g.fillRect(area);
    drawChevron(g, chevronBox(area), open, ink);
    g.setColour(ink);
    drawText(g, label, area.withTrimmedLeft(kMetrics.chevronBox).withTrimmedRight(5), juce::Justification::centredLeft);
}

} // namespace azl::ui
