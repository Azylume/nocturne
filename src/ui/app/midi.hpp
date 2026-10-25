#pragma once

#include "document/modulation.hpp"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace azl::ui {

class Session;

// A control on a MIDI controller: a CC or a note by number, or the pitch bend, on a channel 1..16.
struct MidiSource {
    enum class Kind { Cc, Note, PitchBend };
    Kind kind = Kind::Cc;
    int channel = 1;
    int number = 0;

    auto operator<=>(const MidiSource&) const = default;
    // "CC 7 ch 1", "Note 36 ch 10", "Pitch bend ch 2".
    juce::String text() const;
    // Its source when the message comes from a control, with its value 0..1 (a note's velocity; 0 for a note off).
    static std::optional<std::pair<MidiSource, double>> of(const juce::MidiMessage& message);
};

// What a control drives. Continuous controls (CC, pitch bend) move a parameter or a macro; a note, or a CC pressed
// past half way, fires a clip, a column, a deck, Tap or Resync. Ids are the show's, so a mapping to something another
// show lacks does nothing.
struct MidiTarget {
    enum class Kind { Param, Macro, Clip, Column, Deck, Tap, Resync };
    Kind kind = Kind::Param;
    ParamRef param;
    // The macro, column or deck; for a clip its layer.
    uint64_t id = 0;
    // A clip's column.
    uint64_t column = 0;

    bool operator==(const MidiTarget&) const = default;
    bool continuous() const { return kind == Kind::Param || kind == Kind::Macro; }
};

struct MidiBinding {
    MidiSource source;
    MidiTarget target;
};

// MIDI control on the UI's machine, where the controller is plugged in: the UI sends the same edits and actions as a
// click. Nothing opens until start(); with no device connected nothing listens. A continuous control picks its target
// up: it moves it only once it reaches the value the target has (or crosses it), so a knob never makes it jump, also
// when something else moved it since. A turn of a knob is one undo step. The mappings and the input are kept per
// machine in the UI's settings.
class MidiControl : private juce::MidiInputCallback, private juce::AsyncUpdater, private juce::Timer {
public:
    explicit MidiControl(Session& session);
    ~MidiControl() override;

    void useSettings(juce::PropertiesFile* settings);
    // Opens the chosen inputs and follows controllers being plugged in and out.
    void start();
    bool started() const { return started_; }

    // "" every input, "off" none, else a device identifier.
    const std::string& input() const { return input_; }
    void setInput(const std::string& input);
    // The inputs open now, by name.
    juce::StringArray openInputs() const;

    // The next control moved maps onto the target (replacing what it or the control had); Esc in the UI cancels.
    void learn(const MidiTarget& target);
    void cancelLearn();
    const std::optional<MidiTarget>& learning() const { return learning_; }

    const std::vector<MidiBinding>& bindings() const { return bindings_; }
    const MidiBinding* bindingFor(const MidiTarget& target) const;
    void forget(const MidiTarget& target);
    void remove(size_t index);
    // "Layer 1 / Opacity", a macro's name, "Deck 1 / Column 3", or "(not in this show)".
    juce::String describe(const MidiTarget& target) const;

    // Called on the message thread when a mapping comes or goes.
    std::function<void()> onChange;

    // A message as if from an input, on the message thread: what every input's messages go through.
    void handle(const juce::MidiMessage& message);

private:
    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;
    void handleAsyncUpdate() override;
    void timerCallback() override;
    void reopen();
    void save();
    void changed();
    void bind(const MidiSource& source, double value);
    void move(const MidiBinding& binding, double value);
    void fire(const MidiTarget& target);
    // The target's value now as 0..1 of its range; none when the show lacks it.
    std::optional<double> current(const MidiTarget& target) const;

    Session& session_;
    juce::PropertiesFile* settings_ = nullptr;
    bool started_ = false;
    std::string input_;
    std::vector<MidiBinding> bindings_;
    std::optional<MidiTarget> learning_;
    std::optional<juce::MidiDeviceListConnection> devices_;
    std::map<juce::String, std::unique_ptr<juce::MidiInput>> inputs_;
    std::mutex queueMutex_;
    std::deque<juce::MidiMessage> queue_;
    // Pick-up per control: whether it moves its target, its last value, and the values it sent lately.
    struct Takeover {
        bool picked = false;
        std::optional<double> last;
        std::deque<double> sent;
    };
    std::map<MidiSource, Takeover> takeover_;
    // The last value of each CC, so a pad sending CC fires once per press.
    std::map<MidiSource, double> pressed_;
    bool gesture_ = false;
};

} // namespace azl::ui
