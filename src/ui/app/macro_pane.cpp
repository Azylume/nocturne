#include "ui/app/macro_pane.hpp"

#include "document/modulation_edits.hpp"
#include "ui/app/look.hpp"

#include <algorithm>
#include <cmath>

namespace azl::ui {

namespace {

juce::String danglingName(const Modulator& m)
{
    juce::String source = m.source == ModulatorSource::Audio ? utf8(m.feature) : juce::String("LFO");
    source = source.substring(0, 1).toUpperCase() + source.substring(1);
    return source + juce::String::fromUTF8(" \xe2\x86\x92 ") + utf8(m.target.param.param);
}

juce::String chainName(const Session& session, uint64_t layer, std::optional<uint64_t> column)
{
    const Layer* found = layer ? session.layer(layer) : nullptr;
    juce::String name = found ? utf8(found->name) : juce::String("Composition");
    return column ? name + " clip" : name;
}


constexpr int kLine = 22;
constexpr int kKnobsHeight = 64;
constexpr double kSendInterval = 1.0 / 30.0;
const char* const kCombineNames[] = {"Add", "Multiply"};

double seconds()
{
    return juce::Time::getMillisecondCounterHiRes() / 1000.0;
}

const Macro* macroById(const Composition& composition, uint64_t id)
{
    for (const Macro& macro : composition.macros)
        if (macro.id == id)
            return &macro;
    return nullptr;
}

void setSource(Modulator& m, const std::string& source)
{
    m.source = source == "beat" ? ModulatorSource::Lfo : ModulatorSource::Audio;
    if (source != "beat")
        m.feature = source;
}

// An added driver ends where a macro at its value plus it would; a multiplying one scales the macro by its range.
Ends rangeEnds(const ParamInfo& info, const Modulator& m)
{
    return m.combine == Combine::Add ? endsOf(info, m.min, m.max) : Ends{m.min, m.max};
}

void drawKnob(juce::Graphics& g, juce::Rectangle<float> dial, double base, std::optional<double> live, bool exists)
{
    const Palette& p = Look::palette();
    constexpr float start = -2.36f, sweep = 4.71f;
    const float cx = dial.getCentreX(), cy = dial.getCentreY(), r = dial.getWidth() * 0.5f - 2.0f;
    g.setColour(juce::Colour(0xff111111));
    g.fillEllipse(dial.reduced(1.0f));
    juce::Path track;
    track.addCentredArc(cx, cy, r, r, 0, start, start + sweep, true);
    g.setColour(juce::Colour(0xff2e2e2e));
    g.strokePath(track, juce::PathStrokeType(3.0f));
    if (!exists)
        return;
    const double shown = live.value_or(base);
    juce::Path value;
    value.addCentredArc(cx, cy, r, r, 0, start, start + sweep * float(std::clamp(shown, 0.0, 1.0)), true);
    g.setColour(live ? p.accent : p.live);
    g.strokePath(value, juce::PathStrokeType(3.0f));
    const float a = start + sweep * float(std::clamp(shown, 0.0, 1.0));
    g.setColour(p.white);
    g.drawLine(cx, cy, cx + r * std::sin(a), cy - r * std::cos(a), 1.5f);
}

} // namespace

TargetRow::TargetRow(Session& session, uint64_t macro, size_t index)
    : session_(session), macro_(macro), index_(index), from_(session, 0, 100, 1, "%"), to_(session, 0, 100, 1, "%")
{
    auto rangeEnd = [this](double Range::*end) {
        return [this, end](double percent) {
            const MacroTarget* t = target();
            const auto info = t ? paramInfo(session_.composition(), t->param) : std::nullopt;
            if (!info)
                return;
            MacroTarget changed = *t;
            Range range = rangeOf(*info, t->min, t->max);
            range.*end = percent / 100.0;
            std::tie(changed.min, changed.max) = offsetsOf(*info, range);
            send(changed);
        };
    };
    from_.onChange = rangeEnd(&Range::from);
    to_.onChange = rangeEnd(&Range::to);
    remove_.setTooltip("Remove from the macro");
    remove_.onClick = [this] { session_.client().edit(RemoveMacroTarget{macro_, index_}); };
    addAndMakeVisible(from_);
    addAndMakeVisible(to_);
    addAndMakeVisible(remove_);
    refresh();
}

const MacroTarget* TargetRow::target() const
{
    const Macro* macro = macroById(session_.composition(), macro_);
    return macro && index_ < macro->targets.size() ? &macro->targets[index_] : nullptr;
}

void TargetRow::send(const MacroTarget& target)
{
    session_.client().edit(SetMacroTarget{macro_, index_, target});
}

void TargetRow::refresh()
{
    const MacroTarget* t = target();
    const auto info = t ? paramInfo(session_.composition(), t->param) : std::nullopt;
    from_.setEnabled(bool(info));
    to_.setEnabled(bool(info));
    if (info) {
        const juce::String unit = ", as % of its range (" + rangeText(*info) + ")";
        from_.setTooltip("From: where " + utf8(info->label) + " sits with the macro at 0" + unit);
        to_.setTooltip("To: where " + utf8(info->label) + " sits with the macro at 100%" + unit);
        const Range range = rangeOf(*info, t->min, t->max);
        from_.setValue(std::round(range.from * 100.0));
        to_.setValue(std::round(range.to * 100.0));
    }
    repaint();
}

juce::Rectangle<int> TargetRow::curveArea() const
{
    return {getWidth() - 20 - 2 * 46 - 8 - 44, 3, 40, kHeight - 6};
}

void TargetRow::resized()
{
    auto area = getLocalBounds().reduced(0, 2);
    remove_.setBounds(area.removeFromRight(20));
    area.removeFromRight(4);
    to_.setBounds(area.removeFromRight(44));
    area.removeFromRight(2);
    from_.setBounds(area.removeFromRight(44));
}

void TargetRow::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    g.setColour(p.cell);
    g.fillRect(getLocalBounds().withTrimmedBottom(1));
    const MacroTarget* t = target();
    if (!t)
        return;
    const auto area = curveArea();
    g.setColour(p.field);
    g.fillRect(area);
    drawCurve(g, area.toFloat().reduced(2.0f), t->curve, p.live);
    g.setColour(paramInfo(session_.composition(), t->param) ? p.text : p.muted);
    g.setFont(Look::font());
    drawText(g, utf8(paramLabel(session_.composition(), t->param)), getLocalBounds().withRight(area.getX() - 4).reduced(6, 0),
               juce::Justification::centredLeft, true);
}

void TargetRow::mouseUp(const juce::MouseEvent& e)
{
    if (curveArea().contains(e.getPosition()))
        showCurves();
}

juce::String TargetRow::tipAt(juce::Point<int> point)
{
    if (curveArea().contains(point))
        return "How the macro maps onto the parameter: click for curve presets, the editor or to invert the range";
    const MacroTarget* t = target();
    return t ? utf8(paramLabel(session_.composition(), t->param)) : juce::String();
}

void TargetRow::showCurves()
{
    const MacroTarget* t = target();
    if (!t)
        return;
    juce::PopupMenu menu;
    const auto& curves = namedCurves();
    for (size_t i = 0; i < curves.size(); ++i)
        menu.addItem(int(i) + 1, curves[i].name, true, curves[i].curve == t->curve);
    menu.addSeparator();
    menu.addItem(100, "Edit curve...");
    menu.addItem(101, "Invert range");
    juce::Component::SafePointer<TargetRow> self(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this), [self](int result) {
        if (!self)
            return;
        const MacroTarget* t = self->target();
        if (!t)
            return;
        MacroTarget changed = *t;
        const auto& curves = namedCurves();
        if (result >= 1 && result <= int(curves.size())) {
            changed.curve = curves[size_t(result - 1)].curve;
            self->send(changed);
        } else if (result == 101) {
            std::swap(changed.min, changed.max);
            self->send(changed);
        } else if (result == 100) {
            Session& session = self->session_;
            const uint64_t macro = self->macro_;
            const size_t index = self->index_;
            auto editor = std::make_unique<CurveEditor>(session, t->curve, [&session, macro, index](const Curve& curve) {
                const Macro* m = macroById(session.composition(), macro);
                if (!m || index >= m->targets.size())
                    return;
                MacroTarget target = m->targets[index];
                target.curve = curve;
                session.client().edit(SetMacroTarget{macro, index, target});
            });
            juce::CallOutBox::launchAsynchronously(std::move(editor), self->getScreenBounds().withWidth(self->curveArea().getRight()),
                                                   nullptr);
        }
    });
}

ModulatorRow::ModulatorRow(Session& session, uint64_t modulator)
    : session_(session), id_(modulator),
      envelope_(session,
                [this](const auto& change) { send([change](Modulator& m, const ParamInfo&) { change(m); }); }),
      from_(session, 0, 100, 1, "%"), to_(session, 0, 100, 1, "%"),
      lfo_(session, [this] { return this->modulator(); },
           [this](const auto& change) { send([change](Modulator& m, const ParamInfo&) { change(m); }); })
{
    for (int i = 0; i < kSourceCount; ++i)
        source_.addItem(kSourceNames[i], i + 1);
    for (size_t i = 0; i < std::size(kShapes); ++i)
        shape_.addItem(kShapeNames[i], int(i) + 1);
    for (int beats : kBeats)
        beats_.addItem(juce::String(beats) + (beats == 1 ? " beat" : " beats"), beats);
    for (int i = 0; i < 2; ++i)
        combine_.addItem(kCombineNames[i], i + 1);
    source_.setTooltip("What drives the macro: an LFO on the beat, or the kick, snare, hat, bass or the Low, Mid or High band of the audio input");
    shape_.setTooltip("The LFO's wave shape");
    beats_.setTooltip("How many beats one LFO cycle takes");
    combine_.setTooltip("How it joins the macro's other modulators: Add puts its range onto the macro, Multiply scales "
                        "the macro by it");
    from_.setTooltip("From: where it takes the macro at its low end (Multiply: the factor), 0% to 100%");
    to_.setTooltip("To: where it takes the macro at its high end (Multiply: the factor), 0% to 100%");
    remove_.setTooltip("Remove this modulator from the macro");
    source_.onChange = [this] {
        const int i = source_.getSelectedId() - 1;
        if (i >= 0)
            send([i](Modulator& m, const ParamInfo&) { setSource(m, kSources[i]); });
    };
    shape_.onChange = [this] {
        const int i = shape_.getSelectedId() - 1;
        if (i >= 0)
            send([i](Modulator& m, const ParamInfo&) { setLfoShape(m, kShapes[i]); });
    };
    beats_.onChange = [this] {
        const int beats = beats_.getSelectedId();
        send([beats](Modulator& m, const ParamInfo&) { m.beats = beats; });
    };
    combine_.onChange = [this] {
        const int i = combine_.getSelectedId() - 1;
        if (i >= 0)
            send([i](Modulator& m, const ParamInfo&) { m.combine = Combine(i); });
    };
    from_.onChange = [this](double percent) { setEnd(0, percent / 100.0); };
    to_.onChange = [this](double percent) { setEnd(1, percent / 100.0); };
    remove_.onClick = [this] { session_.client().edit(RemoveModulator{id_}); };
    for (juce::Component* c : std::initializer_list<juce::Component*>{&source_, &shape_, &beats_, &combine_, &envelope_,
                                                                      &from_, &to_, &remove_, &lfo_})
        addAndMakeVisible(c);
    refresh();
}

const Modulator* ModulatorRow::modulator() const
{
    for (const Modulator& m : session_.composition().modulators)
        if (m.id == id_)
            return &m;
    return nullptr;
}

std::optional<ParamInfo> ModulatorRow::macroInfo() const
{
    const Modulator* m = modulator();
    const Macro* macro = m ? macroById(session_.composition(), m->target.macro) : nullptr;
    if (!macro)
        return std::nullopt;
    ParamInfo info;
    info.label = macro->name;
    info.value = macro->value;
    return info;
}

void ModulatorRow::send(const std::function<void(Modulator&, const ParamInfo&)>& change)
{
    const Modulator* existing = modulator();
    const auto info = macroInfo();
    if (!existing || !info)
        return;
    Modulator m = *existing;
    change(m, *info);
    if (!(m == *existing))
        session_.client().edit(SetModulator{m});
}

void ModulatorRow::setEnd(int end, double value)
{
    send([end, value](Modulator& m, const ParamInfo& info) {
        double& offset = end ? m.max : m.min;
        offset = m.combine == Combine::Add ? offsetTo(info, value, offset) : info.fit(value);
    });
}

void ModulatorRow::refresh()
{
    const Modulator* m = modulator();
    const auto info = macroInfo();
    if (!m || !info)
        return;
    const std::string source = animationSource(*m);
    for (int i = 0; i < kSourceCount; ++i)
        if (source == kSources[i])
            source_.setSelectedId(i + 1, juce::dontSendNotification);
    shape_.setSelectedId(0, juce::dontSendNotification);
    for (size_t i = 0; i < std::size(kShapes); ++i)
        if (m->shape == kShapes[i])
            shape_.setSelectedId(int(i) + 1, juce::dontSendNotification);
    beats_.setSelectedId(int(m->beats), juce::dontSendNotification);
    combine_.setSelectedId(int(m->combine) + 1, juce::dontSendNotification);
    envelope_.refresh(*m);
    const Ends ends = rangeEnds(*info, *m);
    from_.setValue(std::round(info->fit(ends.from) * 100.0));
    to_.setValue(std::round(info->fit(ends.to) * 100.0));
    const bool beat = m->source == ModulatorSource::Lfo;
    if (beat != beat_) {
        beat_ = beat;
        resized();
    }
    repaint();
}

int ModulatorRow::preferredHeight() const
{
    const Modulator* m = modulator();
    return kHeight + (m && m->source == ModulatorSource::Lfo ? LfoEditor::kHeight + 2 : 0);
}

juce::Rectangle<int> ModulatorRow::barArea() const
{
    return {6, kHeight / 2 + 7, std::max(0, from_.getX() - 12), 8};
}

void ModulatorRow::resized()
{
    auto area = getLocalBounds().reduced(4, 2);
    auto first = area.removeFromTop(18);
    remove_.setBounds(first.removeFromRight(20));
    first.removeFromRight(4);
    combine_.setBounds(first.removeFromRight(80));
    first.removeFromRight(4);
    source_.setBounds(first.removeFromLeft(96));
    first.removeFromLeft(4);
    shape_.setVisible(beat_);
    beats_.setVisible(beat_);
    envelope_.setVisible(!beat_);
    envelope_.setBounds(first);
    shape_.setBounds(first.removeFromLeft(first.getWidth() / 2 - 2));
    first.removeFromLeft(4);
    beats_.setBounds(first);
    area.removeFromTop(4);
    auto second = area.removeFromTop(18);
    to_.setBounds(second.removeFromRight(44));
    second.removeFromRight(16);
    from_.setBounds(second.removeFromRight(44));
    area.removeFromTop(4);
    lfo_.setVisible(beat_);
    lfo_.setBounds(area.removeFromTop(LfoEditor::kHeight).withTrimmedLeft(2));
}

void ModulatorRow::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    g.setColour(p.cell);
    g.fillRect(getLocalBounds().withTrimmedBottom(1));
    g.setColour(p.accent);
    g.fillRect(getLocalBounds().withTrimmedBottom(1).withWidth(2));
    const Modulator* m = modulator();
    const auto info = macroInfo();
    if (!m || !info)
        return;
    const auto& live = session_.modulation().macros;
    const auto it = live.find(m->target.macro);
    drawRangeBar(g, barArea(), *info, rangeEnds(*info, *m), it == live.end() ? std::nullopt : std::optional(it->second));
    g.setColour(p.muted);
    g.setFont(Look::smallFont());
    drawText(g, juce::String::fromUTF8("\xe2\x86\x92"), to_.getBounds().translated(-16, 0).withWidth(16),
               juce::Justification::centred);
}

void ModulatorRow::mouseDown(const juce::MouseEvent& e)
{
    const Modulator* m = modulator();
    const auto info = macroInfo();
    dragEnd_.reset();
    if (!m || !info || !barArea().expanded(0, 4).contains(e.getPosition()))
        return;
    dragEnd_ = rangeHandleAt(barArea(), *info, rangeEnds(*info, *m), e.x);
    if (dragEnd_)
        session_.client().beginGesture();
}

void ModulatorRow::mouseDrag(const juce::MouseEvent& e)
{
    if (const auto info = macroInfo(); dragEnd_ && info)
        setEnd(*dragEnd_, valueAtX(barArea(), *info, e.x));
}

void ModulatorRow::mouseUp(const juce::MouseEvent&)
{
    if (dragEnd_)
        session_.client().endGesture();
    dragEnd_.reset();
}

juce::String ModulatorRow::tipAt(juce::Point<int> point)
{
    const Modulator* m = modulator();
    const auto info = macroInfo();
    if (!m || !info || !barArea().expanded(0, 4).contains(point))
        return {};
    if (auto end = rangeHandleAt(barArea(), *info, rangeEnds(*info, *m), point.x))
        return juce::String(*end ? "To" : "From") + ": drag this end of the range it sweeps";
    return "The range it sweeps the macro over, 0% to 100%, with the macro's live value; drag an end";
}

SetPreview::SetPreview(Session& session, MacroSetPreview preview, std::function<void()> close)
    : session_(session), preview_(std::move(preview)), close_(std::move(close))
{
    const Palette& p = Look::palette();
    setTooltip("What the Macro Set brings: untick what to leave out, then Apply");
    for (const MacroSetItem& item : preview_.items) {
        const bool fresh = item.status == "new";
        auto tick = std::make_unique<juce::ToggleButton>(utf8(item.label) + (fresh ? "  (new, no targets)" : ""));
        tick->setToggleState(true, juce::dontSendNotification);
        tick->setColour(juce::ToggleButton::textColourId, fresh ? p.accent : p.text);
        if (fresh)
            tick->setTooltip("Not in this show: created without targets; untick to leave it out");
        else if (item.kind == "macro")
            tick->setTooltip("Takes this macro slot's name, value and modulators; its targets stay. Untick to leave the slot as it is");
        else
            tick->setTooltip("Untick to leave it out");
        tick->onClick = [this] { follow(); };
        list_.addAndMakeVisible(*tick);
        ticks_.push_back(std::move(tick));
    }
    for (const std::string& note : preview_.notes) {
        auto label = std::make_unique<juce::Label>(juce::String(), utf8(note));
        label->setFont(Look::font());
        label->setColour(juce::Label::textColourId, p.muted);
        label->setBorderSize({});
        list_.addAndMakeVisible(*label);
        notes_.push_back(std::move(label));
    }
    apply_.setTooltip("Load the ticked items as one undo step: names, values and modulators; every macro keeps its targets");
    cancel_.setTooltip("Close without loading anything");
    apply_.onClick = [this] {
        std::vector<std::string> drop;
        for (size_t i = 0; i < ticks_.size(); ++i)
            if (!ticks_[i]->getToggleState())
                drop.push_back(preview_.items[i].key);
        session_.client().loadMacroSet(preview_.name, drop);
        close_();
    };
    cancel_.onClick = [this] { close_(); };
    view_.setViewedComponent(&list_, false);
    view_.setScrollBarsShown(true, false);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&view_, &apply_, &cancel_})
        addAndMakeVisible(c);
    follow();
}

juce::ToggleButton* SetPreview::item(const std::string& key)
{
    for (size_t i = 0; i < ticks_.size(); ++i)
        if (preview_.items[i].key == key)
            return ticks_[i].get();
    return nullptr;
}

void SetPreview::follow()
{
    for (size_t i = 0; i < ticks_.size(); ++i) {
        const MacroSetItem& item = preview_.items[i];
        const juce::ToggleButton* parent = item.parent.empty() ? nullptr : this->item(item.parent);
        ticks_[i]->setEnabled(!parent || parent->getToggleState());
    }
}

void SetPreview::resized()
{
    auto area = getLocalBounds().withTrimmedTop(2 * kLine);
    auto buttons = area.removeFromBottom(kLine).reduced(0, 1);
    apply_.setBounds(buttons.removeFromRight(70));
    buttons.removeFromRight(4);
    cancel_.setBounds(buttons.removeFromRight(70));
    area.removeFromBottom(4);
    view_.setBounds(area);
    const int width = view_.getMaximumVisibleWidth();
    int y = 0;
    for (size_t i = 0; i < ticks_.size(); ++i) {
        const int indent = preview_.items[i].parent.empty() ? 0 : 18;
        ticks_[i]->setBounds(indent, y, width - indent, 18);
        y += 18;
    }
    for (auto& note : notes_) {
        note->setBounds(0, y, width, 18);
        y += 18;
    }
    list_.setSize(width, y);
}

void SetPreview::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    g.fillAll(p.panel);
    auto area = getLocalBounds().reduced(4, 0);
    g.setColour(p.white);
    g.setFont(Look::font(true));
    drawText(g, "Load \"" + utf8(preview_.name) + "\"", area.removeFromTop(kLine), juce::Justification::centredLeft, true);
    const auto plural = [](size_t n, const char* one, const char* many) { return juce::String(int(n)) + (n == 1 ? one : many); };
    g.setColour(p.muted);
    g.setFont(Look::font());
    drawText(g, "Replaces " + plural(preview_.macros, " macro", " macros") + " and " +
                   plural(preview_.modulators, " modulator", " modulators") +
                   " driving them; targets stay",
               area.removeFromTop(kLine), juce::Justification::centredLeft, true);
}

MacrosView::MacrosView(Session& session) : session_(session)
{
    addModulator_.onClick = [this] { addModulator(); };
    load_.onClick = [this] {
        if (sets_.getSelectedId() <= 0)
            return;
        previewing_ = sets_.getText().toStdString();
        session_.client().previewMacroSet(previewing_);
    };
    save_.onClick = [this] {
        askName("Save Macro Set", "The macros' names, values and modulators, kept on the engine machine", "My set",
                [this](const juce::String& name) { session_.client().saveMacroSet(name.toStdString()); });
    };
    assign_.setClickingTogglesState(false);
    assign_.setTooltip("Then click parameter bars to add them to this macro; Esc stops");
    sets_.setTooltip("Macro Sets saved on the engine machine");
    load_.setTooltip("Show what the chosen Macro Set brings, to pick what to load");
    save_.setTooltip("Save the macros' names, values and modulators as a Macro Set; targets stay with the show");
    rename_.setTooltip("Rename the selected macro");
    addModulator_.setTooltip("Add another modulator driving the selected macro; its drivers add or multiply together");
    assign_.onClick = [this] { session_.setLearning(!session_.learning()); };
    rename_.onClick = [this] { rename(session_.selectedMacro()); };
    targetsView_.setViewedComponent(&targets_, false);
    targetsView_.setScrollBarsShown(true, false);
    targets_.addAndMakeVisible(addModulator_);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&sets_, &load_, &save_, &assign_, &rename_,
                                                                      &targetsView_})
        addAndMakeVisible(c);
    session_.addView(this);
    refresh();
}

MacrosView::~MacrosView()
{
    session_.removeView(this);
}

void MacrosView::sessionChanged(int changes)
{
    if (changes & (Session::Document | Session::Panels | Session::MacroSets | Session::Connection))
        refresh();
    if (const auto preview = session_.client().macroSetPreview(); preview && preview->name == previewing_) {
        previewing_.clear();
        preview_ = std::make_unique<SetPreview>(session_, *preview, [this] {
            juce::MessageManager::callAsync([self = juce::Component::SafePointer<MacrosView>(this)] {
                if (self)
                    self->preview_.reset();
            });
        });
        addAndMakeVisible(*preview_);
        resized();
    }
    else if (changes & Session::Modulation) {
        repaint(knobs_);
        for (auto& row : rows_)
            row->repaint();
    }
    if (session_.client().modulation().dangling.size() != danglingCount_)
        resized();
}

void MacrosView::refresh()
{
    const Composition& composition = session_.composition();
    const juce::String chosen = sets_.getText();
    sets_.clear(juce::dontSendNotification);
    int id = 1;
    for (const std::string& name : session_.client().macroSets())
        sets_.addItem(utf8(name), id++);
    if (chosen.isNotEmpty())
        sets_.setText(chosen, juce::dontSendNotification);
    else if (sets_.getNumItems() > 0)
        sets_.setSelectedItemIndex(0, juce::dontSendNotification);

    const Macro* macro = macroSlots(composition)[size_t(session_.selectedMacro())];
    assign_.setToggleState(session_.learning(), juce::dontSendNotification);
    assign_.setButtonText(session_.learning() ? "Stop assigning" : "Assign");
    addModulator_.setEnabled(composition.modulators.size() < kMaxModulators);
    std::vector<uint64_t> ids;
    for (const Modulator& m : composition.modulators)
        if (macro && m.target.macro == macro->id)
            ids.push_back(m.id);
    if (ids != modIds_) {
        modIds_ = ids;
        modRows_.clear();
        for (uint64_t id : ids) {
            modRows_.push_back(std::make_unique<ModulatorRow>(session_, id));
            targets_.addAndMakeVisible(*modRows_.back());
        }
        resized();
    }
    for (auto& row : modRows_)
        row->refresh();
    if (std::any_of(modRows_.begin(), modRows_.end(), [](const auto& row) { return row->getHeight() != row->preferredHeight(); }))
        resized();

    const size_t count = macro ? macro->targets.size() : 0;
    if (!macro || macro->id != rowsMacro_ || rows_.size() != count) {
        rows_.clear();
        rowsMacro_ = macro ? macro->id : 0;
        for (size_t i = 0; i < count; ++i) {
            rows_.push_back(std::make_unique<TargetRow>(session_, macro->id, i));
            targets_.addAndMakeVisible(*rows_.back());
        }
        resized();
    }
    for (auto& row : rows_)
        row->refresh();
    repaint();
}

Menu MacrosView::danglingMenu() const
{
    const Composition& composition = session_.composition();
    const auto& dangling = session_.client().modulation().dangling;
    Menu menu;
    for (const Modulator& m : composition.modulators) {
        if (!std::ranges::count(dangling, m.id))
            continue;
        Menu sub{{.label = "Remove", .run = [this, id = m.id] { session_.client().edit(RemoveModulator{id}); }}};
        forEachChain(composition, [&](const std::vector<Effect>& chain, uint64_t layer, std::optional<uint64_t> column) {
            for (const Effect& effect : chain) {
                const EffectDef* def = findEffect(effect.type);
                if (!def || std::ranges::none_of(def->params, [&](const ParamDef& d) { return d.name == m.target.param.param; }))
                    continue;
                Modulator moved = m;
                moved.target.param = {0, effect.id, m.target.param.param};
                sub.push_back({.label = "Reconnect to " + chainName(session_, layer, column) + " / " + utf8(effect.type),
                               .run = [this, moved] { session_.client().edit(SetModulator{moved}); }});
            }
        });
        menu.push_back({.label = danglingName(m), .sub = std::move(sub)});
    }
    return menu;
}

juce::Rectangle<int> MacrosView::knobArea(int slot) const
{
    const int width = knobs_.getWidth() / kMacroSlots;
    return {knobs_.getX() + slot * width, knobs_.getY(), width, knobs_.getHeight()};
}

void MacrosView::resized()
{
    auto area = getLocalBounds();
    auto sets = area.removeFromTop(kLine).reduced(0, 1);
    sets.removeFromLeft(30);
    save_.setBounds(sets.removeFromRight(70));
    sets.removeFromRight(2);
    load_.setBounds(sets.removeFromRight(44));
    sets.removeFromRight(2);
    sets_.setBounds(sets);
    danglingCount_ = session_.client().modulation().dangling.size();
    dangling_ = danglingCount_ ? area.removeFromBottom(kLine) : juce::Rectangle<int>();
    area.removeFromTop(4);
    knobs_ = area.removeFromTop(kKnobsHeight);
    area.removeFromTop(4);
    editorHeader_ = area.removeFromTop(kLine);
    auto header = editorHeader_.reduced(0, 1);
    rename_.setBounds(header.removeFromRight(60));
    header.removeFromRight(2);
    assign_.setBounds(header.removeFromRight(100));
    area.removeFromTop(2);
    targetsView_.setBounds(area);
    const int width = targetsView_.getMaximumVisibleWidth();
    int y = 0;
    for (auto& row : modRows_) {
        row->setBounds(0, y, width, row->preferredHeight());
        y += row->preferredHeight();
    }
    addModulator_.setBounds(0, y + 2, 110, kLine - 4);
    y += kLine + 6;
    targetsTop_ = y;
    for (auto& row : rows_) {
        row->setBounds(0, y, width, TargetRow::kHeight);
        y += TargetRow::kHeight;
    }
    targets_.setSize(width, rows_.empty() ? y + 3 * kLine : y);
    if (preview_)
        preview_->setBounds(getLocalBounds().withTrimmedTop(kLine));
}

void MacrosView::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    const Composition& composition = session_.composition();
    const auto slots = macroSlots(composition);
    const ModulationState state = session_.client().modulation();
    g.setFont(Look::font());
    g.setColour(p.muted);
    drawText(g, "Set", sets_.getBounds().withX(0).withWidth(28), juce::Justification::centredLeft);
    for (int i = 0; i < kMacroSlots; ++i) {
        const Macro* macro = slots[size_t(i)];
        auto area = knobArea(i);
        if (i == session_.selectedMacro()) {
            g.setColour(p.cell);
            g.fillRect(area.reduced(1, 0));
        }
        auto dial = area.removeFromTop(38).withSizeKeepingCentre(34, 34).toFloat();
        std::optional<double> live;
        if (macro && macroModulator(composition, macro->id))
            if (auto it = state.macros.find(macro->id); it != state.macros.end())
                live = it->second;
        const double base = pressed_ == i && dragValue_ ? *dragValue_ : macro ? macro->value : 0.0;
        drawKnob(g, dial, base, pressed_ == i && dragValue_ ? std::nullopt : live, macro || (pressed_ == i && dragValue_));
        g.setFont(Look::smallFont());
        g.setColour(macro ? p.text : p.muted);
        drawText(g, macro ? utf8(macro->name) : "Macro " + juce::String(i + 1), area.removeFromTop(13).reduced(1, 0),
                   juce::Justification::centred, true);
        g.setColour(p.white);
        drawText(g, juce::String(juce::roundToInt(live.value_or(base) * 100)), area.removeFromTop(12),
                   juce::Justification::centred);
    }
    const Macro* macro = slots[size_t(session_.selectedMacro())];
    auto header = editorHeader_.withRight(assign_.getX() - 4);
    g.setFont(Look::font(true));
    g.setColour(p.white);
    const juce::String name = macro ? utf8(macro->name) : "Macro " + juce::String(session_.selectedMacro() + 1);
    drawText(g, name, header, juce::Justification::centredLeft, true);
    const int nameWidth = juce::GlyphArrangement::getStringWidthInt(Look::font(true), name) + 10;
    g.setFont(Look::font());
    g.setColour(p.muted);
    const size_t count = macro ? macro->targets.size() : 0;
    const size_t drivers = modRows_.size();
    drawText(g, juce::String(int(drivers)) + (drivers == 1 ? " modulator, " : " modulators, ") + juce::String(int(count)) +
                   (count == 1 ? " target" : " targets"),
               header.withTrimmedLeft(nameWidth), juce::Justification::centredLeft, true);
    if (!state.dangling.empty()) {
        juce::StringArray names;
        for (const Modulator& m : composition.modulators)
            if (std::ranges::count(state.dangling, m.id))
                names.add(danglingName(m));
        g.setColour(p.fault);
        drawText(g, names.joinIntoString(", ") + (names.size() == 1 ? ": its effect was removed, so it does nothing."
                                                                      : ": their effects were removed, so they do nothing.") +
                       " Click to remove or reconnect.",
                   dangling_, juce::Justification::centredLeft, true);
        g.setColour(p.muted);
    }
    if (count == 0)
        g.drawFittedText("No targets. Click Assign, then click parameter bars; or right-click a parameter and pick "
                         "Add to macro.",
                         targetsView_.getBounds().withTrimmedTop(targetsTop_ - targetsView_.getViewPositionY()).reduced(4, 0),
                         juce::Justification::topLeft, 3);
}

int MacrosView::slotAt(juce::Point<int> point) const
{
    for (int i = 0; i < kMacroSlots; ++i)
        if (knobArea(i).contains(point))
            return i;
    return -1;
}

juce::String MacrosView::tipAt(juce::Point<int> point)
{
    if (slotAt(point) >= 0)
        return "Macro value, 0 to 100%: drag up or down; click selects it, double-click renames";
    return {};
}

uint64_t MacrosView::macroFor(int slot)
{
    auto [edits, id] = ensureMacro(session_.composition(), slot);
    for (const Edit& edit : edits)
        session_.client().edit(edit);
    return id;
}

void MacrosView::mouseDown(const juce::MouseEvent& e)
{
    if (dangling_.contains(e.getPosition())) {
        pressed_ = -1;
        showMenu(*this, danglingMenu());
        return;
    }
    pressed_ = slotAt(e.getPosition());
    dragging_ = false;
    dragValue_.reset();
    if (pressed_ < 0)
        return;
    const Macro* macro = macroSlots(session_.composition())[size_t(pressed_)];
    dragStart_ = macro ? macro->value : 0.0;
}

void MacrosView::mouseDrag(const juce::MouseEvent& e)
{
    if (pressed_ < 0 || (!dragging_ && std::abs(e.getDistanceFromDragStartY()) < 3))
        return;
    if (!dragging_) {
        dragging_ = true;
        session_.client().beginGesture();
        dragMacro_ = macroFor(pressed_);
        lastSend_ = 0.0;
    }
    dragValue_ = std::clamp(dragStart_ - e.getDistanceFromDragStartY() / 150.0, 0.0, 1.0);
    if (seconds() - lastSend_ >= kSendInterval) {
        session_.client().edit(SetMacroValue{dragMacro_, *dragValue_});
        lastSend_ = seconds();
    }
    repaint();
}

void MacrosView::mouseUp(const juce::MouseEvent&)
{
    if (dragging_ && dragValue_) {
        session_.client().edit(SetMacroValue{dragMacro_, *dragValue_});
        session_.client().endGesture();
    } else if (pressed_ >= 0) {
        session_.selectMacro(pressed_);
    }
    pressed_ = -1;
    dragging_ = false;
    dragValue_.reset();
    repaint();
}

void MacrosView::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (const int slot = slotAt(e.getPosition()); slot >= 0)
        rename(slot);
}

void MacrosView::rename(int slot)
{
    const Macro* macro = macroSlots(session_.composition())[size_t(slot)];
    const juce::String name = macro ? utf8(macro->name) : "Macro " + juce::String(slot + 1);
    const auto area = slot == session_.selectedMacro() ? editorHeader_.withWidth(160)
                                                         : knobArea(slot).withTop(knobArea(slot).getY() + 38).withHeight(18);
    editInline(editor_, *this, area, name, [this, slot](const juce::String& text) {
        session_.client().beginGesture();
        const uint64_t id = macroFor(slot);
        session_.client().edit(Rename{"macro", id, text.toStdString()});
        session_.client().endGesture();
    });
}


// A new driver takes the first source not yet driving the macro, kick first.
void MacrosView::addModulator()
{
    const Composition& composition = session_.composition();
    const int slot = session_.selectedMacro();
    const Macro* macro = macroSlots(composition)[size_t(slot)];
    const char* const order[] = {"kick", "snare", "hat", "bass", "lowBand", "midBand", "highBand", "beat"};
    std::string source = order[0];
    for (const char* candidate : order) {
        if (std::ranges::none_of(composition.modulators, [&](const Modulator& m) {
                return macro && m.target.macro == macro->id && animationSource(m) == candidate;
            })) {
            source = candidate;
            break;
        }
    }
    session_.client().beginGesture();
    Modulator m;
    m.target.macro = macroFor(slot);
    setSource(m, source);
    m.beats = 4.0;
    setEnvelope(m, EnvelopePreset::Meter);
    m.name = "Drive " + (macro ? macro->name : "Macro " + std::to_string(slot + 1));
    session_.client().edit(AddModulator{std::nullopt, m});
    session_.client().endGesture();
}

} // namespace azl::ui
