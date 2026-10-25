#include "ui/app/midi.hpp"

#include "document/modulation_edits.hpp"
#include "ui/app/params.hpp"
#include "ui/app/session.hpp"

#include <algorithm>
#include <cmath>

namespace azl::ui {

namespace {

// Within this of the target's value a control picks it up.
constexpr double kPickUp = 0.02;
// A turn of a knob ends, as one undo step, this long after its last message.
constexpr int kGestureMs = 400;
// The values a control sent lately, to tell its own edits coming back from the engine's from someone else's.
constexpr size_t kSentKept = 16;

const char* kindName(MidiTarget::Kind kind)
{
    switch (kind) {
    case MidiTarget::Kind::Param: return "param";
    case MidiTarget::Kind::Macro: return "macro";
    case MidiTarget::Kind::Clip: return "clip";
    case MidiTarget::Kind::Column: return "column";
    case MidiTarget::Kind::Deck: return "deck";
    case MidiTarget::Kind::Tap: return "tap";
    case MidiTarget::Kind::Resync: return "resync";
    }
    return "param";
}

std::optional<MidiTarget::Kind> kindNamed(const std::string& name)
{
    for (auto kind : {MidiTarget::Kind::Param, MidiTarget::Kind::Macro, MidiTarget::Kind::Clip,
                      MidiTarget::Kind::Column, MidiTarget::Kind::Deck, MidiTarget::Kind::Tap,
                      MidiTarget::Kind::Resync})
        if (name == kindName(kind))
            return kind;
    return std::nullopt;
}

const char* sourceName(MidiSource::Kind kind)
{
    return kind == MidiSource::Kind::Note ? "note" : kind == MidiSource::Kind::PitchBend ? "pitchBend" : "cc";
}

Message toJson(const MidiBinding& b)
{
    Message target{{"kind", kindName(b.target.kind)}};
    if (b.target.kind == MidiTarget::Kind::Param)
        target.update({{"layer", b.target.param.layer}, {"effect", b.target.param.effect}, {"param", b.target.param.param}});
    if (b.target.id)
        target["id"] = b.target.id;
    if (b.target.column)
        target["column"] = b.target.column;
    return {{"source", {{"kind", sourceName(b.source.kind)}, {"channel", b.source.channel}, {"number", b.source.number}}},
            {"target", target}};
}

std::optional<MidiBinding> fromJson(const Message& j)
{
    if (!j.is_object() || !j.contains("source") || !j.contains("target"))
        return std::nullopt;
    const Message& s = j["source"];
    const Message& t = j["target"];
    if (!s.is_object() || !t.is_object())
        return std::nullopt;
    MidiBinding b;
    const std::string source = s.value("kind", std::string());
    if (source == "note")
        b.source.kind = MidiSource::Kind::Note;
    else if (source == "pitchBend")
        b.source.kind = MidiSource::Kind::PitchBend;
    else if (source != "cc")
        return std::nullopt;
    b.source.channel = std::clamp(s.value("channel", 1), 1, 16);
    b.source.number = std::clamp(s.value("number", 0), 0, 127);
    const auto kind = kindNamed(t.value("kind", std::string()));
    if (!kind)
        return std::nullopt;
    b.target.kind = *kind;
    if (*kind == MidiTarget::Kind::Param)
        b.target.param = {t.value("layer", uint64_t(0)), t.value("effect", uint64_t(0)), t.value("param", std::string())};
    b.target.id = t.value("id", uint64_t(0));
    b.target.column = t.value("column", uint64_t(0));
    return b;
}

const Macro* macroById(const Composition& composition, uint64_t id)
{
    for (const Macro& macro : composition.macros)
        if (macro.id == id)
            return &macro;
    return nullptr;
}

const Column* columnById(const Composition& composition, uint64_t id, const Deck** deck = nullptr)
{
    for (const Deck& d : composition.decks)
        for (const Column& column : d.columns)
            if (column.id == id) {
                if (deck)
                    *deck = &d;
                return &column;
            }
    return nullptr;
}

juce::String utf8(const std::string& text)
{
    return juce::String::fromUTF8(text.c_str());
}

} // namespace

juce::String MidiSource::text() const
{
    const juce::String channelText = " ch " + juce::String(channel);
    switch (kind) {
    case Kind::Note: return "Note " + juce::String(number) + channelText;
    case Kind::PitchBend: return "Pitch bend" + channelText;
    case Kind::Cc: break;
    }
    return "CC " + juce::String(number) + channelText;
}

std::optional<std::pair<MidiSource, double>> MidiSource::of(const juce::MidiMessage& message)
{
    const int channel = message.getChannel();
    if (channel < 1)
        return std::nullopt;
    if (message.isController())
        return std::pair{MidiSource{Kind::Cc, channel, message.getControllerNumber()},
                         message.getControllerValue() / 127.0};
    if (message.isNoteOn())
        return std::pair{MidiSource{Kind::Note, channel, message.getNoteNumber()}, message.getVelocity() / 127.0};
    if (message.isNoteOff())
        return std::pair{MidiSource{Kind::Note, channel, message.getNoteNumber()}, 0.0};
    if (message.isPitchWheel())
        return std::pair{MidiSource{Kind::PitchBend, channel, 0}, message.getPitchWheelValue() / 16383.0};
    return std::nullopt;
}

MidiControl::MidiControl(Session& session) : session_(session) {}

MidiControl::~MidiControl()
{
    devices_.reset();
    for (auto& [id, input] : inputs_)
        input->stop();
    inputs_.clear();
    cancelPendingUpdate();
    stopTimer();
    if (gesture_)
        session_.client().endGesture();
}

void MidiControl::useSettings(juce::PropertiesFile* settings)
{
    settings_ = settings;
    if (!settings_)
        return;
    const Message saved = Message::parse(settings_->getValue("midi", "{}").toStdString(), nullptr, false);
    if (!saved.is_object())
        return;
    input_ = saved.value("input", std::string());
    bindings_.clear();
    if (saved.contains("bindings") && saved["bindings"].is_array())
        for (const Message& j : saved["bindings"])
            if (auto binding = fromJson(j))
                bindings_.push_back(*binding);
    if (started_)
        reopen();
    changed();
}

void MidiControl::save()
{
    if (!settings_)
        return;
    Message list = Message::array();
    for (const MidiBinding& b : bindings_)
        list.push_back(toJson(b));
    settings_->setValue("midi", juce::String(Message{{"input", input_}, {"bindings", list}}.dump()));
}

void MidiControl::changed()
{
    if (onChange)
        onChange();
}

void MidiControl::start()
{
    if (started_)
        return;
    started_ = true;
    devices_ = juce::MidiDeviceListConnection::make([this] { reopen(); });
    reopen();
}

void MidiControl::setInput(const std::string& input)
{
    input_ = input;
    save();
    if (started_)
        reopen();
    changed();
}

void MidiControl::reopen()
{
    std::map<juce::String, juce::String> wanted;
    if (input_ != "off")
        for (const juce::MidiDeviceInfo& device : juce::MidiInput::getAvailableDevices())
            if (input_.empty() || device.identifier.toStdString() == input_)
                wanted[device.identifier] = device.name;
    for (auto it = inputs_.begin(); it != inputs_.end();)
        if (!wanted.contains(it->first)) {
            it->second->stop();
            it = inputs_.erase(it);
        } else {
            ++it;
        }
    for (const auto& [id, name] : wanted)
        if (!inputs_.contains(id))
            if (auto input = juce::MidiInput::openDevice(id, this)) {
                input->start();
                inputs_[id] = std::move(input);
            }
    changed();
}

juce::StringArray MidiControl::openInputs() const
{
    juce::StringArray names;
    for (const auto& [id, input] : inputs_)
        names.add(input->getName());
    return names;
}

void MidiControl::handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& message)
{
    {
        std::lock_guard lock(queueMutex_);
        // A controller sending faster than the UI drains keeps its newest messages.
        if (queue_.size() >= 1024)
            queue_.pop_front();
        queue_.push_back(message);
    }
    triggerAsyncUpdate();
}

void MidiControl::handleAsyncUpdate()
{
    std::deque<juce::MidiMessage> messages;
    {
        std::lock_guard lock(queueMutex_);
        messages.swap(queue_);
    }
    // Of a burst of one continuous control only its last value counts; notes all do, in order.
    std::map<MidiSource, size_t> lastOf;
    for (size_t i = 0; i < messages.size(); ++i)
        if (auto s = MidiSource::of(messages[i]); s && s->first.kind != MidiSource::Kind::Note)
            lastOf[s->first] = i;
    for (size_t i = 0; i < messages.size(); ++i) {
        auto s = MidiSource::of(messages[i]);
        if (s && s->first.kind != MidiSource::Kind::Note && lastOf[s->first] != i)
            continue;
        handle(messages[i]);
    }
}

void MidiControl::handle(const juce::MidiMessage& message)
{
    const auto control = MidiSource::of(message);
    if (!control)
        return;
    const auto& [source, value] = *control;
    const bool press = source.kind == MidiSource::Kind::Note
                           ? value > 0.0
                           : source.kind == MidiSource::Kind::Cc && value >= 0.5 && pressed_[source] < 0.5;
    if (source.kind == MidiSource::Kind::Cc)
        pressed_[source] = value;
    if (learning_) {
        // A knob or a fader for a parameter or a macro; a pad, a key or a button for the rest.
        const bool fits = learning_->continuous() ? source.kind != MidiSource::Kind::Note : press;
        if (fits)
            bind(source, value);
        return;
    }
    for (const MidiBinding& binding : bindings_) {
        if (binding.source != source)
            continue;
        if (binding.target.continuous()) {
            if (source.kind != MidiSource::Kind::Note)
                move(binding, value);
        } else if (press) {
            fire(binding.target);
        }
    }
}

void MidiControl::learn(const MidiTarget& target)
{
    learning_ = target;
    session_.showStatus("MIDI learn: move a control for " + describe(target) + " (Esc cancels)", false);
    changed();
}

void MidiControl::cancelLearn()
{
    if (!learning_)
        return;
    learning_.reset();
    session_.showStatus("MIDI learn cancelled", false);
    changed();
}

void MidiControl::bind(const MidiSource& source, double value)
{
    const MidiTarget target = *learning_;
    learning_.reset();
    std::erase_if(bindings_, [&](const MidiBinding& b) { return b.source == source || b.target == target; });
    bindings_.push_back({source, target});
    // Where the control was when learned counts, so moving on past the target picks it up.
    takeover_[source] = {.last = value};
    save();
    session_.showStatus(source.text() + juce::String::fromUTF8(" \xe2\x86\x92 ") + describe(target), false);
    changed();
}

const MidiBinding* MidiControl::bindingFor(const MidiTarget& target) const
{
    for (const MidiBinding& b : bindings_)
        if (b.target == target)
            return &b;
    return nullptr;
}

void MidiControl::forget(const MidiTarget& target)
{
    if (std::erase_if(bindings_, [&](const MidiBinding& b) { return b.target == target; }) == 0)
        return;
    save();
    changed();
}

void MidiControl::remove(size_t index)
{
    if (index >= bindings_.size())
        return;
    bindings_.erase(bindings_.begin() + std::ptrdiff_t(index));
    save();
    changed();
}

juce::String MidiControl::describe(const MidiTarget& target) const
{
    const Composition& c = session_.composition();
    const juce::String missing = "(not in this show)";
    switch (target.kind) {
    case MidiTarget::Kind::Param:
        return paramInfo(c, target.param) ? utf8(paramLabel(c, target.param)) : missing;
    case MidiTarget::Kind::Macro: {
        const Macro* macro = macroById(c, target.id);
        return macro ? (macro->name.empty() ? juce::String("Macro") : utf8(macro->name)) : missing;
    }
    case MidiTarget::Kind::Clip: {
        const Layer* layer = layerById(c, target.id);
        const Column* column = columnById(c, target.column);
        return layer && column ? utf8(layer->name) + " / " + utf8(column->name) : missing;
    }
    case MidiTarget::Kind::Column: {
        const Deck* deck = nullptr;
        const Column* column = columnById(c, target.id, &deck);
        return column ? utf8(deck->name) + " / " + utf8(column->name) : missing;
    }
    case MidiTarget::Kind::Deck:
        for (const Deck& deck : c.decks)
            if (deck.id == target.id)
                return "Deck " + utf8(deck.name);
        return missing;
    case MidiTarget::Kind::Tap: return "Tap";
    case MidiTarget::Kind::Resync: return "Resync";
    }
    return missing;
}

std::optional<double> MidiControl::current(const MidiTarget& target) const
{
    const Composition& c = session_.composition();
    if (target.kind == MidiTarget::Kind::Macro) {
        const Macro* macro = macroById(c, target.id);
        return macro ? std::optional(macro->value) : std::nullopt;
    }
    const auto info = paramInfo(c, target.param);
    return info ? std::optional(std::clamp(info->fraction(info->value), 0.0, 1.0)) : std::nullopt;
}

void MidiControl::move(const MidiBinding& binding, double value)
{
    const auto now = current(binding.target);
    if (!now)
        return;
    Takeover& t = takeover_[binding.source];
    // Half the pick-up window: a value comes back from the engine rounded to its parameter.
    const auto near = [](double a, double b) { return std::abs(a - b) <= kPickUp / 2; };
    // The target moved, not by this control: from the next value it has to pick it up again.
    if (t.picked && std::none_of(t.sent.begin(), t.sent.end(), [&](double v) { return near(v, *now); }))
        t.picked = false;
    if (!t.picked)
        t.picked = std::abs(value - *now) <= kPickUp || (t.last && (*t.last - *now) * (value - *now) <= 0.0);
    t.last = value;
    if (!t.picked)
        return;
    if (!gesture_ && !session_.client().gesturing()) {
        session_.client().beginGesture();
        gesture_ = true;
    }
    if (binding.target.kind == MidiTarget::Kind::Macro) {
        session_.client().edit(SetMacroValue{binding.target.id, value});
    } else {
        const auto info = paramInfo(session_.composition(), binding.target.param);
        session_.client().edit(setParam(binding.target.param, info->at(value)));
    }
    t.sent.push_back(value);
    t.sent.push_back(*now);
    while (t.sent.size() > kSentKept)
        t.sent.pop_front();
    startTimer(kGestureMs);
}

void MidiControl::timerCallback()
{
    stopTimer();
    if (gesture_) {
        session_.client().endGesture();
        gesture_ = false;
    }
}

void MidiControl::fire(const MidiTarget& target)
{
    EngineClient& client = session_.client();
    switch (target.kind) {
    case MidiTarget::Kind::Clip: return client.triggerClip(target.id, target.column);
    case MidiTarget::Kind::Column: return client.triggerColumn(target.id);
    case MidiTarget::Kind::Deck: return client.selectDeck(target.id);
    case MidiTarget::Kind::Tap: return client.tap();
    case MidiTarget::Kind::Resync: return client.resync();
    case MidiTarget::Kind::Param:
    case MidiTarget::Kind::Macro: break;
    }
}

} // namespace azl::ui
