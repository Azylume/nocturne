#include "engine/autopilot_stepper.hpp"

#include "engine/bars.hpp"
#include "engine/transport.hpp"
#include "platform/profile.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace azl {

namespace {

double stepBeats(const Layer& layer, uint64_t column, int bars, double pace)
{
    auto it = layer.clips.find(column);
    return bars::paced(it != layer.clips.end() && it->second.step > 0 ? it->second.step : bars, pace);
}

double columnStep(const Autopilot& autopilot, bool held, double pace)
{
    return bars::paced(held ? autopilot.hold : autopilot.bars, pace);
}

// The deck holding `column`, else the shown one.
const Deck* deckOf(const Composition& composition, uint64_t column)
{
    const Deck* shown = nullptr;
    for (const Deck& deck : composition.decks) {
        if (std::any_of(deck.columns.begin(), deck.columns.end(), [&](const Column& c) { return c.id == column; }))
            return &deck;
        if (deck.id == composition.deck)
            shown = &deck;
    }
    return shown;
}

const Deck* shownDeck(const Composition& composition)
{
    for (const Deck& deck : composition.decks)
        if (deck.id == composition.deck)
            return &deck;
    return nullptr;
}

bool inDeck(const Deck* deck, uint64_t column)
{
    return deck &&
           std::any_of(deck->columns.begin(), deck->columns.end(), [&](const Column& c) { return c.id == column; });
}

} // namespace

uint64_t cueColumn(const Composition& composition, const CueColumn& target)
{
    if (!target.index)
        return target.column;
    for (const Deck& deck : composition.decks)
        if (deck.id == composition.deck)
            return *target.index < deck.columns.size() ? deck.columns[*target.index].id : 0;
    return 0;
}

AutopilotStepper::AutopilotStepper(const BeatClock& clock, const CueState& cues, const std::atomic<uint32_t>& seed,
                                   Playable playable)
    : clock_(clock), cues_(cues), seed_(seed), playable_(std::move(playable))
{
}

void AutopilotStepper::restore(const Composition& composition, const std::vector<ResumePilot>& pilots)
{
    forEachLayer(composition.layers, [&](const Layer& layer) {
        auto saved = std::find_if(pilots.begin(), pilots.end(),
                                  [&](const ResumePilot& p) { return p.layer == layer.id; });
        if (saved == pilots.end() || layer.group || autopilotOf(layer, composition).mode == AutopilotMode::Off ||
            !layer.playing || saved->trigger != layer.playing)
            return;
        Pilot& pilot = pilots_[layer.id];
        pilot = {};
        pilot.trigger = layer.playing;
        pilot.play = saved->play;
        pilot.stepStart = saved->stepStart;
        pilot.next = saved->next;
        pilot.held = saved->held;
        pilot.pace = composition.pace;
        pilot.resyncs = clock_.resyncs();
    });
    auto columns = std::find_if(pilots.begin(), pilots.end(), [](const ResumePilot& p) { return p.layer == 0; });
    if (columns != pilots.end() && composition.columnAutopilot.mode != AutopilotMode::Off)
        restoreColumns(composition, *columns);
}

// The column Autopilot continues its step, and every layer plays the column it fired last again from that beat,
// unless the layer was triggered after it.
void AutopilotStepper::restoreColumns(const Composition& composition, const ResumePilot& saved)
{
    ColumnPilot& pilot = columnPilot_.emplace();
    pilot.column = saved.play.column;
    pilot.next = saved.next;
    pilot.stepStart = saved.stepStart;
    pilot.held = saved.held;
    pilot.pace = composition.pace;
    pilot.resyncs = clock_.resyncs();
    if (saved.play.beat)
        pilot.fired = saved.play;
    forEachLayer(composition.layers, [&](const Layer& layer) {
        if (layer.group)
            return;
        pilot.triggers[layer.id] = layer.playing;
        const bool later = layer.playing && layer.playing->beat && pilot.fired &&
                           *layer.playing->beat > *pilot.fired->beat + kBeatEpsilon;
        if (pilot.fired && !later)
            columnCues_[layer.id] = {layer.playing, *pilot.fired};
    });
}

// Autopilot takes over from the document's last trigger and moves the layer on when a step ends, on the bar; a frame
// that comes late starts the next clip from the last bar line. Switched off, the layer holds what Autopilot played
// until the next trigger, and switched on again it steps from the current bar. A document trigger that replaces a
// pilot is a performer's (Autopilot and column Cues never change the document), so its first step is the hold. A pace
// change rescales the running step from its progress, ending on the bar.
std::optional<Play> AutopilotStepper::steer(const Layer& layer, const Composition& composition, double beat)
{
    AZL_ZONE("steer layer");
    const std::optional<Play> playing = triggered(layer);
    auto it = pilots_.find(layer.id);
    if (columnPilot_) {
        if (it == pilots_.end())
            return playing;
        if (it->second.trigger != playing) {
            pilots_.erase(it);
            return playing;
        }
        it->second.stepStart = bars::floor(beat);
        return it->second.play;
    }
    bool manual = false;
    if (it != pilots_.end() && it->second.trigger != playing) {
        manual = playing == layer.playing;
        pilots_.erase(it);
        it = pilots_.end();
    }
    const Autopilot autopilot = autopilotOf(layer, composition);
    if (autopilot.mode == AutopilotMode::Off || !playing) {
        if (it == pilots_.end())
            return playing;
        it->second.stepStart = bars::floor(beat);
        return it->second.play;
    }
    if (it == pilots_.end()) {
        it = pilots_.try_emplace(layer.id).first;
        Pilot& fresh = it->second;
        fresh.trigger = playing;
        fresh.play = *playing;
        const std::optional<double> at = playing->beat;
        fresh.stepStart = at && *at > beat ? bars::ceil(*at) : bars::floor(beat);
        fresh.resyncs = clock_.resyncs();
        fresh.held = manual && autopilot.hold > 0;
        fresh.pace = composition.pace;
        fresh.next = pickNext(layer, composition, fresh);
    }
    Pilot& pilot = it->second;
    if (pilot.resyncs != clock_.resyncs()) {
        pilot.resyncs = clock_.resyncs();
        pilot.stepStart = 0.0;
    }
    if (!playable_(layer, pilot.next))
        pilot.next = pickNext(layer, composition, pilot);
    const double step = stepLength(layer, composition, pilot, composition.pace);
    if (pilot.pace != composition.pace) {
        pilot.stepStart = bars::rescaledStart(pilot.stepStart, stepLength(layer, composition, pilot, pilot.pace), step, beat);
        pilot.pace = composition.pace;
    }
    const double end = pilot.stepStart + step;
    if (beat < end - kBeatEpsilon)
        return pilot.play;
    const double at = std::max(end, bars::floor(beat));
    if (pilot.next)
        pilot.play = Play{pilot.next, at};
    pilot.stepStart = at;
    pilot.held = false;
    pilot.next = pickNext(layer, composition, pilot);
    return pilot.play;
}

// A column Cue that fires plays its column on every layer from its beat, as a column trigger would, until the layer's
// next trigger. A resync starts over from the firings to come.
void AutopilotStepper::fireColumnCues(const Composition& composition, double beat)
{
    AZL_ZONE("fire column cues");
    if (columnCueResyncs_ != clock_.resyncs()) {
        columnCueResyncs_ = clock_.resyncs();
        lastColumnCue_ = beat;
    }
    if (cues_.columns.empty() || cues_.columns.back().beat <= lastColumnCue_ + kBeatEpsilon)
        return;
    const CueState::ColumnFiring& firing = cues_.columns.back();
    lastColumnCue_ = firing.beat;
    const uint64_t column = cueColumn(composition, firing.column);
    if (!column)
        return;
    forEachLayer(composition.layers, [&](const Layer& layer) {
        if (!layer.group)
            columnCues_[layer.id] = {layer.playing, Play{column, firing.beat}};
    });
}

// The column Autopilot triggers a whole column of the shown deck on every layer at the end of each step, on the bar, as
// a column trigger would, but outside the document, as a column Cue does. While it runs the layers' own Autopilots
// hold their clips (steer); switched off, they step on from the bar they are in. A performer's trigger holds it for
// its hold from the trigger's bar, and a column trigger moves it on from that column. Columns with nothing playable
// are skipped, so it never steps into a black column.
void AutopilotStepper::stepColumns(const Composition& composition, double beat)
{
    AZL_ZONE("step columns");
    const Autopilot& autopilot = composition.columnAutopilot;
    if (autopilot.mode == AutopilotMode::Off) {
        columnPilot_.reset();
        return;
    }
    std::map<uint64_t, std::optional<Play>> triggers;
    forEachLayer(composition.layers, [&](const Layer& layer) {
        if (!layer.group)
            triggers[layer.id] = layer.playing;
    });
    if (!columnPilot_) {
        ColumnPilot& fresh = columnPilot_.emplace();
        fresh.column = composition.column;
        fresh.stepStart = bars::floor(beat);
        fresh.resyncs = clock_.resyncs();
        fresh.pace = composition.pace;
        fresh.triggers = triggers;
        fresh.next = pickColumn(composition, fresh);
    }
    ColumnPilot& pilot = *columnPilot_;
    if (triggers != pilot.triggers) {
        bool manual = false;
        bool column = true;
        double from = bars::floor(beat);
        for (const auto& [id, play] : triggers) {
            auto seen = pilot.triggers.find(id);
            const bool changed = seen != pilot.triggers.end() && seen->second != play;
            column = column && changed && play && play->column == composition.column;
            if (!changed)
                continue;
            manual = true;
            if (play && play->beat && *play->beat > beat)
                from = std::max(from, bars::ceil(*play->beat));
        }
        pilot.triggers = std::move(triggers);
        if (manual) {
            if (column)
                pilot.column = composition.column;
            pilot.stepStart = from;
            pilot.held = autopilot.hold > 0;
            pilot.pace = composition.pace;
            pilot.next = pickColumn(composition, pilot);
        }
    }
    if (pilot.resyncs != clock_.resyncs()) {
        pilot.resyncs = clock_.resyncs();
        pilot.stepStart = 0.0;
    }
    if (!inDeck(shownDeck(composition), pilot.next) || !columnPlayable(composition, pilot.next))
        pilot.next = pickColumn(composition, pilot);
    const double step = columnStep(autopilot, pilot.held, composition.pace);
    if (pilot.pace != composition.pace) {
        pilot.stepStart = bars::rescaledStart(pilot.stepStart, columnStep(autopilot, pilot.held, pilot.pace), step, beat);
        pilot.pace = composition.pace;
    }
    const double end = pilot.stepStart + step;
    if (beat < end - kBeatEpsilon)
        return;
    const double at = std::max(end, bars::floor(beat));
    if (pilot.next) {
        pilot.column = pilot.next;
        pilot.fired = Play{pilot.next, at};
        forEachLayer(composition.layers, [&](const Layer& layer) {
            if (!layer.group)
                columnCues_[layer.id] = {layer.playing, Play{pilot.next, at}};
        });
    }
    pilot.stepStart = at;
    pilot.held = false;
    pilot.next = pickColumn(composition, pilot);
}

void AutopilotStepper::keepLayers(const std::set<uint64_t>& ids)
{
    std::erase_if(pilots_, [&](const auto& pilot) { return !ids.contains(pilot.first); });
    std::erase_if(columnCues_, [&](const auto& cued) { return !ids.contains(cued.first); });
}

// The next column of the shown deck after the one played last (its first when that is in another deck), or a random
// one other than that, with its bottom layer filled where any is; 0 when there is none with anything to play.
uint64_t AutopilotStepper::pickColumn(const Composition& composition, const ColumnPilot& pilot)
{
    const Deck* deck = shownDeck(composition);
    if (!deck || deck->columns.empty())
        return 0;
    const std::vector<Column>& columns = deck->columns;
    auto here = std::find_if(columns.begin(), columns.end(), [&](const Column& c) { return c.id == pilot.column; });
    const size_t from = here == columns.end() ? columns.size() - 1 : size_t(here - columns.begin());
    std::vector<uint64_t> others;
    for (size_t i = 1; i <= columns.size(); ++i) {
        const uint64_t column = columns[(from + i) % columns.size()].id;
        if (column != pilot.column && columnPlayable(composition, column))
            others.push_back(column);
    }
    if (others.empty())
        return 0;
    if (composition.columnAutopilot.mode == AutopilotMode::Random) {
        keepBaseFilled(composition, others);
        return others[keyedUniform(mixKey(layerKey(seed_, Choice::Columns, 0),
                                          uint64_t(std::llround(pilot.stepStart * 16.0))),
                                   uint32_t(others.size()))];
    }
    return others.front();
}

// Random never lands on a column whose bottom layer is empty, unless every candidate is.
void AutopilotStepper::keepBaseFilled(const Composition& composition, std::vector<uint64_t>& columns)
{
    const Layer* base = nullptr;
    forEachLayer(composition.layers, [&](const Layer& layer) {
        if (!base && !layer.group)
            base = &layer;
    });
    if (!base)
        return;
    std::vector<uint64_t> filled;
    std::copy_if(columns.begin(), columns.end(), std::back_inserter(filled),
                 [&](uint64_t column) { return playable_(*base, column); });
    if (!filled.empty())
        columns = std::move(filled);
}

bool AutopilotStepper::columnPlayable(const Composition& composition, uint64_t column)
{
    bool any = false;
    forEachLayer(composition.layers, [&](const Layer& layer) { any = any || (!layer.group && playable_(layer, column)); });
    return any;
}

std::optional<Play> AutopilotStepper::triggered(const Layer& layer)
{
    auto it = columnCues_.find(layer.id);
    if (it == columnCues_.end())
        return layer.playing;
    if (it->second.trigger == layer.playing)
        return it->second.play;
    columnCues_.erase(it);
    return layer.playing;
}

double AutopilotStepper::stepLength(const Layer& layer, const Composition& composition, const Pilot& pilot,
                                    double pace) const
{
    const Autopilot autopilot = autopilotOf(layer, composition);
    return pilot.held ? bars::paced(autopilot.hold, pace) : stepBeats(layer, pilot.play.column, autopilot.bars, pace);
}

Autopilot AutopilotStepper::autopilotOf(const Layer& layer, const Composition& composition) const
{
    const std::optional<Play> play = playOf(layer);
    return cuedAutopilot(layer, play ? columnDeck(composition, play->column) : 0, cues_.autopilot);
}

std::optional<Play> AutopilotStepper::playOf(const Layer& layer) const
{
    if (auto it = pilots_.find(layer.id); it != pilots_.end())
        return it->second.play;
    auto cued = columnCues_.find(layer.id);
    return cued != columnCues_.end() && cued->second.trigger == layer.playing ? cued->second.play : layer.playing;
}

// The next playable cell of the deck in column order, or a random one other than the cell playing; the cell playing
// only when it is the deck's one playable cell. Clips that failed are skipped.
uint64_t AutopilotStepper::pickNext(const Layer& layer, const Composition& composition, Pilot& pilot)
{
    const uint64_t current = pilot.play.column;
    const Deck* deck = deckOf(composition, current);
    if (!deck || deck->columns.empty())
        return 0;
    const std::vector<Column>& columns = deck->columns;
    auto here = std::find_if(columns.begin(), columns.end(), [&](const Column& c) { return c.id == current; });
    const size_t from = here == columns.end() ? columns.size() - 1 : size_t(here - columns.begin());
    std::vector<uint64_t> others;
    for (size_t i = 1; i <= columns.size(); ++i) {
        const uint64_t column = columns[(from + i) % columns.size()].id;
        if (column != current && playable_(layer, column))
            others.push_back(column);
    }
    if (others.empty())
        return playable_(layer, current) ? current : 0;
    if (autopilotOf(layer, composition).mode == AutopilotMode::Random)
        return others[keyedUniform(mixKey(layerKey(seed_, Choice::Pilot, layer.id),
                                          uint64_t(std::llround(pilot.stepStart * 16.0))),
                                   uint32_t(others.size()))];
    return others.front();
}

std::optional<uint64_t> AutopilotStepper::pickedNext(uint64_t layer) const
{
    auto it = pilots_.find(layer);
    if (it == pilots_.end())
        return std::nullopt;
    return it->second.next;
}

uint64_t AutopilotStepper::layerNext(const Layer& layer, const Composition& composition) const
{
    if (columnPilot_)
        return 0;
    auto it = pilots_.find(layer.id);
    if (it == pilots_.end() || !it->second.next || autopilotOf(layer, composition).mode == AutopilotMode::Off)
        return 0;
    return it->second.next;
}

uint64_t AutopilotStepper::columnNext() const
{
    return columnPilot_ ? columnPilot_->next : 0;
}

std::vector<ResumePilot> AutopilotStepper::pilots(const Composition& composition) const
{
    std::vector<ResumePilot> pilots;
    forEachLayer(composition.layers, [&](const Layer& layer) {
        auto p = pilots_.find(layer.id);
        if (p != pilots_.end() && autopilotOf(layer, composition).mode != AutopilotMode::Off)
            pilots.push_back(
                {layer.id, p->second.trigger, p->second.play, p->second.stepStart, p->second.next, p->second.held});
    });
    if (columnPilot_) {
        const ColumnPilot& pilot = *columnPilot_;
        pilots.push_back(
            {0, std::nullopt, pilot.fired.value_or(Play{pilot.column}), pilot.stepStart, pilot.next, pilot.held});
    }
    return pilots;
}

AutopilotStepper::Shown AutopilotStepper::shown(const Layer& layer, const Composition& composition, double beat) const
{
    auto pilot = pilots_.find(layer.id);
    if (pilot == pilots_.end() || autopilotOf(layer, composition).mode == AutopilotMode::Off || columnPilot_)
        return {};
    if (!pilot->second.held)
        return {pilot->second.next, 0};
    const double end = pilot->second.stepStart + stepLength(layer, composition, pilot->second, pilot->second.pace);
    return {pilot->second.next, bars::left(end, beat)};
}

AutopilotStepper::Columns AutopilotStepper::columns(const Composition& composition, double beat) const
{
    if (!columnPilot_)
        return {};
    const ColumnPilot& pilot = *columnPilot_;
    if (!pilot.held)
        return {pilot.column, pilot.next, 0};
    const double end = pilot.stepStart + columnStep(composition.columnAutopilot, true, pilot.pace);
    return {pilot.column, pilot.next, bars::left(end, beat)};
}

} // namespace azl
