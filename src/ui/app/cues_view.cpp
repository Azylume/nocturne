#include "ui/app/cues_view.hpp"

#include "document/cue_edits.hpp"
#include "document/lineup.hpp"
#include "document/wall_clock.hpp"
#include "ui/app/look.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace azl::ui {

namespace {

constexpr int kRow = 20;
constexpr int kGap = 1;
constexpr int kCross = 20;
constexpr int kToggle = 24;
constexpr int kNextWidth = 104;
constexpr int kBottom = kRow + 8;
constexpr double kBar = 4.0;
const char* const kSections[] = {"Intro", "Build", "Drop", "Breakdown", "Outro"};
const char* const kModes[] = {"off", "Next", "Random"};
const int kCueBars[] = {1, 2, 3, 4, 5, 9, 17, 33};
const int kEvery[] = {1, 2, 4, 8, 16};
const int kRepeats[] = {1, 2, 4, 8, 16, 32};
const double kValues[] = {1.0, 0.75, 0.5, 0.25, 0.0};
const int kRamps[] = {0, 1, 2, 4, 8, 16};

const char* const kNewName = "Drop cue";

enum class Kind { Deck, Column, Autopilot, Macro, Effect };

juce::String arrow()
{
    return juce::String::fromUTF8(" \xe2\x86\x92 ");
}

juce::String barsText(int bars)
{
    return juce::String(bars) + (bars == 1 ? " bar" : " bars");
}

Kind kindOf(const Cue& cue)
{
    if (cue.deck)
        return Kind::Deck;
    if (cue.column || (cue.autopilot.empty() && cue.macros.empty() && cue.effects.empty()))
        return Kind::Column;
    if (!cue.autopilot.empty())
        return Kind::Autopilot;
    return cue.macros.empty() ? Kind::Effect : Kind::Macro;
}

juce::String sectionText(const Cue& cue)
{
    return cue.section ? "the " + juce::String(kSections[int(*cue.section)]) : juce::String("the beat clock");
}

juce::String repeatText(const std::optional<int>& every)
{
    return every ? "every " + barsText(*every) : juce::String("once");
}

juce::String whenText(const Cue& cue)
{
    if (cue.prepared)
        return "At the next deck switch (" + utf8(cue.name) + ")";
    if (cue.at) {
        juce::String text = "At " + juce::String(clockText(cue.at->minute));
        if (cue.at->date && cue.gig.empty())
            text += " on " + juce::String(*cue.at->date);
        return cue.name.empty() ? text : text + " (" + utf8(cue.name) + ")";
    }
    const juce::String repeat = cue.every ? ", " + repeatText(cue.every) : juce::String();
    if (cue.section && cue.bar == 1)
        return "When " + sectionText(cue) + " starts" + repeat;
    return "On bar " + juce::String(cue.bar) + " of " + sectionText(cue) + repeat;
}

juce::String autopilotText(const CueAutopilot& a)
{
    if (a.mode == AutopilotMode::Off)
        return "off";
    juce::String text = a.mode ? juce::String(kModes[int(*a.mode)]) : juce::String();
    if (a.bars)
        text += " every " + barsText(*a.bars);
    return text.trim();
}

juce::String effectValueText(const CueEffect& e)
{
    if (e.step)
        return "to step " + juce::String(int(*e.step) + 1);
    return autopilotText({0, e.mode, e.bars});
}

juce::String valueText(const CueMacro& m)
{
    return "to " + juce::String(juce::roundToInt(m.value * 100)) + "%";
}

juce::String rampText(int bars)
{
    return bars ? "over " + barsText(bars) : juce::String("at once");
}

juce::String columnText(const Composition& composition, const std::optional<CueColumn>& column)
{
    if (!column)
        return "nothing";
    if (column->index)
        return "column " + juce::String(int(*column->index) + 1) + " of this deck";
    for (const Deck& deck : composition.decks)
        for (const Column& c : deck.columns)
            if (c.id == column->column)
                return utf8(c.name) + " of " + utf8(deck.name);
    return "a removed column";
}

juce::String deckText(const Composition& composition, uint64_t id)
{
    for (const Deck& deck : composition.decks)
        if (deck.id == id)
            return utf8(deck.name);
    return "a removed deck";
}

juce::String layerText(const Composition& composition, uint64_t id)
{
    juce::String name = "a removed layer";
    forEachLayer(composition.layers, [&](const Layer& layer) {
        if (layer.id == id)
            name = utf8(layer.name);
    });
    return name;
}

juce::String macroText(const Composition& composition, uint64_t id)
{
    for (const Macro& macro : composition.macros)
        if (macro.id == id)
            return utf8(macro.name);
    return "a removed macro";
}

// Every effect with steps, with what names it: its layer, or the composition, and its type.
std::vector<std::pair<uint64_t, juce::String>> steppedEffects(const Composition& composition)
{
    std::vector<std::pair<uint64_t, juce::String>> effects;
    if (!composition.palettes.empty())
        effects.emplace_back(kPaletteEffect, "Palettes");
    forEachChain(composition, [&](const std::vector<Effect>& chain, uint64_t layer, std::optional<uint64_t>) {
        for (const Effect& effect : chain)
            if (!effect.steps.empty())
                effects.emplace_back(effect.id, (layer ? layerText(composition, layer) : juce::String("Composition")) +
                                                    juce::String::fromUTF8(" \xc2\xb7 ") + utf8(effect.type));
    });
    return effects;
}

juce::String effectText(const Composition& composition, uint64_t id)
{
    for (const auto& [effect, name] : steppedEffects(composition))
        if (effect == id)
            return name;
    return "an effect without steps";
}

std::vector<uint64_t> cueLayers(const Composition& composition)
{
    std::vector<uint64_t> ids;
    forEachLayer(composition.layers, [&](const Layer& layer) {
        if (!layer.group)
            ids.push_back(layer.id);
    });
    return ids;
}

// Splits a long list into submenus of kMenuItems entries, nested again while they are more than `limit`.
Menu fitted(Menu entries, size_t limit = CuesView::kMenuItems)
{
    if (entries.size() <= limit)
        return entries;
    Menu pages;
    for (size_t i = 0; i < entries.size(); i += CuesView::kMenuItems) {
        const size_t end = std::min(entries.size(), i + CuesView::kMenuItems);
        pages.push_back({.label = entries[i].label + juce::String::fromUTF8(" \xe2\x80\x93 ") + entries[end - 1].label,
                         .sub = Menu(entries.begin() + ptrdiff_t(i), entries.begin() + ptrdiff_t(end))});
    }
    return fitted(std::move(pages), limit);
}

std::optional<double> nextIn(const std::vector<CueFiring>& upcoming, uint64_t cue)
{
    for (const CueFiring& firing : upcoming)
        if (firing.cue == cue)
            return firing.beat;
    return std::nullopt;
}

bool firedIn(const CueState& state, uint64_t cue, double beat)
{
    return std::any_of(state.active.begin(), state.active.end(),
                       [&](const CueFiring& f) { return f.cue == cue && beat - f.beat < kBar; });
}

juce::String nextFrom(const std::vector<CueFiring>& upcoming, const CueState& state, const Cue& cue, double beat)
{
    if (!cue.enabled)
        return "off";
    if (cue.at)
        return "at " + juce::String(clockText(cue.at->minute));
    if (cue.prepared)
        return "waits for a deck switch";
    if (const auto at = nextIn(upcoming, cue.id))
        return "next: in " + barsText(std::max(1, int(std::ceil((*at - beat) / kBar - 1e-6))));
    if (std::any_of(state.active.begin(), state.active.end(), [&](const CueFiring& f) { return f.cue == cue.id; }))
        return "fired";
    if (cue.section)
        return "waits for " + sectionText(cue);
    return {};
}

void paintGigHeader(juce::Graphics& g, const Composition& composition, const std::string& date,
                    juce::Rectangle<int> row)
{
    const Palette& p = Look::palette();
    juce::String text = "Gig " + juce::String(date);
    for (const GigWindow& w : composition.gigs)
        if (w.date == date)
            text += ", " + juce::String(clockText(w.start)) + "-" + juce::String(clockText(w.end));
    g.setColour(p.header);
    g.fillRect(row);
    g.setColour(p.accent);
    drawText(g, text, row.withTrimmedLeft(6), juce::Justification::centredLeft);
    g.setColour(p.muted);
    drawText(g, "Edit lineup", row.withTrimmedRight(8), juce::Justification::centredRight);
}

Cue newCue()
{
    return {.name = kNewName, .section = Section::Drop, .column = CueColumn{0, 0}};
}

// Back to a musical position: a time-of-day cue's deck switch goes, its column stays.
void leaveClock(Cue& cue)
{
    if (!cue.at)
        return;
    cue.at.reset();
    cue.deck.reset();
    if (!cue.column)
        cue.column = CueColumn{0, 0};
}

// A time-of-day cue carries no Section, repeat, Autopilot, macro or effect: those go, a column stays or is the first.
void toClock(Cue& cue)
{
    if (cue.at)
        return;
    const int nextHour = (localMinute(std::time(nullptr)) / 60 + 1) % 24 * 60;
    cue.at = CueAt{nextHour};
    cue.section.reset();
    cue.every.reset();
    cue.bar = 1;
    cue.autopilot.clear();
    cue.macros.clear();
    cue.effects.clear();
    if (!cue.column)
        cue.column = CueColumn{0, 0};
    if (cue.name == kNewName)
        cue.name.clear();
}

Menu timeMenu(const CueAt& current, const std::function<void(int)>& pick)
{
    Menu hours;
    for (int hour = 0; hour < 24; ++hour) {
        Menu minutes;
        for (int minute = hour * 60; minute < hour * 60 + 60; minute += 5)
            minutes.push_back({.label = clockText(minute), .run = [pick, minute] { pick(minute); },
                               .ticked = current.minute == minute});
        hours.push_back({.label = clockText(hour * 60), .ticked = current.minute / 60 == hour, .sub = std::move(minutes)});
    }
    return hours;
}

void dropAction(Cue& cue)
{
    switch (kindOf(cue)) {
    case Kind::Deck:
        cue.deck.reset();
        break;
    case Kind::Column:
        cue.column.reset();
        break;
    case Kind::Autopilot:
        cue.autopilot.erase(cue.autopilot.begin());
        break;
    case Kind::Macro:
        cue.macros.erase(cue.macros.begin());
        break;
    case Kind::Effect:
        cue.effects.erase(cue.effects.begin());
        break;
    }
}

} // namespace

CuesView::Rows::Rows(CuesView& owner) : owner(owner)
{
    setInterceptsMouseClicks(false, true);
}

CuesView::CuesView(Session& session)
    : session_(session), gig_(session), clock_([this] { return session_.client().beatNow(); })
{
    gigButton_.setTooltip("Tonight's Gig at this venue: its window and lineup, each DJ's deck switched on time");
    gigButton_.onClick = [this] { showsGig() ? closeGig() : openGig(); };
    gig_.onClose = [this] { closeGig(); };
    addAndMakeVisible(gigButton_);
    addChildComponent(gig_);
    add_.setTooltip("Build a new cue: when it fires, what it does and on what");
    cancel_.setTooltip("Close the builder without adding the cue");
    add_.onClick = [this] { open(std::nullopt); };
    done_.onClick = [this] {
        if (draft_ && !draft_->id)
            session_.client().edit(AddCue{std::nullopt, *draft_});
        close();
    };
    cancel_.onClick = [this] { close(); };
    addAndMakeVisible(rows_);
    addAndMakeVisible(add_);
    rows_.addChildComponent(done_);
    rows_.addChildComponent(cancel_);
    session_.addView(this);
}

CuesView::~CuesView()
{
    session_.removeView(this);
}

void CuesView::setClock(std::function<double()> clock)
{
    clock_ = std::move(clock);
    rows_.repaint();
}

void CuesView::sessionChanged(int changes)
{
    const auto& cues = session_.composition().cues;
    if (changes & Session::Document) {
        if (draft_ && draft_->id) {
            auto it = std::find_if(cues.begin(), cues.end(), [&](const Cue& c) { return c.id == draft_->id; });
            if (it == cues.end())
                return close();
            if (*it != *draft_) {
                draft_ = *it;
                rebuild();
            }
        }
        resized();
        rows_.repaint();
    } else if (changes & Session::Switches) {
        resized();
        rows_.repaint();
    } else if ((changes & Session::Clock) && !cues.empty()) {
        rows_.repaint();
    }
}

void CuesView::open(std::optional<uint64_t> cue)
{
    const auto& cues = session_.composition().cues;
    if (cue) {
        auto it = std::find_if(cues.begin(), cues.end(), [&](const Cue& c) { return c.id == *cue; });
        if (it == cues.end())
            return;
        draft_ = *it;
    } else {
        draft_ = newCue();
    }
    rebuild();
    const int bottom = builderArea().getBottom() + scroll_;
    scroll_ = std::max(scroll_, bottom - listArea().getHeight());
    resized();
    rows_.repaint();
}

void CuesView::openGig(std::optional<std::string> date)
{
    close();
    gig_.load(std::move(date));
    gig_.setVisible(true);
    rows_.setVisible(false);
    add_.setVisible(false);
    resized();
}

void CuesView::closeGig()
{
    gig_.setVisible(false);
    rows_.setVisible(true);
    add_.setVisible(true);
    resized();
}

void CuesView::close()
{
    draft_.reset();
    rebuild();
    scroll_ = std::clamp(scroll_, 0, std::max(0, contentHeight() - listArea().getHeight()));
    resized();
    rows_.repaint();
}

void CuesView::change(const std::function<void(Cue&)>& edit)
{
    if (!draft_)
        return;
    edit(*draft_);
    if (draft_->id)
        session_.client().edit(SetCue{*draft_});
    rebuild();
    resized();
    rows_.repaint();
}

bool CuesView::shows(Part part) const
{
    if (!draft_)
        return false;
    const Kind kind = kindOf(*draft_);
    switch (part) {
    case Part::Time:
        return draft_->at.has_value();
    case Part::Bar:
    case Part::Repeat:
        return !draft_->at;
    case Part::Value:
        return kind != Kind::Column && kind != Kind::Deck;
    case Part::Ramp:
        return kind == Kind::Macro;
    case Part::Also:
        return kind == Kind::Deck;
    default:
        return true;
    }
}

juce::String CuesView::pickLabel(Part part) const
{
    if (!shows(part))
        return {};
    const Cue& cue = *draft_;
    const Composition& composition = session_.composition();
    const Kind kind = kindOf(cue);
    switch (part) {
    case Part::When:
        return cue.at ? juce::String("the clock reads") : sectionText(cue);
    case Part::Time:
        return juce::String(clockText(cue.at->minute)) + (cue.at->date ? " on " + juce::String(*cue.at->date) : juce::String());
    case Part::Bar:
        return cue.section && cue.bar == 1 ? juce::String("as it starts") : "bar " + juce::String(cue.bar);
    case Part::Repeat:
        return repeatText(cue.every);
    case Part::Do:
        return kind == Kind::Deck        ? "show"
               : kind == Kind::Column    ? "fire"
               : kind == Kind::Autopilot ? "Autopilot"
               : kind == Kind::Effect    ? "effect steps"
                                         : "macro";
    case Part::Value:
        if (kind == Kind::Effect)
            return effectValueText(cue.effects[0]);
        return kind == Kind::Autopilot ? autopilotText(cue.autopilot[0]) : valueText(cue.macros[0]);
    case Part::Ramp:
        return rampText(cue.macros[0].bars);
    case Part::Target:
        if (kind == Kind::Deck)
            return deckText(composition, *cue.deck);
        if (kind == Kind::Column)
            return columnText(composition, cue.column);
        if (kind == Kind::Effect)
            return effectText(composition, cue.effects[0].effect);
        return kind == Kind::Autopilot ? layerText(composition, cue.autopilot[0].layer)
                                       : macroText(composition, cue.macros[0].macro);
    case Part::Also:
        return "and fire " + columnText(composition, cue.column);
    }
    return {};
}

Menu CuesView::pickMenu(Part part)
{
    if (!shows(part))
        return {};
    const Cue& cue = *draft_;
    const Composition& composition = session_.composition();
    const Kind kind = kindOf(cue);
    Menu menu;
    switch (part) {
    case Part::When:
        for (int i = 0; i < 5; ++i)
            menu.push_back({.label = kSections[i],
                            .run = [this, i] { change([i](Cue& c) { leaveClock(c); c.section = Section(i); }); },
                            .ticked = cue.section == Section(i),
                            .tip = juce::String("Fires in every ") + kSections[i] + ", counted from its first bar"});
        menu.push_back({});
        menu.push_back({.label = "The beat clock",
                        .run = [this] { change([](Cue& c) { leaveClock(c); c.section.reset(); }); },
                        .ticked = !cue.section && !cue.at,
                        .tip = "Fires on a bar of the beat clock counted from its start"});
        menu.push_back({.label = "A time of day",
                        .run = [this] { change(toClock); },
                        .ticked = cue.at.has_value(),
                        .tip = "Fires at a time of day on the venue's clock, on the next bar after it; it can show a deck"});
        break;
    case Part::Time:
        return timeMenu(*cue.at, [this](int minute) { change([minute](Cue& c) { c.at->minute = minute; }); });
    case Part::Repeat: {
        std::vector<int> repeats(std::begin(kRepeats), std::end(kRepeats));
        if (cue.every && std::find(repeats.begin(), repeats.end(), *cue.every) == repeats.end())
            repeats.insert(std::upper_bound(repeats.begin(), repeats.end(), *cue.every), *cue.every);
        menu.push_back({.label = "Once",
                        .run = [this] { change([](Cue& c) { c.every.reset(); }); },
                        .ticked = !cue.every});
        for (int bars : repeats)
            menu.push_back({.label = "Every " + barsText(bars),
                            .run = [this, bars] { change([bars](Cue& c) { c.every = bars; }); },
                            .ticked = cue.every == bars});
        break;
    }
    case Part::Bar: {
        std::vector<int> bars(std::begin(kCueBars), std::end(kCueBars));
        if (std::find(bars.begin(), bars.end(), cue.bar) == bars.end())
            bars.insert(std::upper_bound(bars.begin(), bars.end(), cue.bar), cue.bar);
        for (int bar : bars)
            menu.push_back({.label = bar == 1 && cue.section ? juce::String("As it starts") : "Bar " + juce::String(bar),
                            .run = [this, bar] { change([bar](Cue& c) { c.bar = bar; }); },
                            .ticked = cue.bar == bar});
        break;
    }
    case Part::Do: {
        const bool clock = cue.at.has_value();
        const juce::String clockOnly = "A cue at a time of day only shows a deck and fires a column";
        const uint64_t shown = composition.decks.empty() ? 0 : composition.deck;
        if (clock)
            menu.push_back({.label = "Show a deck",
                            .run = [this, shown] { change([shown](Cue& c) {
                                if (!c.deck)
                                    c.deck = shown;
                            }); },
                            .enabled = shown != 0,
                            .ticked = kind == Kind::Deck,
                            .tip = "Shows a deck when the time comes; its column starts on the next bar"});
        const auto layers = cueLayers(composition);
        const bool macros = !composition.macros.empty();
        menu.push_back({.label = "Fire a column",
                        .run = [this] { change([](Cue& c) {
                            c.deck.reset();
                            if (kindOf(c) != Kind::Column || !c.column) {
                                dropAction(c);
                                c.column = CueColumn{0, 0};
                            }
                        }); },
                        .ticked = kind == Kind::Column,
                        .tip = "Starts a column on every layer, as a column trigger does"});
        menu.push_back({.label = "Set a layer's Autopilot",
                        .run = [this, layer = layers.empty() ? 0 : layers[0]] { change([layer](Cue& c) {
                            if (kindOf(c) == Kind::Autopilot)
                                return;
                            dropAction(c);
                            std::erase_if(c.autopilot, [&](const CueAutopilot& a) { return a.layer == layer; });
                            c.autopilot.insert(c.autopilot.begin(), CueAutopilot{layer, AutopilotMode::Next, 4});
                        }); },
                        .enabled = !clock && !layers.empty(),
                        .ticked = kind == Kind::Autopilot,
                        .tip = clock           ? clockOnly
                               : layers.empty() ? "Add a layer first"
                                                : "Changes a layer's Autopilot until the Section ends"});
        menu.push_back({.label = "Move a macro",
                        .run = [this, macro = macros ? composition.macros[0].id : 0] { change([macro](Cue& c) {
                            if (kindOf(c) == Kind::Macro)
                                return;
                            dropAction(c);
                            std::erase_if(c.macros, [&](const CueMacro& m) { return m.macro == macro; });
                            c.macros.insert(c.macros.begin(), CueMacro{macro, 1.0, 0});
                        }); },
                        .enabled = !clock && macros,
                        .ticked = kind == Kind::Macro,
                        .tip = clock    ? clockOnly
                               : macros ? "Moves a macro to a value until the Section ends"
                                        : "Add a macro first"});
        const auto stepped = steppedEffects(composition);
        menu.push_back({.label = "Step an effect",
                        .run = [this, effect = stepped.empty() ? 0 : stepped[0].first] { change([effect](Cue& c) {
                            if (kindOf(c) == Kind::Effect)
                                return;
                            dropAction(c);
                            std::erase_if(c.effects, [&](const CueEffect& e) { return e.effect == effect; });
                            c.effects.insert(c.effects.begin(), CueEffect{effect, AutopilotMode::Next, 4});
                        }); },
                        .enabled = !clock && !stepped.empty(),
                        .ticked = kind == Kind::Effect,
                        .tip = clock            ? clockOnly
                               : stepped.empty() ? "Give an effect steps first"
                                                 : "Sets an effect's Autopilot or moves it to one of its steps"});
        break;
    }
    case Part::Value:
        if (kind == Kind::Effect)
            return effectValueMenu(composition, cue.effects[0]);
        if (kind == Kind::Autopilot) {
            const CueAutopilot& a = cue.autopilot[0];
            menu.push_back({.label = "Off",
                            .run = [this] { change([](Cue& c) { c.autopilot[0].mode = AutopilotMode::Off; c.autopilot[0].bars.reset(); }); },
                            .ticked = a.mode == AutopilotMode::Off});
            for (AutopilotMode mode : {AutopilotMode::Next, AutopilotMode::Random}) {
                Menu every;
                for (int bars : kEvery)
                    every.push_back({.label = "Every " + barsText(bars),
                                     .run = [this, mode, bars] { change([mode, bars](Cue& c) { c.autopilot[0].mode = mode; c.autopilot[0].bars = bars; }); },
                                     .ticked = a.mode == mode && a.bars == bars});
                menu.push_back({.label = kModes[int(mode)], .ticked = a.mode == mode, .sub = every});
            }
        } else {
            for (double value : kValues)
                menu.push_back({.label = valueText({0, value, 0}),
                                .run = [this, value] { change([value](Cue& c) { c.macros[0].value = value; }); },
                                .ticked = std::abs(cue.macros[0].value - value) < 1e-9});
        }
        break;
    case Part::Ramp:
        for (int bars : kRamps)
            menu.push_back({.label = rampText(bars).substring(0, 1).toUpperCase() + rampText(bars).substring(1),
                            .run = [this, bars] { change([bars](Cue& c) { c.macros[0].bars = bars; }); },
                            .ticked = cue.macros[0].bars == bars});
        break;
    case Part::Also: {
        menu = columnMenu(composition, cue.column,
                          [this](const CueColumn& column) { change([column](Cue& c) { c.column = column; }); }, 2);
        menu.push_back({});
        menu.push_back({.label = "Nothing",
                        .run = [this] { change([](Cue& c) { c.column.reset(); }); },
                        .ticked = !cue.column,
                        .tip = "Only shows the deck; its layers go on with what they play"});
        break;
    }
    case Part::Target:
        if (kind == Kind::Deck) {
            for (const Deck& deck : composition.decks)
                menu.push_back({.label = utf8(deck.name),
                                .run = [this, id = deck.id] { change([id](Cue& c) { c.deck = id; }); },
                                .ticked = cue.deck == deck.id});
            return fitted(std::move(menu));
        }
        if (kind == Kind::Column)
            return columnMenu(composition, cue.column,
                              [this](const CueColumn& column) { change([column](Cue& c) { c.column = column; }); });
        if (kind == Kind::Effect) {
            for (const auto& [effect, name] : steppedEffects(composition))
                menu.push_back({.label = name,
                                .run = [this, effect] { change([effect](Cue& c) { c.effects[0].effect = effect; }); },
                                .ticked = cue.effects[0].effect == effect});
        } else if (kind == Kind::Autopilot) {
            for (uint64_t layer : cueLayers(composition))
                menu.push_back({.label = layerText(composition, layer),
                                .run = [this, layer] { change([layer](Cue& c) { c.autopilot[0].layer = layer; }); },
                                .ticked = cue.autopilot[0].layer == layer});
        } else {
            for (const Macro& macro : composition.macros)
                menu.push_back({.label = utf8(macro.name),
                                .run = [this, id = macro.id] { change([id](Cue& c) { c.macros[0].macro = id; }); },
                                .ticked = cue.macros[0].macro == macro.id});
        }
        return fitted(std::move(menu));
    }
    return menu;
}

// An effect's Autopilot off, Next or Random every so many bars, or a move to one of its steps.
Menu CuesView::effectValueMenu(const Composition& composition, const CueEffect& current)
{
    Menu menu{{.label = "Autopilot off",
               .run = [this] { change([](Cue& c) { c.effects[0] = {c.effects[0].effect, AutopilotMode::Off}; }); },
               .ticked = current.mode == AutopilotMode::Off && !current.step}};
    for (AutopilotMode mode : {AutopilotMode::Next, AutopilotMode::Random}) {
        Menu every;
        for (int bars : kEvery)
            every.push_back({.label = "Every " + barsText(bars),
                             .run = [this, mode, bars] {
                                 change([mode, bars](Cue& c) { c.effects[0] = {c.effects[0].effect, mode, bars}; });
                             },
                             .ticked = current.mode == mode && current.bars == bars && !current.step});
        menu.push_back({.label = kModes[int(mode)], .ticked = current.mode == mode && !current.step, .sub = every});
    }
    Menu steps;
    const Effect* effect = effectById(composition, current.effect);
    std::vector<std::string> names;
    if (current.effect == kPaletteEffect)
        for (const ColourPalette& palette : composition.palettes)
            names.push_back(palette.name);
    for (size_t i = 0; effect && i < effect->steps.size(); ++i)
        names.push_back(effect->steps[i].name);
    for (size_t i = 0; i < names.size(); ++i)
        steps.push_back({.label = "Step " + juce::String(int(i) + 1) + "  " + utf8(names[i]),
                         .run = [this, i] {
                             change([i](Cue& c) { c.effects[0] = {c.effects[0].effect, std::nullopt, std::nullopt, i}; });
                         },
                         .ticked = current.step == i});
    menu.push_back({.label = "Go to step", .enabled = !steps.empty(), .ticked = current.step.has_value(), .sub = steps});
    return menu;
}

Menu CuesView::columnMenu(const Composition& composition, const std::optional<CueColumn>& current,
                          const std::function<void(const CueColumn&)>& pick, size_t spare)
{
    size_t widest = 1;
    for (const Deck& deck : composition.decks)
        widest = std::max(widest, deck.columns.size());
    const Deck* shown = nullptr;
    for (const Deck& deck : composition.decks)
        if (deck.id == composition.deck)
            shown = &deck;
    Menu positions;
    for (size_t i = 0; i < widest; ++i) {
        const bool now = shown && i < shown->columns.size();
        positions.push_back({.label = "Column " + juce::String(int(i) + 1),
                             .run = [pick, i] { pick({0, i}); },
                             .ticked = current && current->index == i,
                             .tip = now ? "Now: " + utf8(shown->columns[i].name) + " of " + utf8(shown->name)
                                        : juce::String("The deck in view has no column here now")});
    }
    Menu decks;
    for (const Deck& deck : composition.decks) {
        Menu columns;
        bool holds = false;
        for (const Column& column : deck.columns) {
            const bool ticked = current && !current->index && current->column == column.id;
            holds = holds || ticked;
            columns.push_back({.label = utf8(column.name),
                               .run = [pick, id = column.id] { pick({id, std::nullopt}); },
                               .ticked = ticked});
        }
        if (columns.empty())
            columns.push_back({.label = "No columns", .run = [] {}, .enabled = false});
        decks.push_back({.label = utf8(deck.name), .ticked = holds, .sub = fitted(std::move(columns))});
    }
    Menu menu{{.label = kThisDeck, .ticked = current && current->index.has_value(), .sub = fitted(std::move(positions))}, {}};
    for (MenuEntry& entry : fitted(std::move(decks), kMenuItems - menu.size() - spare))
        menu.push_back(std::move(entry));
    return menu;
}

juce::String CuesView::describe(const Composition& composition, const Cue& cue)
{
    juce::StringArray actions;
    if (cue.deck)
        actions.add("show" + arrow() + deckText(composition, *cue.deck));
    if (cue.column)
        actions.add("fire" + arrow() + columnText(composition, cue.column));
    for (const CueAutopilot& a : cue.autopilot)
        actions.add("Autopilot " + autopilotText(a) + arrow() + layerText(composition, a.layer));
    for (const CueMacro& m : cue.macros)
        actions.add("macro " + valueText(m) + (m.bars ? " " + rampText(m.bars) : juce::String()) + arrow() +
                    macroText(composition, m.macro));
    for (const CueEffect& e : cue.effects)
        actions.add("effect steps " + effectValueText(e) + arrow() + effectText(composition, e.effect));
    if (actions.isEmpty())
        actions.add("nothing");
    return whenText(cue) + arrow() + actions.joinIntoString("; ");
}

juce::String CuesView::nextText(const Composition& composition, const Cue& cue, double beat)
{
    CueState state;
    cueStateAt(composition, beat, state);
    return nextFrom(upcomingCues(composition, beat), state, cue, beat);
}

bool CuesView::firing(const Composition& composition, const Cue& cue, double beat)
{
    CueState state;
    cueStateAt(composition, beat, state);
    return firedIn(state, cue.id, beat);
}

void CuesView::rebuild()
{
    pieces_.clear();
    const bool adding = draft_ && !draft_->id;
    add_.setVisible(!adding);
    done_.setVisible(draft_.has_value());
    cancel_.setVisible(adding);
    if (!draft_)
        return;
    done_.setButtonText(adding ? "Add" : "Done");
    done_.setTooltip(adding ? "Add this cue" : "Close the builder; every pick is already applied");
    auto text = [this](const juce::String& words) {
        auto label = std::make_unique<juce::Label>(juce::String(), words);
        label->setFont(Look::font());
        label->setColour(juce::Label::textColourId, Look::palette().muted);
        label->setBorderSize({});
        rows_.addAndMakeVisible(*label);
        pieces_.push_back(std::move(label));
    };
    auto chip = [this](Part part, const juce::String& tip) {
        if (!shows(part))
            return;
        auto button = std::make_unique<juce::TextButton>(pickLabel(part));
        button->setTooltip(tip);
        button->onClick = [this, part, b = button.get()] { showMenu(*b, pickMenu(part)); };
        rows_.addAndMakeVisible(*button);
        pieces_.push_back(std::move(button));
    };
    text("When");
    chip(Part::When, "The Section the cue fires in, the beat clock, or a time of day");
    chip(Part::Time, "The time of day on the venue's clock; it fires on the next bar after it");
    chip(Part::Bar, "The bar it fires on, counted from the start");
    chip(Part::Repeat, "Fires once, or again every so many bars from there");
    text(arrow().trim());
    chip(Part::Do, "What the cue does");
    chip(Part::Value, "The Autopilot it sets, or the macro's value");
    chip(Part::Ramp, "How long the macro takes to get there");
    text(arrow().trim());
    chip(Part::Target, "What it acts on");
    chip(Part::Also, "The column the deck starts on the next bar, or nothing");
}

int CuesView::flowBuilder(juce::Rectangle<int> area, bool place) const
{
    if (!draft_)
        return 0;
    area = area.reduced(4);
    const juce::Font font = Look::font();
    int x = area.getX(), y = area.getY();
    for (const auto& piece : pieces_) {
        const bool label = dynamic_cast<juce::Label*>(piece.get()) != nullptr;
        const juce::String text = label ? static_cast<juce::Label*>(piece.get())->getText()
                                        : static_cast<juce::TextButton*>(piece.get())->getButtonText();
        const int width = std::min(area.getWidth(), juce::GlyphArrangement::getStringWidthInt(font, text) + (label ? 4 : 16));
        if (x > area.getX() && x + width > area.getRight()) {
            x = area.getX();
            y += kRow + 3;
        }
        if (place)
            piece->setBounds(x, y, width, kRow);
        x += width + 3;
    }
    return y + 2 * kRow + 3 + 4 - (area.getY() - 4);
}

std::vector<CuesView::Slot> CuesView::slots() const
{
    const auto& cues = session_.composition().cues;
    std::vector<Slot> list;
    std::vector<std::string> gigs;
    for (size_t i = 0; i < cues.size(); ++i) {
        if (cues[i].gig.empty())
            list.push_back({i, {}});
        else if (std::find(gigs.begin(), gigs.end(), cues[i].gig) == gigs.end())
            gigs.push_back(cues[i].gig);
    }
    std::sort(gigs.begin(), gigs.end());
    const EngineClient::Switches switches = session_.client().switches();
    for (const std::string& gig : gigs) {
        list.push_back({std::nullopt, gig});
        for (size_t i = 0; i < cues.size(); ++i)
            if (cues[i].gig == gig)
                list.push_back({i, gig});
        if (gig != switches.date || switches.lines.empty())
            continue;
        list.push_back({std::nullopt, gig, true});
        for (size_t i = 0; switchesOpen_ && i < switches.lines.size(); ++i)
            list.push_back({std::nullopt, gig, true, i});
    }
    if (draft_ && !draft_->id)
        list.push_back({cues.size(), {}});
    return list;
}

size_t CuesView::rowCount() const
{
    return slots().size();
}

std::optional<size_t> CuesView::draftRow() const
{
    if (!draft_)
        return std::nullopt;
    const auto& cues = session_.composition().cues;
    for (size_t i = 0; i < cues.size(); ++i)
        if (cues[i].id == draft_->id)
            return i;
    return cues.size();
}

int CuesView::contentHeight() const
{
    const int builder = draft_ ? flowBuilder({0, 0, getWidth(), 0}, false) + kGap : 0;
    return int(rowCount()) * (kRow + kGap) + builder;
}

juce::Rectangle<int> CuesView::listArea() const
{
    return getLocalBounds().withTrimmedBottom(kBottom);
}

juce::Rectangle<int> CuesView::slotArea(size_t position) const
{
    int y = int(position) * (kRow + kGap) - scroll_;
    if (const auto row = draftRow()) {
        const std::vector<Slot> list = slots();
        const auto edited = std::find_if(list.begin(), list.end(), [&](const Slot& s) { return s.cue == row; });
        if (position > size_t(edited - list.begin()))
            y += flowBuilder({0, 0, getWidth(), 0}, false) + kGap;
    }
    return {0, y, getWidth(), kRow};
}

juce::Rectangle<int> CuesView::rowArea(size_t index) const
{
    const std::vector<Slot> list = slots();
    const auto it = std::find_if(list.begin(), list.end(), [&](const Slot& s) { return s.cue == index; });
    return slotArea(size_t(it - list.begin()));
}

juce::Rectangle<int> CuesView::gigArea(const std::string& date) const
{
    const std::vector<Slot> list = slots();
    const auto it = std::find_if(list.begin(), list.end(), [&](const Slot& s) { return s.header() && s.gig == date; });
    return it == list.end() ? juce::Rectangle<int>() : slotArea(size_t(it - list.begin()));
}

juce::Rectangle<int> CuesView::switchesArea() const
{
    const std::vector<Slot> list = slots();
    const auto it = std::find_if(list.begin(), list.end(), [](const Slot& s) { return s.switches && !s.line; });
    return it == list.end() ? juce::Rectangle<int>() : slotArea(size_t(it - list.begin()));
}

std::vector<juce::String> CuesView::shownSwitches() const
{
    const EngineClient::Switches switches = session_.client().switches();
    std::vector<juce::String> shown;
    for (const Slot& slot : slots())
        if (slot.line && *slot.line < switches.lines.size())
            shown.push_back(switchText(switches.lines[*slot.line]));
    return shown;
}

juce::String CuesView::switchText(const Message& line)
{
    const std::string local = line.value("local", "");
    juce::String text = local.size() >= 19 ? juce::String(local.substr(11, 8)) : juce::String(local);
    const std::string name = line.value("deckName", "");
    text += "  " + (name.empty() ? "Deck " + juce::String(line.value("deck", uint64_t(0))) : juce::String(name));
    if (line.contains("slot") && line["slot"].is_number())
        text += "  slot " + juce::String(line["slot"].get<int>()) + " " + juce::String(line.value("dj", ""));
    return text + "  by " + juce::String(line.value("cause", ""));
}

juce::Rectangle<int> CuesView::removeArea(size_t index) const
{
    return rowArea(index).removeFromRight(kCross);
}

juce::Rectangle<int> CuesView::toggleArea(size_t index) const
{
    return rowArea(index).withTrimmedRight(kCross).removeFromRight(kToggle);
}

juce::Rectangle<int> CuesView::nextArea(size_t index) const
{
    return rowArea(index).withTrimmedRight(kCross + kToggle).removeFromRight(kNextWidth);
}

juce::Rectangle<int> CuesView::builderArea() const
{
    const auto row = draftRow();
    if (!row)
        return {};
    const int height = flowBuilder({0, 0, getWidth(), 0}, false);
    return {0, rowArea(*row).getBottom() + kGap, getWidth(), height};
}

void CuesView::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    scroll_ = wheelScrolled(scroll_, wheel, contentHeight(), listArea().getHeight());
    resized();
    rows_.repaint();
}

void CuesView::resized()
{
    rows_.setBounds(listArea());
    gig_.setBounds(listArea());
    auto bottom = getLocalBounds().removeFromBottom(kBottom).reduced(4);
    add_.setBounds(bottom.removeFromRight(90));
    bottom.removeFromRight(4);
    gigButton_.setBounds(bottom.removeFromRight(48));
    const auto builder = builderArea();
    flowBuilder(builder, true);
    auto buttons = builder.reduced(4).removeFromBottom(kRow);
    done_.setBounds(buttons.removeFromRight(64));
    buttons.removeFromRight(3);
    cancel_.setBounds(buttons.removeFromRight(64));
}

void CuesView::paint(juce::Graphics& g)
{
    g.setColour(Look::palette().header);
    g.fillRect(getLocalBounds().removeFromBottom(kBottom));
}

void CuesView::paintRows(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    const Composition& composition = session_.composition();
    g.setFont(Look::font());
    if (rowCount() == 0) {
        g.setColour(p.muted);
        drawText(g, "No cues yet. A cue reads: when, do, on what.", rows_.getLocalBounds().withHeight(kRow).reduced(6, 0),
                 juce::Justification::centredLeft);
        return;
    }
    const double beat = clock_();
    const std::vector<CueFiring> upcoming = upcomingCues(composition, beat);
    CueState state;
    cueStateAt(composition, beat, state);
    const auto edited = draftRow();
    const std::vector<Slot> list = slots();
    for (size_t position = 0; position < list.size(); ++position) {
        const juce::Rectangle<int> row = slotArea(position);
        if (!row.intersects(rows_.getLocalBounds()))
            continue;
        if (list[position].switches) {
            const EngineClient::Switches switches = session_.client().switches();
            g.setColour(p.cell);
            g.fillRect(row);
            g.setColour(p.muted);
            const juce::String text =
                list[position].line ? switchText(switches.lines.at(*list[position].line))
                                    : juce::String(switchesOpen_ ? "- " : "+ ") + "Deck switches (" +
                                          juce::String(int(switches.lines.size())) + ")";
            drawText(g, text, row.withTrimmedLeft(list[position].line ? 18 : 6), juce::Justification::centredLeft);
            continue;
        }
        if (!list[position].cue) {
            paintGigHeader(g, composition, list[position].gig, row);
            continue;
        }
        const size_t i = *list[position].cue;
        const bool isDraft = edited == i;
        const Cue& cue = isDraft ? *draft_ : composition.cues[i];
        const bool lit = cue.id && firedIn(state, cue.id, beat);
        g.setColour(lit ? p.liveRow : p.cell);
        g.fillRect(row);
        if (isDraft) {
            g.setColour(p.accent);
            g.drawRect(row);
        }
        g.setColour(lit ? p.live : cue.enabled ? p.text : p.muted);
        drawText(g, describe(composition, cue), row.withTrimmedLeft(6).withRight(nextArea(i).getX() - 4),
                 juce::Justification::centredLeft);
        g.setColour(lit ? p.live : p.muted);
        drawText(g, cue.id ? nextFrom(upcoming, state, cue, beat) : juce::String("new"), nextArea(i),
                 juce::Justification::centredRight);
        if (cue.id) {
            const auto box = toggleArea(i).withSizeKeepingCentre(10, 10).toFloat();
            g.setColour(cue.enabled ? p.accent : p.muted);
            if (cue.enabled)
                g.fillRect(box);
            else
                g.drawRect(box);
            g.setColour(p.muted);
            drawText(g, juce::String::fromUTF8("\xc3\x97"), removeArea(i), juce::Justification::centred);
        }
    }
    if (edited) {
        g.setColour(p.header);
        g.fillRect(builderArea());
    }
}

juce::String CuesView::tipAt(juce::Point<int> point)
{
    if (!listArea().contains(point))
        return {};
    const auto& cues = session_.composition().cues;
    for (const Slot& slot : slots())
        if (slot.header() && gigArea(slot.gig).contains(point))
            return "The lineup of the Gig on " + juce::String(slot.gig) + ": a deck switch per DJ. Click to edit it";
    if (switchesArea().contains(point))
        return "Every deck switch the venue made this night, with what caused it. Click to show or hide them";
    for (size_t i = 0; i < cues.size(); ++i) {
        if (removeArea(i).contains(point))
            return "Remove the cue";
        if (toggleArea(i).contains(point))
            return cues[i].enabled ? "Switch the cue off; it does not fire until switched on"
                                   : "Switch the cue on";
        if (nextArea(i).contains(point))
            return cues[i].at ? "The time of day it fires on the venue's clock, on the next bar after it"
                              : "When it fires next on the beat clock; a Section's cues wait until that Section is marked";
        if (rowArea(i).contains(point))
            return describe(session_.composition(), cues[i]) +
                   (cues[i].gig.empty() ? ". Click to edit it in place" : ". Click to edit its Gig");
    }
    return {};
}

void CuesView::mouseUp(const juce::MouseEvent& e)
{
    if (showsGig() || !listArea().contains(e.getPosition()))
        return;
    const auto& cues = session_.composition().cues;
    for (const Slot& slot : slots())
        if (slot.header() && gigArea(slot.gig).contains(e.getPosition()))
            return openGig(slot.gig);
    if (switchesArea().contains(e.getPosition())) {
        switchesOpen_ = !switchesOpen_;
        resized();
        rows_.repaint();
        return;
    }
    for (size_t i = 0; i < cues.size(); ++i) {
        if (removeArea(i).contains(e.getPosition()))
            return session_.client().edit(RemoveCue{cues[i].id});
        if (toggleArea(i).contains(e.getPosition()))
            return session_.client().edit(SetCueEnabled{cues[i].id, !cues[i].enabled});
        if (rowArea(i).contains(e.getPosition()) && !cues[i].gig.empty())
            return openGig(cues[i].gig);
        if (rowArea(i).contains(e.getPosition())) {
            if (draft_ && draft_->id == cues[i].id)
                return close();
            return open(cues[i].id);
        }
    }
}

} // namespace azl::ui
