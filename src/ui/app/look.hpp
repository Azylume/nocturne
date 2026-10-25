#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace azl::ui {

// Every colour, size and font of the UI lives here, so a restyle changes this file only. The look follows Resolume
// Arena: flat grey panels, dense small text, no rounded corners or gradients.
struct Palette {
    juce::Colour background{0xff0d0d0d};
    juce::Colour panel{0xff171717};
    juce::Colour header{0xff141414};
    juce::Colour bar{0xff1b1b1b};
    juce::Colour cell{0xff202020};
    juce::Colour emptyCell{0xff191919};
    juce::Colour button{0xff2a2a2a};
    juce::Colour buttonHover{0xff363636};
    juce::Colour field{0xff0f0f0f};
    juce::Colour line{0xff000000};
    juce::Colour text{0xffd2d2d2};
    juce::Colour muted{0xff8a8a8a};
    juce::Colour white{0xfff2f2f2};
    juce::Colour live{0xff7ef0c6};
    juce::Colour liveInk{0xff0b2b20};
    juce::Colour liveRow{0xff1c4538};
    juce::Colour clipLabel{0xff9b7b88};
    juce::Colour groupLabel{0xff5d6f8a};
    juce::Colour selectedLabel{0xffe4d3da};
    juce::Colour accent{0xffe9b04b};
    juce::Colour fault{0xffd9534f};
    juce::Colour meter{0xff3e8a73};
    juce::Colour mapCanvas{0xff111111};
    juce::Colour mapFrame{0xff333333};
    juce::Colour mapSlice{0xff444444};
    juce::Colour mapSliceOn{0xff5a8a7a};
    juce::Colour mapMask{0xff3a3a3a};
    juce::Colour mapMaskOn{0xff8a6a30};
    // One per screen in the Map Input view.
    std::array<juce::Colour, 8> screenColours{juce::Colour(0xff7ef0c6), juce::Colour(0xffe9b04b),
                                              juce::Colour(0xff5cc8ff), juce::Colour(0xffff7aa8),
                                              juce::Colour(0xffb48cff), juce::Colour(0xff9be36b),
                                              juce::Colour(0xffffd36b), juce::Colour(0xff6bf0ff)};
};

struct Metrics {
    int transportHeight = 32;
    int deckTabsHeight = 26;
    int columnRowHeight = 22;
    int labelHeight = 17;
    int paneTabsHeight = 22;
    int layerControlWidth = 140;
    int chevronBox = 22;
    int columns = 12;
    // A clip cell is never narrower: with more columns than fit, the grid scrolls sideways.
    int cellWidth = 86;
    int gap = 2;
    float fontSize = 12.0f;
    float smallFontSize = 11.0f;
};

class Look : public juce::LookAndFeel_V4 {
public:
    Look();

    static const Palette& palette();
    static const Metrics& metrics();
    static juce::Font font(bool bold = false);
    static juce::Font smallFont();

    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted,
                              bool down) override;
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
    void drawComboBox(juce::Graphics&, int width, int height, bool down, int buttonX, int buttonY, int buttonW,
                      int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    void positionComboBoxText(juce::ComboBox&, juce::Label&) override;
    juce::Font getPopupMenuFont() override;
    void drawPopupMenuBackground(juce::Graphics&, int width, int height) override;
    juce::Font getLabelFont(juce::Label&) override;
    void drawScrollbar(juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height, bool vertical,
                       int thumbStart, int thumbSize, bool over, bool down) override;
    int getDefaultScrollbarWidth() override { return 8; }
    void drawTabButton(juce::TabBarButton&, juce::Graphics&, bool over, bool down) override;
    int getTabButtonBestWidth(juce::TabBarButton&, int tabDepth) override;
    void drawTabAreaBehindFrontButton(juce::TabbedButtonBar&, juce::Graphics&, int, int) override {}
};

inline juce::String utf8(const std::string& text)
{
    return juce::String::fromUTF8(text.c_str());
}

// Graphics::drawText with the shaped line kept, so a label is shaped once instead of on every paint.
void drawText(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area,
              juce::Justification justification, bool ellipses = true);
void drawText(juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area,
              juce::Justification justification, bool ellipses = true);
void drawText(juce::Graphics& g, const juce::String& text, int x, int y, int width, int height,
              juce::Justification justification, bool ellipses = true);

// Small drawing helpers every panel shares.
void fillWithText(juce::Graphics& g, juce::Rectangle<int> area, juce::Colour fill, juce::Colour ink,
                  const juce::String& text, juce::Justification justification = juce::Justification::centredLeft,
                  int indent = 5);
void drawBar(juce::Graphics& g, juce::Rectangle<int> area, float fraction, juce::Colour fill);
// A disclosure chevron or dropdown arrow centred in `box`: pointing down when open, right when closed.
void drawChevron(juce::Graphics& g, juce::Rectangle<int> box, bool open, juce::Colour colour);
// The square at the left of a header row that its chevron sits in.
juce::Rectangle<int> chevronBox(juce::Rectangle<int> header);
// A collapsible header row: its fill, the chevron and the label after it, in the current font.
void drawHeader(juce::Graphics& g, juce::Rectangle<int> area, juce::Colour fill, juce::Colour ink,
                const juce::String& label, bool open);

} // namespace azl::ui
