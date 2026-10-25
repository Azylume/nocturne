#include "ui/app/midi_settings.hpp"

#include "ui/app/look.hpp"
#include "ui/app/midi.hpp"

namespace azl::ui {

namespace {

constexpr int kRow = 24;
constexpr int kLabelWidth = 112;
constexpr const char* kHint =
    "Right-click a parameter, a macro knob, a clip, a column header, a deck tab, Tap or Resync and pick MIDI learn, "
    "then move a control. A knob or fader picks its target up where it is, so nothing jumps.";

} // namespace

MidiSettings::MidiSettings(Session& session) : session_(session)
{
    input_.setTooltip("The MIDI inputs of this machine that control the UI");
    input_.onChange = [this] {
        const int id = input_.getSelectedId();
        if (id > 0 && size_t(id) <= inputIds_.size())
            session_.midi().setInput(inputIds_[size_t(id - 1)]);
    };
    addAndMakeVisible(input_);
    view_.setViewedComponent(&list_, false);
    view_.setScrollBarsShown(true, false);
    addAndMakeVisible(view_);
    session_.midi().onChange = [this] { refresh(); };
}

MidiSettings::~MidiSettings()
{
    session_.midi().onChange = nullptr;
}

void MidiSettings::visibilityChanged()
{
    if (isVisible())
        refresh();
}

void MidiSettings::refresh()
{
    MidiControl& midi = session_.midi();
    input_.clear(juce::dontSendNotification);
    inputIds_ = {"", "off"};
    input_.addItem("All MIDI inputs", 1);
    input_.addItem("Off", 2);
    // Listing the devices starts CoreMIDI, so only once the section shows.
    if (isShowing())
        for (const juce::MidiDeviceInfo& device : juce::MidiInput::getAvailableDevices()) {
            inputIds_.push_back(device.identifier.toStdString());
            input_.addItem(device.name, int(inputIds_.size()));
        }
    if (!midi.input().empty() && midi.input() != "off" &&
        std::find(inputIds_.begin(), inputIds_.end(), midi.input()) == inputIds_.end()) {
        inputIds_.push_back(midi.input());
        input_.addItem("(not connected)", int(inputIds_.size()));
    }
    const auto at = std::find(inputIds_.begin(), inputIds_.end(), midi.input());
    input_.setSelectedId(int(at - inputIds_.begin()) + 1, juce::dontSendNotification);

    rows_.clear();
    for (size_t i = 0; i < midi.bindings().size(); ++i) {
        const MidiBinding& b = midi.bindings()[i];
        auto row = std::make_unique<Row>();
        row->text = b.source.text() + juce::String::fromUTF8(" \xe2\x86\x92 ") + midi.describe(b.target);
        row->remove.setTooltip("Remove this mapping");
        // After the click returns: removing rebuilds the rows, this button with them.
        row->remove.onClick = [this, i] {
            juce::MessageManager::callAsync([self = juce::Component::SafePointer(this), i] {
                if (self)
                    self->session_.midi().remove(i);
            });
        };
        list_.addAndMakeVisible(*row);
        rows_.push_back(std::move(row));
    }
    resized();
    repaint();
}

juce::String MidiSettings::inputsText() const
{
    const MidiControl& midi = session_.midi();
    if (!midi.started())
        return "MIDI is off in this run";
    const juce::StringArray open = midi.openInputs();
    return open.isEmpty() ? juce::String("No MIDI controller connected") : "Listening to " + open.joinIntoString(", ");
}

void MidiSettings::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    g.setFont(Look::font());
    g.setColour(p.muted);
    g.drawText("Input", 0, 0, kLabelWidth, kRow, juce::Justification::centredLeft);
    g.drawText(inputsText(), kLabelWidth, kRow + 4, getWidth() - kLabelWidth, kRow, juce::Justification::centredLeft);
    g.drawText("Mappings", 0, 2 * (kRow + 8), kLabelWidth, kRow, juce::Justification::centredLeft);
    if (rows_.empty())
        g.drawText("None yet", kLabelWidth, 2 * (kRow + 8), getWidth() - kLabelWidth, kRow,
                   juce::Justification::centredLeft);
    g.drawFittedText(kHint, 0, getHeight() - 2 * kRow, getWidth(), 2 * kRow, juce::Justification::topLeft, 2);
}

void MidiSettings::resized()
{
    input_.setBounds(kLabelWidth, 0, 260, kRow);
    const int top = 2 * (kRow + 8);
    view_.setBounds(kLabelWidth, top, getWidth() - kLabelWidth, std::max(0, getHeight() - top - 2 * kRow - 8));
    list_.setSize(view_.getWidth() - view_.getScrollBarThickness(), int(rows_.size()) * (kRow + 4));
    for (size_t i = 0; i < rows_.size(); ++i)
        rows_[i]->setBounds(0, int(i) * (kRow + 4), list_.getWidth(), kRow);
}

void MidiSettings::Row::paint(juce::Graphics& g)
{
    g.setFont(Look::font());
    g.setColour(Look::palette().text);
    g.drawText(text, 0, 0, getWidth() - 80, getHeight(), juce::Justification::centredLeft, true);
}

void MidiSettings::Row::resized()
{
    remove.setBounds(getLocalBounds().removeFromRight(72));
    addAndMakeVisible(remove);
}

} // namespace azl::ui
