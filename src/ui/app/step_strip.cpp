#include "ui/app/step_strip.hpp"

#include "effects/effect_registry.hpp"
#include "ui/app/look.hpp"

namespace azl::ui {

namespace {

const int kEvery[] = {1, 2, 4, 8, 16, 32};
const int kHold[] = {0, 1, 2, 4, 8, 16};
const double kTransition[] = {0.0, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0};
constexpr int kChipGap = 2;

const Effect* effectOf(Session& session, uint64_t id)
{
    return effectById(session.composition(), id);
}

juce::String beatsLabel(double beats)
{
    if (beats == 0.0)
        return "Cut";
    if (beats < 1.0)
        return "1/" + juce::String(juce::roundToInt(1.0 / beats)) + " beat";
    return juce::String(beats, 0) + (beats == 1.0 ? " beat" : " beats");
}

struct Pilot {
    Autopilot autopilot;
    double transition = 0.0;
};

// The effect's Autopilot, or with kPaletteEffect the palettes'.
std::optional<Pilot> pilotOf(Session& session, uint64_t id)
{
    const Composition& composition = session.composition();
    if (id == kPaletteEffect)
        return composition.palettes.empty() ? std::nullopt
                                            : std::optional(Pilot{composition.palettePilot, composition.paletteTransition});
    const Effect* effect = effectOf(session, id);
    return effect ? std::optional(Pilot{effect->autopilot, effect->transition}) : std::nullopt;
}

Edit pilotEdit(uint64_t id, std::optional<AutopilotMode> mode, std::optional<int> bars = std::nullopt,
               std::optional<int> hold = std::nullopt, std::optional<double> transition = std::nullopt)
{
    if (id == kPaletteEffect)
        return SetPaletteAutopilot{mode, bars, hold, transition};
    return SetEffectAutopilot{id, mode, bars, hold, transition};
}

} // namespace

PilotBadge::PilotBadge(Session& session, uint64_t effect) : session_(session), effect_(effect)
{
    setTooltip(effect == kPaletteEffect
                   ? "Autopilot through the palettes: click for Off, Next or Random; right-click for how often, the "
                     "hold after a palette you pick and the crossfade"
                   : "Autopilot through this effect's steps: click for Off, Next or Random; right-click for how often, "
                     "the hold after a step you pick and the transition");
}

juce::String PilotBadge::text() const
{
    const auto pilot = pilotOf(session_, effect_);
    if (!pilot || pilot->autopilot.mode == AutopilotMode::Off)
        return "AP";
    return juce::String(pilot->autopilot.mode == AutopilotMode::Next ? "N" : "R") + juce::String(pilot->autopilot.bars);
}

void PilotBadge::cycle()
{
    const auto pilot = pilotOf(session_, effect_);
    if (pilot)
        session_.client().edit(pilotEdit(effect_, AutopilotMode((int(pilot->autopilot.mode) + 1) % 3)));
}

Menu PilotBadge::menu()
{
    const auto pilot = pilotOf(session_, effect_);
    if (!pilot)
        return {};
    Session& session = session_;
    const uint64_t id = effect_;
    Menu every, hold, transition;
    for (const int bars : kEvery)
        every.push_back({.label = juce::String(bars) + (bars == 1 ? " bar" : " bars"),
                         .run = [&session, id, bars] { session.client().edit(pilotEdit(id, std::nullopt, bars)); },
                         .ticked = pilot->autopilot.bars == bars});
    for (const int bars : kHold)
        hold.push_back({.label = bars == 0 ? juce::String("One step") : juce::String(bars) + (bars == 1 ? " bar" : " bars"),
                        .run = [&session, id, bars] {
                            session.client().edit(pilotEdit(id, std::nullopt, std::nullopt, bars));
                        },
                        .ticked = pilot->autopilot.hold == bars});
    for (const double beats : kTransition)
        transition.push_back({.label = beatsLabel(beats),
                              .run = [&session, id, beats] {
                                  session.client().edit(pilotEdit(id, std::nullopt, std::nullopt, std::nullopt, beats));
                              },
                              .ticked = pilot->transition == beats});
    const bool palettes = id == kPaletteEffect;
    return {{.label = "Every",
             .sub = every,
             .tip = palettes ? "How often the Autopilot moves to another palette, on the bar"
                             : "How often the Autopilot moves to another step, on the bar"},
            {.label = "Hold",
             .sub = hold,
             .tip = palettes ? "After you pick a palette, the Autopilot stays on it this long"
                             : "After you pick a step, the Autopilot stays on it this long"},
            {.label = "Transition",
             .sub = transition,
             .tip = palettes ? "How long every linked colour crossfades into the next palette"
                             : "How long a move to a step glides: numbers and colours glide, choices switch halfway"}};
}

void PilotBadge::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    const auto pilot = pilotOf(session_, effect_);
    const bool on = pilot && pilot->autopilot.mode != AutopilotMode::Off;
    g.setColour(on ? p.accent : p.button);
    g.fillRect(getLocalBounds().reduced(0, 3));
    g.setColour(on ? p.liveInk : p.muted);
    g.setFont(Look::smallFont());
    drawText(g, text(), getLocalBounds(), juce::Justification::centred, false);
}

void PilotBadge::mouseUp(const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        showMenu(*this, menu());
    else
        cycle();
}

StepStrip::StepStrip(Session& session, uint64_t effect) : session_(session), effect_(effect) {}

const Effect* StepStrip::effect() const
{
    return effectOf(session_, effect_);
}

int StepStrip::preferredHeight(int width) const
{
    const Effect* e = effect();
    if (!e || !session_.stepsOpen(effect_))
        return kRow;
    return chipBounds(e->steps.size(), width).getBottom() + kChipGap;
}

juce::Rectangle<int> StepStrip::chipBounds(size_t index, int width) const
{
    const int perRow = std::max(1, (width - 2 * kChipGap) / (kChip + kChipGap));
    const int row = int(index) / perRow, column = int(index) % perRow;
    return {kChipGap + column * (kChip + kChipGap), kRow + row * (kChip + kChipGap), kChip, kChip};
}

std::optional<size_t> StepStrip::chipAt(juce::Point<int> point) const
{
    const Effect* e = effect();
    if (!e || !session_.stepsOpen(effect_))
        return std::nullopt;
    for (size_t i = 0; i <= e->steps.size(); ++i)
        if (chipBounds(i, getWidth()).contains(point))
            return i == e->steps.size() ? kAdd : i;
    return std::nullopt;
}

std::pair<std::optional<size_t>, std::optional<size_t>> StepStrip::shown() const
{
    const ClockState clock = session_.client().clock();
    auto it = clock.effects.find(effect_);
    if (it == clock.effects.end())
        return {};
    return {it->second.step, it->second.next};
}

void StepStrip::refreshClock()
{
    if (shown() != painted_)
        repaint();
}

std::map<std::string, double> StepStrip::currentValues() const
{
    const Effect* e = effect();
    const EffectDef* def = e ? findEffect(e->type) : nullptr;
    if (!def)
        return {};
    const ClockState clock = session_.client().clock();
    const auto at = clock.effects.find(effect_);
    const bool onStep = at != clock.effects.end() && at->second.step;
    std::map<std::string, double> values;
    for (const ParamDef& param : def->params) {
        auto doc = e->params.find(param.name);
        double value = doc == e->params.end() ? param.defaultValue : doc->second;
        const bool stepped = onStep && std::any_of(e->steps.begin(), e->steps.end(), [&](const EffectStep& s) {
            return s.params.contains(param.name);
        });
        // The engine publishes single precision, and a value just edited a frame late: the document's value stands
        // unless the steps moved it.
        if (auto driven = session_.drivenValue(ParamRef{0, effect_, param.name});
            driven && stepped && std::abs(*driven - value) > 1e-6)
            value = fitParam(param, *driven);
        values[param.name] = value;
    }
    return values;
}

void StepStrip::setSteps(std::vector<EffectStep> steps)
{
    session_.client().edit(SetEffectSteps{effect_, std::move(steps)});
}

void StepStrip::addCurrent()
{
    const Effect* e = effect();
    if (!e || e->steps.size() >= kMaxEffectSteps)
        return;
    std::vector<EffectStep> steps = e->steps;
    steps.push_back({"Step " + std::to_string(steps.size() + 1), currentValues()});
    setSteps(std::move(steps));
}

Menu StepStrip::addMenu()
{
    const Effect* e = effect();
    const EffectDef* def = e ? findEffect(e->type) : nullptr;
    if (!def)
        return {};
    Menu presets;
    for (const PresetEntry& preset : session_.client().presetEntries()) {
        if (preset.effectType != e->type)
            continue;
        std::map<std::string, double> params;
        for (const auto& [name, value] : preset.params)
            if (std::any_of(def->params.begin(), def->params.end(), [&](const ParamDef& p) { return p.name == name; }))
                params[name] = value;
        presets.push_back({.label = utf8(preset.name) + "  (" + utf8(preset.group) + ")",
                           .run = [this, name = preset.name, params] {
                               const Effect* now = effect();
                               if (!now || now->steps.size() >= kMaxEffectSteps)
                                   return;
                               std::vector<EffectStep> steps = now->steps;
                               steps.push_back({name, params});
                               setSteps(std::move(steps));
                           }});
    }
    const bool room = e->steps.size() < kMaxEffectSteps;
    return {{.label = "Add from current values",
             .run = [this] { addCurrent(); },
             .enabled = room,
             .tip = "A step of the values the effect shows now"},
            {.label = "Add from preset",
             .enabled = room && !presets.empty(),
             .sub = presets,
             .tip = "A step of one of this effect's presets"}};
}

Menu StepStrip::chipMenu(size_t step)
{
    const Effect* e = effect();
    if (!e || step >= e->steps.size())
        return {};
    const std::vector<EffectStep> steps = e->steps;
    auto moved = [this, steps, step](size_t to) {
        std::vector<EffectStep> next = steps;
        const EffectStep chip = next[step];
        next.erase(next.begin() + ptrdiff_t(step));
        next.insert(next.begin() + ptrdiff_t(to), chip);
        setSteps(std::move(next));
    };
    return {{.label = "Update from current values",
             .run = [this, steps, step] {
                 std::vector<EffectStep> next = steps;
                 next[step].params = currentValues();
                 setSteps(std::move(next));
             },
             .tip = "The step takes the values the effect shows now"},
            {.label = "Rename...",
             .run = [this, steps, step] {
                 askName("Rename step", "Name of the step", utf8(steps[step].name), [this, steps, step](const juce::String& n) {
                     std::vector<EffectStep> next = steps;
                     next[step].name = n.toStdString();
                     setSteps(std::move(next));
                 });
             }},
            {.label = "Move left", .run = [moved, step] { moved(step - 1); }, .enabled = step > 0},
            {.label = "Move right", .run = [moved, step] { moved(step + 1); }, .enabled = step + 1 < steps.size()},
            {},
            {.label = "Remove",
             .run = [this, steps, step] {
                 std::vector<EffectStep> next = steps;
                 next.erase(next.begin() + ptrdiff_t(step));
                 setSteps(std::move(next));
             }}};
}

void StepStrip::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    const Effect* e = effect();
    if (!e)
        return;
    painted_ = shown();
    const bool open = session_.stepsOpen(effect_);
    const auto header = getLocalBounds().removeFromTop(kRow);
    drawChevron(g, header.withWidth(kRow), open, p.muted);
    g.setFont(Look::smallFont());
    g.setColour(p.muted);
    juce::String label = "Steps";
    if (!e->steps.empty())
        label << " (" << int(e->steps.size()) << ")";
    if (painted_.first && *painted_.first < e->steps.size())
        label << juce::String::fromUTF8(" \xc2\xb7 ") << int(*painted_.first + 1);
    drawText(g, label, header.withTrimmedLeft(kRow), juce::Justification::centredLeft);
    if (!open)
        return;
    for (size_t i = 0; i <= e->steps.size(); ++i) {
        const auto chip = chipBounds(i, getWidth());
        if (i == e->steps.size()) {
            g.setColour(p.button);
            g.fillRect(chip);
            g.setColour(p.text);
            drawText(g, "+", chip, juce::Justification::centred, false);
            continue;
        }
        const bool on = painted_.first == i;
        g.setColour(on ? p.accent : p.button);
        g.fillRect(chip);
        if (painted_.second == i && !on) {
            g.setColour(p.accent);
            g.drawRect(chip, 1);
        }
        g.setColour(on ? p.liveInk : p.text);
        drawText(g, juce::String(int(i + 1)), chip, juce::Justification::centred, false);
    }
}

void StepStrip::mouseUp(const juce::MouseEvent& e)
{
    if (e.getMouseDownY() < kRow) {
        session_.setStepsOpen(effect_, !session_.stepsOpen(effect_));
        return;
    }
    const std::optional<size_t> chip = chipAt(e.getMouseDownPosition());
    if (!chip)
        return;
    if (*chip == kAdd)
        showMenu(*this, addMenu());
    else if (e.mods.isPopupMenu())
        showMenu(*this, chipMenu(*chip));
    else
        session_.client().triggerStep(effect_, *chip);
}

juce::String StepStrip::tipAt(juce::Point<int> local)
{
    const Effect* e = effect();
    if (local.y < kRow)
        return "Snapshots of this effect's settings that its Autopilot steps through: click to show or hide them";
    const std::optional<size_t> chip = chipAt(local);
    if (!e || !chip)
        return {};
    if (*chip == kAdd)
        return "Add a step of the current values or of a preset";
    return utf8(e->steps[*chip].name) + ": click to go there; right-click to update, rename, move or remove it";
}

} // namespace azl::ui
