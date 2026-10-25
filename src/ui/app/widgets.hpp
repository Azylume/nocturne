#pragma once

#include "ui/app/session.hpp"
#include "ui/app/tooltips.hpp"

#include <functional>
#include <memory>

namespace azl::ui {

// What a modulator offers: its sources, a beat LFO's shapes and lengths in beats.
inline const char* const kSources[] = {"beat", "kick", "snare", "hat", "bass", "lowBand", "midBand", "highBand"};
inline const char* const kSourceNames[] = {"Beat (LFO)", "Kick", "Snare", "Hat", "Bass", "Low", "Mid", "High"};
inline constexpr int kSourceCount = int(std::size(kSources));
inline const LfoShape kShapes[] = {LfoShape::Sine, LfoShape::Saw,    LfoShape::Square,
                                   LfoShape::Ramp, LfoShape::Random, LfoShape::Custom};
inline const char* const kShapeNames[] = {"Sine", "Saw", "Square", "Ramp", "Random", "Custom"};
inline const int kBeats[] = {1, 2, 4, 8, 16};

// A text field over `area` of `parent` with `text` selected; Enter or leaving it calls `done` with the new text (not
// when unchanged or empty), Esc drops it. The field lives in `slot`. `keys` sees each key first and may take it.
void editInline(std::unique_ptr<juce::TextEditor>& slot, juce::Component& parent, juce::Rectangle<int> area,
                const juce::String& text, std::function<void(const juce::String&)> done,
                std::function<bool(const juce::KeyPress&)> keys = {});

// Asks for a name in a small window; `done` gets it unless cancelled or empty.
void askName(const juce::String& title, const juce::String& message, const juce::String& initial,
             std::function<void(const juce::String&)> done);

// A popup menu as data, so the same entries are shown and run by tests. An entry with neither `run` nor `sub` is a
// separator. Each entry shows its shortcut on the right and its tip as a tooltip; a mixed one (some of the selected
// clips have it) shows a dash.
struct MenuEntry {
    juce::String label;
    std::function<void()> run;
    bool enabled = true;
    bool ticked = false;
    std::vector<MenuEntry> sub;
    juce::String shortcut{};
    juce::String tip{};
    bool mixed = false;
};
using Menu = std::vector<MenuEntry>;
void showMenu(juce::Component& target, Menu menu);
// The Add effect submenu: an entry per effect type, or with saved presets a submenu of its default and each preset.
Menu addEffectMenu(Session& session, const std::function<void(const Effect&)>& add);
struct MidiTarget;
// MIDI learn for the target, and Forget MIDI when a control is mapped to it.
void addMidiEntries(Menu& menu, Session& session, const MidiTarget& target);

// The Sources tab's text clip, beside the generator types.
inline const std::string kTextSource = "text";
// "Solid" for solid.
juce::String generatorLabel(const std::string& type);
// A clip of the Sources tab's `type`, a generator or kTextSource, with its default settings, named after it.
ClipRef generatorClip(const std::string& type);
// A small picture of the generator's look (or of text), for its cell and the Sources tab.
void paintGenerator(juce::Graphics& g, juce::Rectangle<int> area, const Effect& generator);
// The entry at `path`, submenus separated by "/" ("Group/None"); null when missing.
const MenuEntry* menuEntry(const Menu& menu, const juce::String& path);

// The scroll offset of a list painted by hand, `content` pixels tall in `visible` pixels, after a wheel or trackpad
// move; the smallest trackpad step still moves it a pixel.
int wheelScrolled(int offset, const juce::MouseWheelDetails& wheel, int content, int visible);

// A number dragged sideways or scrolled (Shift: ten steps at a time), or typed after a click; while typing, the up and
// down keys step it. A drag, and a run of wheel steps, is one gesture; `onEnd` follows each.
class NumberField : public juce::Component, public juce::SettableTooltipClient, private juce::Timer {
public:
    NumberField(Session& session, double min, double max, double step, juce::String suffix = {});

    // None shows "mixed": the clips it edits differ.
    void setValue(std::optional<double> value);
    double value() const { return value_; }
    bool mixed() const { return mixed_; }
    std::function<void(double)> onChange;
    std::function<void()> onEnd;
    bool wheel = true;
    bool editing() const { return editor_ != nullptr; }

    void paint(juce::Graphics&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;

private:
    void set(double value);
    void startEditing();
    void timerCallback() override;
    bool stepKey(const juce::KeyPress&);

    Session& session_;
    const double min_, max_, step_;
    const juce::String suffix_;
    double value_ = 0.0;
    bool mixed_ = false;
    double dragStart_ = 0.0;
    bool dragging_ = false;
    bool wheeling_ = false;
    double wheelRest_ = 0.0;
    std::unique_ptr<juce::TextEditor> editor_;
};

// A button for an action that takes a show down: it acts only after being held for `hold` seconds, filling with
// progress meanwhile, and fires once per press; released early, it does nothing.
class HoldButton : public juce::Component, public juce::SettableTooltipClient, private juce::Timer {
public:
    static constexpr double kHold = 1.5;

    explicit HoldButton(juce::String text, double hold = kHold);

    void setText(const juce::String& text);
    std::function<void()> onHold;
    void press();
    void release();
    // 0..1 while pressed.
    double progress() const;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void enablementChanged() override;

private:
    void timerCallback() override;

    juce::String text_;
    const double hold_;
    std::optional<double> pressedAt_;
    bool fired_ = false;
};

void drawCurve(juce::Graphics& g, juce::Rectangle<float> area, const Curve& curve, juce::Colour colour);

// Drags the two control points of a curve; a drag is one gesture.
class CurveEditor : public juce::Component, public juce::SettableTooltipClient {
public:
    CurveEditor(Session& session, Curve curve, std::function<void(const Curve&)> changed);

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> plot() const;
    juce::Point<float> handle(int i) const;

    Session& session_;
    Curve curve_;
    std::function<void(const Curve&)> changed_;
    int dragging_ = -1;
};

// Puts a beat LFO on `shape`; Custom without points of its own starts from the shape it had.
void setLfoShape(Modulator& modulator, LfoShape shape);

// A beat LFO's curve over one cycle as it moves its target (curve, From and To and the LFO sensitivity applied), with
// a playhead at the live phase. On Custom, double-click adds a point or makes one smooth or straight, dragging moves
// one between its neighbours and right-click removes one; a drag is one gesture.
class LfoEditor : public juce::Component, public juce::TooltipClient, private juce::Timer {
public:
    using Change = std::function<void(const std::function<void(Modulator&)>&)>;
    LfoEditor(Session& session, std::function<const Modulator*()> modulator, Change change);

    juce::Rectangle<float> plot() const;
    // The plotted height (0..1 of the target's range) at `x` periods, and where point `index` sits.
    double plotAt(double x) const;
    juce::Point<float> pointPosition(size_t index) const;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    juce::String getTooltip() override;

    static constexpr int kHeight = 56;

private:
    // The modulator with the points being dragged.
    std::optional<Modulator> shown() const;
    double sourceAt(const Modulator& m, float y, double previous) const;
    std::optional<size_t> pointAt(juce::Point<float> position) const;
    void sendPoints();
    void timerCallback() override;

    Session& session_;
    std::function<const Modulator*()> modulator_;
    Change change_;
    std::optional<size_t> dragging_;
    std::vector<LfoPoint> dragPoints_;
    bool moved_ = false;
    double lastSend_ = 0.0;
};

// An audio modulator's envelope on one line: its preset, or for an old show's raw modulator a flag that puts it on
// Meter with one click; the attack, hold and release fields open behind the "ms" toggle, folded by default.
class EnvelopeControls : public juce::Component {
public:
    using Change = std::function<void(const std::function<void(Modulator&)>&)>;
    EnvelopeControls(Session& session, Change change);

    void refresh(const Modulator& modulator);
    void resized() override;
    void paint(juce::Graphics&) override;
    juce::ComboBox& presetBox() { return preset_; }
    juce::TextButton& rawFlag() { return raw_; }
    juce::TextButton& timesToggle() { return times_; }
    NumberField& attackField() { return attack_; }
    NumberField& holdField() { return hold_; }
    NumberField& releaseField() { return release_; }

private:
    void layOut();

    Change change_;
    juce::ComboBox preset_;
    juce::TextButton raw_{"Raw: click for Meter"}, times_{"ms"};
    NumberField attack_, hold_, release_;
    bool rawShown_ = false;
    bool open_ = false;
};

// The settings of a parameter's animation, dropped down under its row: source, shape and length of a beat LFO or
// the envelope of an audio source, and the range it sweeps.
class AnimateBox : public juce::Component {
public:
    AnimateBox(Session& session, ParamRef ref);
    AnimateBox(Session& session, const ParamInfo& info);

    void refresh();
    void resized() override;
    void paint(juce::Graphics&) override;
    juce::ComboBox& sourceBox() { return source_; }
    juce::ComboBox& beatsBox() { return beats_; }
    EnvelopeControls& envelope() { return envelope_; }
    // The animation's ends in the parameter's unit: from (or an audio source's rest) and to (its peak).
    NumberField& fromField() { return from_; }
    NumberField& toField() { return to_; }
    juce::ComboBox& shapeBox() { return shape_; }
    LfoEditor& lfoEditor() { return lfo_; }
    // Taller by the LFO editor for a beat LFO.
    int preferredHeight() const;

    static constexpr int kHeight = 64;

private:
    void send(const std::function<void(Modulator&, const ParamInfo&)>& change);

    Session& session_;
    const ParamRef ref_;
    const double scale_;
    juce::ComboBox source_, shape_, beats_;
    juce::TextButton stop_{juce::String::fromUTF8("\xc3\x97")};
    EnvelopeControls envelope_;
    NumberField from_, to_;
    LfoEditor lfo_;
    bool beat_ = true;
};

// A parameter's bar with a range on it: the band between the two ends (wrapping round the bar's end for a wrapping
// parameter), a handle at each end (from muted, to bright) and the live value as a line.
void drawRangeBar(juce::Graphics& g, juce::Rectangle<int> bar, const ParamInfo& info, Ends ends,
                  std::optional<double> live);
// The handle (0 from, 1 to) within a few pixels of `x`, the nearer one when both are.
std::optional<int> rangeHandleAt(juce::Rectangle<int> bar, const ParamInfo& info, Ends ends, int x);
// The parameter's value at `x` on the bar.
double valueAtX(juce::Rectangle<int> bar, const ParamInfo& info, int x);

// One parameter: its name (with "~" when animated), a bar to drag and its value; double-click the value to type it,
// right-click for macro and animation. In learn mode a click on the bar adds the parameter to the selected macro.
// An animated parameter shows its range on the bar: dragging a handle moves that end, past the other one too.
// A choice with named options shows a dropdown of them instead of the bar and the value, or a checkbox when they are
// Off and On, set to the driven option while a macro or modulator drives it. The label's tooltip names the parameter
// in full.
class ParamRow : public juce::Component, public PointerTips {
public:
    ParamRow(Session& session, ParamRef ref);

    const ParamRef& ref() const { return ref_; }
    AnimateBox* animateBox() { return animate_.get(); }
    // Null unless the parameter has named options other than Off and On.
    juce::ComboBox* optionBox() { return options_.get(); }
    // Null unless the parameter's options are Off and On.
    juce::ToggleButton* switchBox() { return switch_.get(); }
    void refresh();
    // A modulation update: repaints only when the value its modulation drives has moved.
    void refreshDriven();
    int preferredHeight() const;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    juce::String tipAt(juce::Point<int> point) override;

    static constexpr int kHeight = 22;
    juce::Rectangle<int> barArea() const;
    juce::Rectangle<int> tagArea() const;
    juce::Rectangle<int> labelArea() const;
    // Where the animation's end (0 from, 1 to) sits on the bar; nothing when not animated.
    std::optional<int> handleX(int end) const;

private:
    juce::Rectangle<int> valueArea() const;
    void showOptions(const ParamInfo& info);
    void showSwitch(const ParamInfo& info);
    void setFromX(int x);
    void setEndFromX(int x);
    void sendEnds();
    void showMenu();

    Session& session_;
    const ParamRef ref_;
    int valueWidth_ = 0;
    std::optional<std::pair<double, double>> measuredRange_;
    std::optional<double> dragValue_;
    // The end being dragged and the animation's offsets as dragged so far.
    std::optional<int> dragEnd_;
    std::pair<double, double> dragOffsets_;
    double lastSend_ = 0.0;
    std::unique_ptr<AnimateBox> animate_;
    std::unique_ptr<juce::TextEditor> editor_;
    std::optional<double> shownDriven_;
    std::unique_ptr<juce::ComboBox> options_;
    std::unique_ptr<juce::ToggleButton> switch_;
};

// Every parameter row under `root` takes a modulation update.
void refreshDrivenRows(juce::Component& root);

juce::String formatValue(const ParamInfo& info, double value);
// "0% to 100%": the parameter's range in its unit.
juce::String rangeText(const ParamInfo& info);

// A semi-transparent picture of what `component` draws over `area`: the item a drag carries.
juce::ScaledImage dragImage(juce::Component& component, juce::Rectangle<int> area);
// Drags `description` from `component` with the picture of the item at `area`, held where the mouse took it.
void startDrag(juce::Component& component, const juce::String& description, juce::Rectangle<int> area,
               const juce::MouseEvent& e);
// A dragged effect type ("effect:TYPE"), preset ("preset:" and its JSON: effectType, name, params) or chain preset
// ("chain:" and its JSON: name, effects).
bool isEffectDrag(const juce::String& description);
// The effect a drag adds, with the preset's parameters; none for other drags and chain presets.
std::optional<Effect> draggedEffect(const juce::String& description);
// The effects a drag adds: a type's from its default preset, a preset's, or a chain preset's in order.
std::vector<Effect> draggedEffects(Session& session, const juce::String& description);
juce::String presetDrag(const PresetEntry& preset);
// Moves the effect of a dragged card ("fxcard:ID") onto the owner's chain at `gap` (none: the end), or with Option
// held adds a copy there.
void dropEffectCard(Session& session, const juce::String& description, const ChainOwner& owner,
                    std::optional<size_t> gap);
// When `cell` is one of several selected clips, drops an effect type ("effect:TYPE") or a card ("fxcard:ID") onto
// every selected clip as one undo step: onto the cell at `gap` as a single drop does, onto the others at the end (a
// card as a copy), and returns true.
bool dropOnSelection(Session& session, const juce::String& description, Session::ClipCell cell,
                     std::optional<size_t> gap);

} // namespace azl::ui
