#pragma once

#include "ui/app/widgets.hpp"

#include <memory>
#include <vector>

namespace azl::ui {

// One target of a macro: the parameter, its curve (click for presets or the editor) and the range it sweeps.
class TargetRow : public juce::Component, public PointerTips {
public:
    TargetRow(Session& session, uint64_t macro, size_t index);

    void refresh();
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseUp(const juce::MouseEvent&) override;
    juce::String tipAt(juce::Point<int> point) override;
    NumberField& fromField() { return from_; }
    NumberField& toField() { return to_; }

    static constexpr int kHeight = 22;

private:
    const MacroTarget* target() const;
    void send(const MacroTarget& target);
    juce::Rectangle<int> curveArea() const;
    void showCurves();

    Session& session_;
    const uint64_t macro_;
    const size_t index_;
    NumberField from_, to_;
    juce::TextButton remove_{juce::String::fromUTF8("\xc3\x97")};
};

// One modulator driving a macro: its source, a beat LFO's shape and length or an audio source's envelope, how
// it combines with the macro's other drivers and the range it sweeps (fields, and handles on a bar), and remove.
class ModulatorRow : public juce::Component, public PointerTips {
public:
    ModulatorRow(Session& session, uint64_t modulator);

    void refresh();
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    juce::String tipAt(juce::Point<int> point) override;
    juce::Rectangle<int> barArea() const;
    juce::ComboBox& sourceBox() { return source_; }
    juce::ComboBox& shapeBox() { return shape_; }
    juce::ComboBox& beatsBox() { return beats_; }
    juce::ComboBox& combineBox() { return combine_; }
    EnvelopeControls& envelope() { return envelope_; }
    NumberField& fromField() { return from_; }
    NumberField& toField() { return to_; }
    juce::TextButton& removeButton() { return remove_; }
    LfoEditor& lfoEditor() { return lfo_; }
    // Taller by the LFO editor for a beat LFO.
    int preferredHeight() const;

    static constexpr int kHeight = 46;

private:
    const Modulator* modulator() const;
    std::optional<ParamInfo> macroInfo() const;
    void send(const std::function<void(Modulator&, const ParamInfo&)>& change);
    void setEnd(int end, double value);

    Session& session_;
    const uint64_t id_;
    juce::ComboBox source_, shape_, beats_, combine_;
    EnvelopeControls envelope_;
    NumberField from_, to_;
    juce::TextButton remove_{juce::String::fromUTF8("\xc3\x97")};
    LfoEditor lfo_;
    std::optional<int> dragEnd_;
    bool beat_ = true;
};

// What loading a Macro Set brings, before anything changes: its macros by slot and their modulators, each ticked to keep
// it; a macro this show has no slot for says it arrives without targets. Notes below say what the load leaves alone or
// ignores. Apply loads the ticked items as one undo step.
class SetPreview : public juce::Component, public juce::SettableTooltipClient {
public:
    SetPreview(Session& session, MacroSetPreview preview, std::function<void()> close);

    void paint(juce::Graphics&) override;
    void resized() override;
    juce::ToggleButton* item(const std::string& key);
    juce::TextButton& applyButton() { return apply_; }
    juce::TextButton& cancelButton() { return cancel_; }

private:
    void follow();

    Session& session_;
    const MacroSetPreview preview_;
    std::function<void()> close_;
    std::vector<std::unique_ptr<juce::ToggleButton>> ticks_;
    std::vector<std::unique_ptr<juce::Label>> notes_;
    juce::Viewport view_;
    juce::Component list_;
    juce::TextButton apply_{"Apply"}, cancel_{"Cancel"};
};

// The eight composition macros as knobs (drag up or down, click to select, double-click to rename), Macro Sets, and
// the selected macro's editor: Assign (learn mode), every modulator driving it and its targets with curves and ranges.
class MacrosView : public juce::Component, public PointerTips, private Session::View {
public:
    explicit MacrosView(Session& session);
    ~MacrosView() override;

    juce::Rectangle<int> knobArea(int slot) const;
    // For each modulator whose effect is gone: remove it, or reconnect it to an effect with that parameter.
    Menu danglingMenu() const;
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    juce::String tipAt(juce::Point<int> point) override;

    juce::TextButton& loadButton() { return load_; }
    juce::TextButton& assignButton() { return assign_; }
    juce::TextButton& addModulatorButton() { return addModulator_; }
    SetPreview* setPreview() { return preview_.get(); }
    ModulatorRow* modulatorRow(size_t index) { return index < modRows_.size() ? modRows_[index].get() : nullptr; }
    size_t modulatorRows() const { return modRows_.size(); }
    TargetRow* targetRow(size_t index) { return index < rows_.size() ? rows_[index].get() : nullptr; }

private:
    void sessionChanged(int changes) override;
    void refresh();
    int slotAt(juce::Point<int> point) const;
    // The macro in the slot, created (with any missing before it) within the current gesture if needed.
    uint64_t macroFor(int slot);
    void rename(int slot);
    void addModulator();

    Session& session_;
    juce::ComboBox sets_;
    juce::TextButton load_{"Load"}, save_{"Save as..."}, assign_{"Assign"}, rename_{"Rename"},
        addModulator_{"Add modulator"};
    juce::Viewport targetsView_;
    juce::Component targets_;
    std::vector<std::unique_ptr<ModulatorRow>> modRows_;
    std::vector<uint64_t> modIds_;
    std::vector<std::unique_ptr<TargetRow>> rows_;
    uint64_t rowsMacro_ = 0;
    int targetsTop_ = 0;
    std::unique_ptr<SetPreview> preview_;
    std::string previewing_;
    std::unique_ptr<juce::TextEditor> editor_;
    int pressed_ = -1;
    bool dragging_ = false;
    double dragStart_ = 0.0;
    std::optional<double> dragValue_;
    double lastSend_ = 0.0;
    uint64_t dragMacro_ = 0;
    juce::Rectangle<int> knobs_, editorHeader_, dangling_;
    size_t danglingCount_ = 0;
};

} // namespace azl::ui
