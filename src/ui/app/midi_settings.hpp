#pragma once

#include "ui/app/session.hpp"

#include <memory>
#include <vector>

namespace azl::ui {

// Settings' MIDI section, for this machine (the controller plugs in where the UI runs): which inputs listen, and the
// mappings, each with what it drives and a Remove button. Mappings are made by MIDI learn on a right-click.
class MidiSettings : public juce::Component {
public:
    explicit MidiSettings(Session& session);
    ~MidiSettings() override;

    // All inputs, Off, then each MIDI input of this machine.
    juce::ComboBox& inputBox() { return input_; }
    int rows() const { return int(rows_.size()); }
    juce::String rowText(int row) const { return rows_[size_t(row)]->text; }
    juce::TextButton& removeButton(int row) { return rows_[size_t(row)]->remove; }
    // The inputs open now, or that none is.
    juce::String inputsText() const;

    void refresh();
    void paint(juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;

private:
    struct Row : juce::Component {
        juce::String text;
        juce::TextButton remove{"Remove"};
        void paint(juce::Graphics&) override;
        void resized() override;
    };

    Session& session_;
    juce::ComboBox input_;
    std::vector<std::string> inputIds_;
    juce::Viewport view_;
    juce::Component list_;
    std::vector<std::unique_ptr<Row>> rows_;
};

} // namespace azl::ui
