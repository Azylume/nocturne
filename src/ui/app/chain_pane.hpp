#pragma once

#include "ui/app/bottom_panes.hpp"
#include "ui/app/colour_picker.hpp"
#include "ui/app/preset_tree.hpp"
#include "ui/app/step_strip.hpp"
#include "ui/app/widgets.hpp"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace azl::ui {

// The header a group of an effect's parameters folds under: a chevron, the group's name and how many of its switches
// are on. A click opens or closes it.
class GroupHeader : public juce::Component, public juce::SettableTooltipClient {
public:
    GroupHeader(std::string name, std::function<void()> onClick);

    const std::string& name() const { return name_; }
    void show(bool open, int on, int count);
    void paint(juce::Graphics&) override;
    void mouseUp(const juce::MouseEvent&) override;

    static constexpr int kHeight = 22;

private:
    const std::string name_;
    std::function<void()> onClick_;
    bool open_ = false;
    int on_ = 0;
    int count_ = 0;
};

// One effect of the chain: a header to collapse it (click) or reorder it (drag), its preset menu and remove button,
// and a row per parameter with dry/wet last. Collapsed it is a thin vertical strip.
class EffectCard : public juce::Component, public PointerTips {
public:
    EffectCard(Session& session, ChainOwner owner, uint64_t effect);
    ~EffectCard() override;

    uint64_t effect() const { return effect_; }
    bool dragging() const { return dragging_; }
    void refresh();
    int cardWidth() const;
    ParamRow* row(const std::string& param);
    // The header of the group of parameters with this name; null when the effect has no such group.
    GroupHeader* groupHeader(const std::string& group);
    // The swatch row of the colour group with this key, above the group's sliders.
    SwatchRow* swatchRow(const std::string& group);
    juce::TextButton& removeButton() { return remove_; }
    juce::TextButton& bypassButton() { return bypass_; }
    PilotBadge& pilotBadge() { return pilot_; }
    StepStrip& stepStrip() { return steps_; }
    // The step strip's chip highlight follows the clock.
    void refreshClock();
    juce::Rectangle<int> chevronArea() const;
    // What the header names: the effect type.
    juce::String headerText() const;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    juce::String tipAt(juce::Point<int> point) override;
    // The header's right-click menu.
    Menu cardMenu();
    // The Presets button's menu: Save, Save as, Rename, Delete, Back to default, Set as default, Browse.
    Menu presetMenu();
    void saveAs(const std::string& name);
    void renamePreset(const std::string& name);
    // The preset the effect is set from, with "*" once a parameter has left it; empty for none.
    juce::String presetLabel() const;
    // This effect's presets by group in a box under the Presets button.
    void showPresets();
    PresetPanel* presetPanel() const { return panel_.get(); }
    // Closes the box after this event; a click outside it or an applied preset does the same.
    void closePresets();

    static constexpr int kWidth = 220;
    static constexpr int kCollapsedWidth = 24;
    static constexpr int kHeader = 22;

private:
    Session& session_;
    const ChainOwner owner_;
    const uint64_t effect_;
    std::string type_;
    juce::TextButton presets_{"Presets"}, bypass_{"B"}, remove_{juce::String::fromUTF8("\xc3\x97")};
    bool bypassed_ = false;
    PilotBadge pilot_;
    juce::Viewport body_;
    juce::Component list_;
    StepStrip steps_;
    std::vector<std::unique_ptr<ParamRow>> rows_;
    // Each with the index of the row it sits above.
    std::vector<std::pair<std::unique_ptr<SwatchRow>, size_t>> swatches_;
    // Each row's group, empty for none; each group's header with the index of its first row, and the groups open.
    std::vector<std::string> rowGroups_;
    std::vector<std::pair<std::unique_ptr<GroupHeader>, size_t>> groups_;
    std::set<std::string> openGroups_;
    bool dragging_ = false;
    void buildRows(const EffectDef& def);
    void dropPresets(const juce::CallOutBox* box);
    std::unique_ptr<PresetPanel> panel_;
    std::unique_ptr<juce::CallOutBox> box_;
};

// The owner's own settings, first in the chain: the composition's tempo, a layer's opacity, blend, transition, group
// and Autopilot, or a clip's transport, step and transition, and for a text or generator clip its settings under a
// Source header. The rows scroll when they outgrow the card.
class OwnerCard : public juce::Component {
public:
    explicit OwnerCard(Session& session);

    void setOwner(const ChainOwner& owner);
    void refresh();
    ParamRow* opacityRow() { return opacity_.get(); }
    juce::ComboBox* choice(const juce::String& label) const;
    // Whether the field is the deck in view's own rather than the shared one.
    bool own(const juce::String& label) const;
    NumberField* field(const juce::String& label) const;
    // A text clip's text field and the toggle that unfolds its finer settings; null for other owners.
    juce::TextEditor* textField() const { return text_.get(); }
    juce::TextButton* moreToggle() const { return more_.get(); }
    // Whether the card has a Source section, and a generator clip's row there for the parameter.
    bool hasSource() const;
    ParamRow* sourceRow(const std::string& param) const;
    // The Source section's swatch of a colour group: a text clip's kTextColour or kTextOutline, or a generator's.
    SwatchRow* swatchRow(const std::string& group) const;
    // The header's right-click menu: save the owner's chain as a chain preset, or add one to it.
    Menu chainMenu();
    void saveChain(const std::string& name);
    void paint(juce::Graphics&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void resized() override;

    static constexpr int kWidth = 210;

private:
    // The rows in their order: a label with its controls side by side, or a section header. A fine row shows only
    // while the section is unfolded.
    struct Line {
        juce::String label;
        std::vector<juce::Component*> parts;
        bool header = false;
        bool fine = false;
        juce::Rectangle<int> bounds{};
        // A parameter or swatch row across the card instead.
        ParamRow* row = nullptr;
        SwatchRow* swatch = nullptr;
    };
    struct List : juce::Component {
        std::function<void(juce::Graphics&)> painter;
        void paint(juce::Graphics& g) override { painter(g); }
    };
    void paintRows(juce::Graphics& g);
    void addTextControls(Session::ClipCell cell);
    void addSwatch(ColourRef ref);
    void addGeneratorRows(const Effect& generator);
    void addTransitionRows(const Layer& layer);
    struct Choice {
        juce::String label;
        std::unique_ptr<juce::ComboBox> box;
        std::function<int()> current;
        // The deck in view sets this field for itself.
        bool own = false;
    };
    void addChoice(const juce::String& label, const juce::StringArray& items, std::function<int()> current,
                   std::function<void(int)> pick);
    // A layer field the deck in view may set for itself: `get` reads the deck's own index, `set` writes it or clears
    // it to the shared value. Picking edits the deck's own value while it has one, else the shared one; with several
    // decks a last item switches between the two.
    void addDeckChoice(const juce::String& label, juce::StringArray items, std::function<int()> shared,
                       std::function<void(int)> pickShared, std::function<std::optional<int>(const DeckLayer&)> get,
                       std::function<void(DeckLayer&, std::optional<int>)> set);
    void rebuild();
    // The owner's name and its kind.
    std::pair<juce::String, juce::String> title() const;
    std::string signature() const;

    Session& session_;
    ChainOwner owner_;
    bool built_ = false;
    std::string signature_;
    juce::Viewport body_;
    List list_;
    std::vector<Choice> choices_;
    std::unique_ptr<ParamRow> opacity_;
    std::unique_ptr<NumberField> bpm_;
    struct Field {
        juce::String label;
        std::unique_ptr<NumberField> field;
        std::function<std::optional<double>()> current;
    };
    std::vector<Field> fields_;
    NumberField& addField(const juce::String& label, double min, double max, double step, const juce::String& suffix,
                          std::function<std::optional<double>()> current, std::function<void(double)> set);
    std::vector<Line> lines_;
    std::unique_ptr<juce::TextEditor> text_;
    std::function<std::optional<std::string>()> textCurrent_;
    std::unique_ptr<juce::TextButton> more_;
    bool moreOpen_ = false;
    std::vector<std::unique_ptr<ParamRow>> sourceRows_;
    std::vector<std::unique_ptr<SwatchRow>> swatches_;
    // The rows being added are fine ones.
    bool fine_ = false;
};

// The owner card, the effect cards side by side and a drop zone at the end. Effects dropped from the browser are
// added where they land; a card dragged by its header, from this chain or another, moves there (Option: a copy). A
// generator clip's generator has no card: it shows in the owner card, and the chain shown starts after it.
class ChainContent : public juce::Component, public juce::DragAndDropTarget {
public:
    explicit ChainContent(Session& session);

    void refresh(int height);
    EffectCard* card(uint64_t effect);
    void refreshClock();
    OwnerCard& ownerCard() { return ownerCard_; }
    // The chain position a drop at `x` lands on, counting a hidden generator.
    size_t gapAt(int x) const;
    // Drops an effect type or a card at `gap`, none being the end.
    void drop(const juce::String& description, std::optional<size_t> gap);

    void paint(juce::Graphics&) override;
    bool isInterestedInDragSource(const SourceDetails&) override;
    void itemDragMove(const SourceDetails&) override;
    void itemDragExit(const SourceDetails&) override;
    void itemDropped(const SourceDetails&) override;

private:
    void layout(int height);

    Session& session_;
    ChainOwner owner_;
    OwnerCard ownerCard_;
    std::vector<std::unique_ptr<EffectCard>> cards_;
    // The card being dragged after its chain left the view: hidden, it still carries the drag.
    std::unique_ptr<EffectCard> parked_;
    std::optional<size_t> dropGap_;
    // The chain position of the first card: 1 past a generator.
    size_t first_ = 0;
};

// The effect chain of the composition, the selected layer or the selected clip, scrolling sideways (the vertical wheel
// scrolls it too). A drag hovering a tab switches to it; dropped on a tab it lands at the end of that chain.
class ChainPane : public TabbedPane, public juce::DragAndDropTarget, private Session::View {
public:
    explicit ChainPane(Session& session);
    ~ChainPane() override;

    void resized() override;
    ChainContent& content() { return content_; }

    bool isInterestedInDragSource(const SourceDetails&) override;
    void itemDragMove(const SourceDetails&) override;
    void itemDropped(const SourceDetails&) override;

private:
    void sessionChanged(int changes) override;
    juce::String tabName(int index) const override;
    void paintBody(juce::Graphics&, juce::Rectangle<int>) override {}
    void tabChanged() override;
    void refresh();

    Session& session_;
    juce::Viewport viewport_;
    ChainContent content_;
    std::optional<ParamRef> scrolledTo_;
};

} // namespace azl::ui
