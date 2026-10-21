#include "engine/effect_stepper.hpp"

#include "document/palette.hpp"
#include "engine/bars.hpp"
#include "engine/transport.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace azl {

namespace {

double stepLength(const Autopilot& autopilot, bool held, double pace)
{
    return bars::paced(held ? autopilot.hold : autopilot.bars, pace);
}

} // namespace

EffectStepper::EffectStepper(const BeatClock& clock, const CueState& cues, const std::atomic<uint32_t>& seed)
    : clock_(clock), cues_(cues), seed_(seed)
{
}

void EffectStepper::step(const Composition& composition, double beat)
{
    std::set<uint64_t> ids;
    positions_.clear();
    forEachChain(composition, [&](const std::vector<Effect>& chain, uint64_t, std::optional<uint64_t>) {
        for (const Effect& effect : chain) {
            if (effect.steps.empty())
                continue;
            ids.insert(effect.id);
            stepEffect(effect, composition, beat);
        }
    });
    if (!composition.palettes.empty()) {
        palette_.steps.resize(composition.palettes.size());
        palette_.autopilot = composition.palettePilot;
        palette_.playing = composition.palettePlaying.value_or(StepPlay{activePaletteIndex(composition), std::nullopt});
        ids.insert(kPaletteEffect);
        stepEffect(palette_, composition, beat);
    }
    std::erase_if(pilots_, [&](const auto& pilot) { return !ids.contains(pilot.first); });
    std::sort(positions_.begin(), positions_.end(),
              [](const StepPosition& a, const StepPosition& b) { return a.effect < b.effect; });
}

// A trigger moves the effect to its step from the trigger's beat; while the Autopilot runs it moves on from there on
// the bar, the first step after a trigger lasting the hold. Switched on, the Autopilot steps from the bar it is in;
// switched off, the effect stays on its step.
void EffectStepper::stepEffect(const Effect& effect, const Composition& composition, double beat)
{
    auto [it, fresh] = pilots_.try_emplace(effect.id);
    Pilot& pilot = it->second;
    const Autopilot autopilot = cuedAutopilot(effect, cues_.effects);
    const bool on = autopilot.mode != AutopilotMode::Off;
    const std::optional<StepPlay> trigger = triggerOf(effect);
    if (trigger != pilot.trigger) {
        pilot.trigger = trigger;
        if (trigger)
            retrigger(pilot, effect, *trigger, on && !fresh && pilot.running, beat);
    }
    if (pilot.step && *pilot.step >= effect.steps.size())
        pilot.step.reset();
    if (on)
        run(pilot, effect, composition, beat);
    else
        pilot.running = false;
    if (pilot.step)
        positions_.push_back({effect.id, *pilot.step, pilot.moved});
}

void EffectStepper::retrigger(Pilot& pilot, const Effect& effect, const StepPlay& trigger, bool running, double beat)
{
    if (trigger.step >= effect.steps.size())
        return;
    pilot.step = trigger.step;
    pilot.moved = trigger.beat.value_or(beat);
    pilot.stepStart = pilot.moved > beat ? bars::ceil(pilot.moved) : bars::floor(beat);
    pilot.held = running && effect.autopilot.hold > 0;
    if (running)
        pilot.next = pickNext(effect, pilot, cuedAutopilot(effect, cues_.effects).mode);
}

void EffectStepper::run(Pilot& pilot, const Effect& effect, const Composition& composition, double beat)
{
    const Autopilot autopilot = cuedAutopilot(effect, cues_.effects);
    if (!pilot.running) {
        pilot.running = true;
        pilot.stepStart = bars::floor(beat);
        pilot.resyncs = clock_.resyncs();
        pilot.pace = composition.pace;
        pilot.held = false;
        pilot.next = pickNext(effect, pilot, autopilot.mode);
    }
    if (pilot.resyncs != clock_.resyncs()) {
        pilot.resyncs = clock_.resyncs();
        pilot.stepStart = 0.0;
    }
    if (pilot.next >= effect.steps.size())
        pilot.next = pickNext(effect, pilot, autopilot.mode);
    const double length = stepLength(autopilot, pilot.held, composition.pace);
    if (pilot.pace != composition.pace) {
        pilot.stepStart = bars::rescaledStart(pilot.stepStart, stepLength(autopilot, pilot.held, pilot.pace), length,
                                              beat);
        pilot.pace = composition.pace;
    }
    const double end = pilot.stepStart + length;
    if (beat < end - kBeatEpsilon)
        return;
    const double at = std::max(end, bars::floor(beat));
    pilot.step = pilot.next;
    pilot.moved = at;
    pilot.stepStart = at;
    pilot.held = false;
    pilot.next = pickNext(effect, pilot, autopilot.mode);
}

std::optional<StepPlay> EffectStepper::triggerOf(const Effect& effect) const
{
    std::optional<StepPlay> trigger = effect.playing;
    for (const CueState::StepFiring& firing : cues_.steps) {
        const bool later = !trigger || !trigger->beat || firing.beat > *trigger->beat + kBeatEpsilon;
        if (firing.effect == effect.id && later)
            trigger = StepPlay{firing.step, firing.beat};
    }
    return trigger;
}

// The step after the one the effect is on (the first when it is on none), or a random one other than it.
size_t EffectStepper::pickNext(const Effect& effect, const Pilot& pilot, AutopilotMode mode) const
{
    const size_t count = effect.steps.size();
    if (!pilot.step)
        return mode == AutopilotMode::Random ? keyedUniform(mixKey(layerKey(seed_, Choice::Steps, effect.id),
                                                                   uint64_t(std::llround(pilot.stepStart * 16.0))),
                                                            uint32_t(count))
                                             : 0;
    if (count < 2)
        return *pilot.step;
    if (mode != AutopilotMode::Random)
        return (*pilot.step + 1) % count;
    const size_t pick = keyedUniform(mixKey(layerKey(seed_, Choice::Steps, effect.id),
                                            uint64_t(std::llround(pilot.stepStart * 16.0))),
                                     uint32_t(count - 1));
    return pick >= *pilot.step ? pick + 1 : pick;
}

std::vector<EffectStepper::Shown> EffectStepper::shown(const Composition& composition, double beat) const
{
    std::vector<Shown> shown;
    forEachChain(composition, [&](const std::vector<Effect>& chain, uint64_t, std::optional<uint64_t>) {
        for (const Effect& effect : chain) {
            auto it = pilots_.find(effect.id);
            if (it != pilots_.end())
                shown.push_back(shownOf(effect, it->second, beat));
        }
    });
    return shown;
}

std::optional<EffectStepper::Shown> EffectStepper::shownPalette(double beat) const
{
    auto it = pilots_.find(kPaletteEffect);
    if (it == pilots_.end())
        return std::nullopt;
    return shownOf(palette_, it->second, beat);
}

EffectStepper::Shown EffectStepper::shownOf(const Effect& effect, const Pilot& pilot, double beat) const
{
    Shown s{effect.id, pilot.step};
    if (pilot.running) {
        s.next = pilot.next;
        const Autopilot autopilot = cuedAutopilot(effect, cues_.effects);
        if (pilot.held)
            s.held = bars::left(pilot.stepStart + stepLength(autopilot, true, pilot.pace), beat);
    }
    return s;
}

} // namespace azl
