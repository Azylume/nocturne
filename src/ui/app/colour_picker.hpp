#pragma once

#include "document/palette.hpp"
#include "ui/app/step_strip.hpp"
#include "ui/app/widgets.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace azl::ui {

// The colour the engine renders: the document's, with the channels macros and modulators drive at their live values.
std::optional<Hsva> liveColour(const Session& session, const ColourRef& ref);
void paintSwatch(juce::Graphics& g, juce::Rectangle<int> area, const Rgba& colour);

// Picks a colour: a saturation and value square beside a hue strip, an alpha strip when the colour has alpha, a hex
// field, the colours picked last and the composition palette; or, switched over, the colour's sliders. A drag is one
// undo step. A click on a palette swatch picks it; the current colour dragged onto an empty one adds it.
class ColourPicker : public juce::Component, public PointerTips, private Session::View {
public:
    ColourPicker(Session& session, ColourRef ref);
    ~ColourPicker() override;

    const ColourRef& ref() const { return ref_; }
    const Hsva& colour() const { return colour_; }
    bool sliders() const { return sliders_; }
    void showSliders(bool on);
    juce::TextEditor& hexField() { return hex_; }
    juce::TextButton& viewButton() { return view_; }
    // The picker's parts; empty while the sliders show.
    juce::Rectangle<int> squareArea() const;
    juce::Rectangle<int> hueArea() const;
    juce::Rectangle<int> alphaArea() const;
    juce::Rectangle<int> recentArea(size_t index) const;
    // The current colour, which drags onto an empty palette swatch.
    juce::Rectangle<int> previewArea() const;
    // The palette's swatches, its colours then empty ones to fill the row.
    juce::Rectangle<int> paletteArea(size_t index) const;
    size_t paletteCells() const;
    // A palette colour's right-click menu: link the colour to it or unlink it, update it with the current colour,
    // remove it, clear the palette.
    Menu paletteMenu(size_t slot);
    // What a pick edits: the palette colour a linked colour follows, which recolours every colour linked to it; else
    // the colour itself.
    ColourRef target() const;
    juce::TextButton& unlinkButton() { return unlink_; }
    // The colour's sliders while they show: its parameters' rows, or for a text colour one field per channel.
    const std::vector<std::unique_ptr<ParamRow>>& rows() const { return rows_; }
    const std::vector<std::unique_ptr<NumberField>>& fields() const { return fields_; }

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    juce::String tipAt(juce::Point<int> point) override;

    static constexpr int kWidth = 220;

private:
    enum class Part { None, Square, Hue, Alpha, Preview };

    void sessionChanged(int changes) override;
    void follow();
    void layout();
    void buildSliders();
    Part partAt(juce::Point<int> point) const;
    void dragTo(juce::Point<int> point);
    void send(bool force);
    // One click: the colour as one undo step, remembered among the recent ones.
    void pick(const Hsva& colour);
    void typed();
    juce::Rectangle<int> controlsRow() const;
    int recentTop() const;
    int paletteTop() const;
    std::optional<size_t> paletteCellAt(juce::Point<int> point) const;
    // The palette with one colour replaced, or removed when `colour` is none.
    void editPalette(size_t slot, std::optional<std::array<double, 4>> colour);
    void sendColour(const Hsva& colour);
    bool linkable() const;

    Session& session_;
    const ColourRef ref_;
    const bool alpha_;
    Hsva colour_;
    Part dragging_ = Part::None;
    std::optional<juce::Point<int>> carried_;
    double lastSend_ = 0.0;
    bool sliders_ = false;
    juce::TextEditor hex_;
    juce::TextButton view_{"Sliders"};
    juce::TextButton unlink_{"Unlink"};
    std::vector<std::unique_ptr<ParamRow>> rows_;
    std::vector<std::unique_ptr<NumberField>> fields_;
    juce::Image square_;
    double squareHue_ = -1.0;
};

// A colour's swatch: its live colour over a checkerboard for alpha. A click opens its picker beside it.
class Swatch : public juce::Component, public juce::SettableTooltipClient {
public:
    Swatch(Session& session, ColourRef ref);
    ~Swatch() override;

    const ColourRef& ref() const { return ref_; }
    // Repaints when the colour it shows has moved, and says whether it had.
    bool refresh();
    std::optional<Rgba> shown() const { return shown_; }
    void openPicker();
    void closePicker();
    ColourPicker* picker() const { return picker_.get(); }

    void paint(juce::Graphics&) override;
    void mouseUp(const juce::MouseEvent&) override;

private:
    void dropPicker(const juce::CallOutBox* box);

    Session& session_;
    const ColourRef ref_;
    std::optional<Rgba> shown_;
    std::unique_ptr<ColourPicker> picker_;
    std::unique_ptr<juce::CallOutBox> box_;
};

// A colour's row in a card: its name, a wide swatch and its hex value.
class SwatchRow : public juce::Component {
public:
    SwatchRow(Session& session, ColourRef ref);

    Swatch& swatch() { return swatch_; }
    void refresh()
    {
        swatch_.refresh();
        repaint();
    }
    // A modulation update: repaints only when the colour it shows has moved.
    void refreshDriven()
    {
        if (swatch_.refresh())
            repaint();
    }

    void paint(juce::Graphics&) override;
    void resized() override;

    static constexpr int kHeight = 20;

private:
    Session& session_;
    Swatch swatch_;
};

// The composition palettes: a chip per palette (click selects it, live; the active one outlined, the one the show is
// on lit, the Autopilot's next one marked; double-click renames, right-click removes it, makes it a deck's palette or
// gives a macro the palette position), + to add one, the palette Autopilot badge; then a row per colour of the active
// palette, with a swatch to pick it, the colour the show draws now, its name (double-click to rename) and a cross to
// remove it; right-click gives a macro its hue. "+ Colour" adds one.
class PaletteView : public juce::Component, public PointerTips, private Session::View {
public:
    explicit PaletteView(Session& session);
    ~PaletteView() override;

    juce::TextButton& addButton() { return add_; }
    PilotBadge& pilotBadge() { return pilot_; }
    Swatch* swatch(size_t slot) const { return slot < swatches_.size() ? swatches_[slot].get() : nullptr; }
    juce::Rectangle<int> nameArea(size_t slot) const;
    juce::Rectangle<int> removeArea(size_t slot) const;
    // The active palette's chip.
    juce::Rectangle<int> titleArea() const;
    juce::Rectangle<int> chipArea(size_t palette) const;
    juce::Rectangle<int> newPaletteArea() const;
    juce::Rectangle<int> liveArea(size_t slot) const;
    void rename(size_t slot, const std::string& name);
    void renamePalette(const std::string& name);
    // A copy of the active palette's colours, so the palettes match by slot.
    void addPalette();
    Menu chipMenu(size_t palette);
    Menu colourMenu(size_t slot);

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    juce::String tipAt(juce::Point<int> point) override;

private:
    void sessionChanged(int changes) override;
    void rebuild();
    juce::Rectangle<int> rowArea(size_t slot) const;
    // A submenu of the macros, each adding a target on the palette parameter.
    Menu macroMenu(const std::string& param);

    Session& session_;
    juce::TextButton add_{"+ Colour"};
    PilotBadge pilot_;
    std::optional<ClockState::EffectSteps> shown_;
    std::vector<std::array<double, 4>> live_;
    std::vector<std::unique_ptr<Swatch>> swatches_;
    std::string shape_;
    std::unique_ptr<juce::TextEditor> editor_;
};

} // namespace azl::ui
