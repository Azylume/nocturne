#include "document/cue_edits.hpp"

#include "document/modulation_edits.hpp"
#include "document/wall_clock.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

namespace azl {

namespace {

constexpr double kBar = 4.0;
constexpr double kSameBeat = 1e-6;

constexpr std::array<const char*, 5> kSectionNames = {"intro", "build", "drop", "breakdown", "outro"};
constexpr std::array<const char*, 3> kModeNames = {"off", "next", "random"};

template <size_t N>
size_t indexOf(const std::array<const char*, N>& names, const std::string& name, const char* what)
{
    for (size_t i = 0; i < N; ++i)
        if (name == names[i])
            return i;
    throw ProtocolError(std::string("unknown ") + what + " " + name);
}

template <typename T>
T field(const nlohmann::json& j, const char* key)
{
    auto it = j.find(key);
    if (it == j.end())
        throw ProtocolError(std::string("missing field ") + key);
    try {
        return it->get<T>();
    } catch (const nlohmann::json::exception&) {
        throw ProtocolError(std::string("bad field ") + key);
    }
}

std::optional<Play> playFromJson(const nlohmann::json& j)
{
    if (!j.is_object())
        return std::nullopt;
    Play play{field<uint64_t>(j, "column"), std::nullopt};
    if (auto beat = j.find("beat"); beat != j.end() && !beat->is_null())
        play.beat = field<double>(j, "beat");
    return play;
}

nlohmann::json playJson(const std::optional<Play>& play)
{
    if (!play)
        return nullptr;
    return {{"column", play->column}, {"beat", play->beat ? nlohmann::json(*play->beat) : nullptr}};
}

std::vector<Cue>::iterator findCue(Composition& composition, uint64_t id)
{
    auto it = std::find_if(composition.cues.begin(), composition.cues.end(), [&](const Cue& c) { return c.id == id; });
    if (it == composition.cues.end())
        throw ProtocolError("no cue " + std::to_string(id));
    return it;
}

void checkEvery(const std::optional<int>& every)
{
    if (every && (*every < 1 || *every > kMaxCueBar))
        throw ProtocolError("a cue repeats every 1.." + std::to_string(kMaxCueBar) + " bars");
}

// Only a time of day or a hand switches the deck shown; no musical Cue does.
void checkDeckAtClock(const Cue& cue)
{
    if (cue.deck && !cue.at)
        throw ProtocolError("a cue switches decks only at a time of day");
}

void checkAt(const Composition& composition, const Cue& cue)
{
    if (cue.deck && std::none_of(composition.decks.begin(), composition.decks.end(),
                                 [&](const Deck& d) { return d.id == *cue.deck; }))
        throw ProtocolError("no deck " + std::to_string(*cue.deck));
    checkDeckAtClock(cue);
    if (!cue.at)
        return;
    if (cue.at->minute < 0 || cue.at->minute >= 24 * 60)
        throw ProtocolError("a cue's time of day must be within 00:00..23:59");
    if (cue.at->date && !dateDays(*cue.at->date))
        throw ProtocolError("a cue's date reads YYYY-MM-DD");
    if (cue.section || cue.every || !cue.autopilot.empty() || !cue.macros.empty() || !cue.effects.empty())
        throw ProtocolError("a cue at a time of day only switches decks and fires columns");
}

void checkCue(const Composition& composition, const Cue& cue)
{
    if (cue.prepared) {
        if (!cue.prepared->is_array())
            throw ProtocolError("a cue's prepared change is a list of edits");
        if (cue.section || cue.every || cue.at || cue.deck || cue.column || !cue.autopilot.empty() ||
            !cue.macros.empty() || !cue.effects.empty())
            throw ProtocolError("a cue with a prepared change carries nothing else");
    }
    checkAt(composition, cue);
    if (cue.bar < 1 || cue.bar > kMaxCueBar)
        throw ProtocolError("a cue's bar must be within 1.." + std::to_string(kMaxCueBar));
    checkEvery(cue.every);
    std::set<uint64_t> layers;
    for (const CueAutopilot& a : cue.autopilot) {
        const Layer* layer = layerById(composition, a.layer);
        if (!layer || layer->group)
            throw ProtocolError("no layer " + std::to_string(a.layer) + " with Autopilot");
        if (!layers.insert(a.layer).second)
            throw ProtocolError("a cue sets layer " + std::to_string(a.layer) + "'s Autopilot once");
        if (!a.mode && !a.bars)
            throw ProtocolError("a cue's Autopilot sets a mode, bars or both");
        if (a.bars && (*a.bars < 1 || *a.bars > kMaxStepBars))
            throw ProtocolError("Autopilot bars must be within 1.." + std::to_string(kMaxStepBars));
    }
    std::set<uint64_t> macros;
    for (const CueMacro& m : cue.macros) {
        if (std::none_of(composition.macros.begin(), composition.macros.end(),
                         [&](const Macro& macro) { return macro.id == m.macro; }))
            throw ProtocolError("no macro " + std::to_string(m.macro));
        if (!macros.insert(m.macro).second)
            throw ProtocolError("a cue sets macro " + std::to_string(m.macro) + " once");
        if (!(m.value >= 0.0 && m.value <= 1.0))
            throw ProtocolError("a macro's value must be within 0..1");
        if (m.bars < 0 || m.bars > kMaxCueRampBars)
            throw ProtocolError("a macro ramp lasts 0.." + std::to_string(kMaxCueRampBars) + " bars");
    }
    std::set<uint64_t> effects;
    for (const CueEffect& e : cue.effects) {
        if (e.effect == kPaletteEffect && composition.palettes.empty())
            throw ProtocolError("no palette to switch");
        if (e.effect != kPaletteEffect && !effectById(composition, e.effect))
            throw ProtocolError("no effect " + std::to_string(e.effect));
        if (!effects.insert(e.effect).second)
            throw ProtocolError("a cue sets effect " + std::to_string(e.effect) + " once");
        if (!e.mode && !e.bars && !e.step)
            throw ProtocolError("a cue's effect entry sets a mode, bars, a step or several");
        checkAutopilotBars(e.bars, std::nullopt);
        if (e.step && *e.step >= kMaxEffectSteps)
            throw ProtocolError("an effect holds at most " + std::to_string(kMaxEffectSteps) + " steps");
    }
    if (cue.column) {
        const CueColumn& c = *cue.column;
        if ((c.column != 0) == c.index.has_value())
            throw ProtocolError("a cue fires a column by id or by index in the shown deck, not both");
        if (c.column && std::none_of(composition.decks.begin(), composition.decks.end(), [&](const Deck& d) {
                return std::any_of(d.columns.begin(), d.columns.end(), [&](const Column& col) { return col.id == c.column; });
            }))
            throw ProtocolError("no column " + std::to_string(c.column));
    }
}

void checkTimeline(const Timeline& timeline)
{
    if (timeline.sections.size() + timeline.skips.size() > kMaxTimelineEntries)
        throw ProtocolError("a timeline holds at most " + std::to_string(kMaxTimelineEntries) + " entries");
    for (size_t i = 0; i < timeline.sections.size(); ++i) {
        const double beat = timeline.sections[i].beat;
        if (!std::isfinite(beat) || (i > 0 && beat <= timeline.sections[i - 1].beat + kSameBeat))
            throw ProtocolError("section marks must be on finite beats in increasing order");
    }
    for (const CueSkip& skip : timeline.skips)
        if (!std::isfinite(skip.beat))
            throw ProtocolError("a skipped cue needs a finite beat");
}

// The index of the mark playing at `beat`, or -1 before the first.
ptrdiff_t markAt(const std::vector<SectionMark>& marks, double beat)
{
    auto it = std::upper_bound(marks.begin(), marks.end(), beat + kBeatEpsilon,
                               [](double b, const SectionMark& m) { return b < m.beat; });
    return ptrdiff_t(it - marks.begin()) - 1;
}

struct Window {
    const SectionMark* mark = nullptr;
    double start = -kNever;
    double end = kNever;
};

Window windowOf(const std::vector<SectionMark>& marks, ptrdiff_t index)
{
    Window w;
    if (index >= 0) {
        w.mark = &marks[size_t(index)];
        w.start = w.mark->beat;
    }
    if (size_t(index + 1) < marks.size())
        w.end = marks[size_t(index + 1)].beat;
    return w;
}

bool skipped(const Timeline& timeline, uint64_t cue, double beat)
{
    return std::any_of(timeline.skips.begin(), timeline.skips.end(),
                       [&](const CueSkip& s) { return s.cue == cue && std::abs(s.beat - beat) < kSameBeat; });
}

// The beats a Cue fires on in a window: `first`, then every `period` beats (once when 0), before `end`.
struct Firings {
    double first = 0.0;
    double period = 0.0;
    double end = kNever;
};

std::optional<Firings> firingsIn(const Cue& cue, const Window& w)
{
    if (!cue.enabled || cue.at || cue.prepared)
        return std::nullopt;
    const double period = cue.every ? *cue.every * kBar : 0.0;
    double first = (cue.bar - 1) * kBar;
    if (cue.section) {
        if (!w.mark || w.mark->section != *cue.section)
            return std::nullopt;
        first += w.start;
    } else if (first < w.start - kBeatEpsilon) {
        if (!period)
            return std::nullopt;
        first += std::ceil((w.start - kBeatEpsilon - first) / period) * period;
    }
    if (first >= w.end - kBeatEpsilon)
        return std::nullopt;
    return Firings{first, period, w.end};
}

double firingAt(const Firings& f, double k)
{
    return f.first + k * f.period;
}

// The last firing on or before `beat` that was not skipped.
std::optional<double> lastFiring(const Firings& f, const Timeline& timeline, uint64_t cue, double beat)
{
    if (f.first > beat + kBeatEpsilon)
        return std::nullopt;
    double k = f.period ? std::floor((beat + kBeatEpsilon - f.first) / f.period) : 0.0;
    while (k > 0.0 && firingAt(f, k) >= f.end - kBeatEpsilon)
        --k;
    for (; k >= 0.0; --k)
        if (!skipped(timeline, cue, firingAt(f, k)))
            return firingAt(f, k);
    return std::nullopt;
}

// The first firing after `beat` that is not skipped.
std::optional<double> nextFiring(const Firings& f, const Timeline& timeline, uint64_t cue, double beat)
{
    double k = 0.0;
    if (f.first <= beat + kBeatEpsilon) {
        if (!f.period)
            return std::nullopt;
        k = std::floor((beat + kBeatEpsilon - f.first) / f.period) + 1.0;
    }
    for (; firingAt(f, k) < f.end - kBeatEpsilon; ++k) {
        if (!skipped(timeline, cue, firingAt(f, k)))
            return firingAt(f, k);
        if (!f.period)
            break;
    }
    return std::nullopt;
}

void sortByBeat(std::vector<CueFiring>& firings)
{
    for (size_t i = 1; i < firings.size(); ++i)
        for (size_t j = i; j > 0 && firings[j].beat < firings[j - 1].beat; --j)
            std::swap(firings[j], firings[j - 1]);
}

double macroBase(const Composition& composition, uint64_t id)
{
    for (const Macro& macro : composition.macros)
        if (macro.id == id)
            return macro.value;
    return 0.0;
}

double rampAt(const CueState::Ramp& r, double beat)
{
    if (r.bars <= 0)
        return r.to;
    const double t = std::clamp((beat - r.fired) / (r.bars * kBar), 0.0, 1.0);
    return r.from + (r.to - r.from) * t;
}

void merge(CueState& state, const Composition& composition, const Cue& cue, double fired)
{
    for (const CueAutopilot& a : cue.autopilot) {
        auto it = std::find_if(state.autopilot.begin(), state.autopilot.end(),
                               [&](const CueAutopilot& c) { return c.layer == a.layer; });
        if (it == state.autopilot.end()) {
            state.autopilot.push_back(a);
            continue;
        }
        if (a.mode)
            it->mode = a.mode;
        if (a.bars)
            it->bars = a.bars;
    }
    for (const CueMacro& m : cue.macros) {
        auto it = std::find_if(state.ramps.begin(), state.ramps.end(),
                               [&](const CueState::Ramp& r) { return r.macro == m.macro; });
        const double from = it == state.ramps.end() ? macroBase(composition, m.macro) : rampAt(*it, fired);
        const CueState::Ramp ramp{m.macro, from, m.value, fired, m.bars};
        if (it == state.ramps.end())
            state.ramps.push_back(ramp);
        else
            *it = ramp;
    }
    if (cue.column)
        state.columns.push_back({*cue.column, fired});
    for (const CueEffect& e : cue.effects) {
        if (e.step)
            state.steps.push_back({e.effect, *e.step, fired});
        if (!e.mode && !e.bars)
            continue;
        auto it = std::find_if(state.effects.begin(), state.effects.end(),
                               [&](const CueEffect& c) { return c.effect == e.effect; });
        if (it == state.effects.end()) {
            state.effects.push_back({e.effect, e.mode, e.bars});
            continue;
        }
        it->mode = e.mode ? e.mode : it->mode;
        it->bars = e.bars ? e.bars : it->bars;
    }
}

std::optional<Section> sectionField(const nlohmann::json& j)
{
    if (!j.contains("section") || j["section"].is_null())
        return std::nullopt;
    return sectionFromName(field<std::string>(j, "section"));
}

std::optional<int> everyField(const nlohmann::json& j)
{
    if (!j.contains("every") || j["every"].is_null())
        return std::nullopt;
    return field<int>(j, "every");
}

} // namespace

const char* sectionName(Section section)
{
    return kSectionNames[size_t(section)];
}

Section sectionFromName(const std::string& name)
{
    return Section(indexOf(kSectionNames, name, "section"));
}

Edit apply(Composition& composition, AddCue& edit)
{
    const size_t index = edit.index.value_or(composition.cues.size());
    if (index > composition.cues.size())
        throw ProtocolError("cue index out of range");
    if (composition.cues.size() >= kMaxCues)
        throw ProtocolError("a composition holds at most " + std::to_string(kMaxCues) + " cues");
    Cue cue = edit.cue;
    uint64_t fresh = 1;
    for (const Cue& c : composition.cues) {
        if (cue.id && c.id == cue.id)
            throw ProtocolError("cue id " + std::to_string(cue.id) + " is in use");
        fresh = std::max(fresh, c.id + 1);
    }
    if (!cue.id)
        cue.id = fresh;
    checkCue(composition, cue);
    composition.cues.insert(composition.cues.begin() + ptrdiff_t(index), cue);
    edit.index = index;
    edit.cue = cue;
    return RemoveCue{cue.id};
}

Edit apply(Composition& composition, RemoveCue& edit)
{
    auto it = findCue(composition, edit.cue);
    AddCue inverse{size_t(it - composition.cues.begin()), *it};
    composition.cues.erase(it);
    return inverse;
}

Edit apply(Composition& composition, SetCue& edit)
{
    auto it = findCue(composition, edit.cue.id);
    checkCue(composition, edit.cue);
    SetCue inverse{*it};
    *it = edit.cue;
    return inverse;
}

std::vector<SetCue> dropCueSteps(Composition& composition, const std::set<uint64_t>& effects,
                                 std::vector<std::string>& dropped)
{
    std::vector<SetCue> restore;
    for (Cue& cue : composition.cues) {
        const Cue before = cue;
        std::erase_if(cue.effects, [&](const CueEffect& e) {
            if (!effects.contains(e.effect))
                return false;
            dropped.push_back("cue " + cue.name + "'s step of effect " + std::to_string(e.effect));
            return true;
        });
        if (cue.effects.size() != before.effects.size())
            restore.push_back(SetCue{before});
    }
    return restore;
}

Edit apply(Composition& composition, SetCueEnabled& edit)
{
    auto it = findCue(composition, edit.cue);
    if (edit.enabled)
        checkDeckAtClock(*it);
    SetCueEnabled inverse{it->id, it->enabled};
    it->enabled = edit.enabled;
    return inverse;
}

Edit apply(Composition& composition, SetCueRepeat& edit)
{
    auto it = findCue(composition, edit.cue);
    checkEvery(edit.every);
    SetCueRepeat inverse{it->id, it->every};
    it->every = edit.every;
    return inverse;
}

Edit apply(Composition& composition, SetGigWindow& edit)
{
    if (!dateDays(edit.date))
        throw ProtocolError("a Gig's date reads YYYY-MM-DD");
    std::vector<GigWindow>& gigs = composition.gigs;
    auto it = std::find_if(gigs.begin(), gigs.end(), [&](const GigWindow& w) { return w.date == edit.date; });
    SetGigWindow inverse{edit.date, it == gigs.end() ? std::nullopt : std::optional(*it)};
    if (edit.window) {
        const GigWindow& w = *edit.window;
        if (w.date != edit.date)
            throw ProtocolError("a Gig window is set under its own date");
        if (w.start < 0 || w.start >= 24 * 60 || w.end < 0 || w.end >= 24 * 60 || w.start == w.end)
            throw ProtocolError("a Gig window starts and ends at different times within 00:00..23:59");
        if (it == gigs.end() && gigs.size() >= kMaxGigWindows)
            throw ProtocolError("a venue holds at most " + std::to_string(kMaxGigWindows) + " Gig windows");
    }
    if (it != gigs.end())
        gigs.erase(it);
    if (edit.window)
        gigs.insert(std::upper_bound(gigs.begin(), gigs.end(), edit.window->date,
                                     [](const std::string& date, const GigWindow& w) { return date < w.date; }),
                    *edit.window);
    return inverse;
}

Edit apply(Composition& composition, SetTimeline& edit)
{
    checkTimeline(edit.timeline);
    composition.timeline = edit.timeline;
    return edit;
}

Edit apply(Composition& composition, CancelTrigger& edit)
{
    std::vector<Layer*> layers;
    for (const CancelTrigger::Restore& restore : edit.layers) {
        Layer* layer = nullptr;
        forEachLayer(composition.layers, [&](Layer& l) {
            if (l.id == restore.layer && !l.group)
                layer = &l;
        });
        if (!layer)
            throw ProtocolError("no layer " + std::to_string(restore.layer));
        layers.push_back(layer);
    }
    for (size_t i = 0; i < layers.size(); ++i) {
        const std::optional<Play>& playing = layers[i]->playing;
        if (playing && playing->beat && *playing->beat > edit.beat + kBeatEpsilon)
            layers[i]->playing = edit.layers[i].playing;
    }
    return edit;
}

nlohmann::json toJson(const AddCue& e)
{
    return {{"op", "addCue"}, {"index", e.index ? nlohmann::json(*e.index) : nullptr}, {"cue", e.cue}};
}

nlohmann::json toJson(const RemoveCue& e)
{
    return {{"op", "removeCue"}, {"cue", e.cue}};
}

nlohmann::json toJson(const SetCue& e)
{
    return {{"op", "setCue"}, {"cue", e.cue}};
}

nlohmann::json toJson(const SetCueEnabled& e)
{
    return {{"op", "setCueEnabled"}, {"cue", e.cue}, {"enabled", e.enabled}};
}

nlohmann::json toJson(const SetCueRepeat& e)
{
    return {{"op", "setCueRepeat"}, {"cue", e.cue}, {"every", e.every ? nlohmann::json(*e.every) : nullptr}};
}

nlohmann::json toJson(const SetGigWindow& e)
{
    return {{"op", "setGigWindow"}, {"date", e.date}, {"window", e.window ? nlohmann::json(*e.window) : nullptr}};
}

nlohmann::json toJson(const SetTimeline& e)
{
    return {{"op", "setTimeline"}, {"timeline", e.timeline}};
}

nlohmann::json toJson(const CancelTrigger& e)
{
    nlohmann::json layers = nlohmann::json::array();
    for (const CancelTrigger::Restore& r : e.layers)
        layers.push_back({{"layer", r.layer}, {"playing", playJson(r.playing)}});
    return {{"op", "cancelTrigger"}, {"beat", e.beat}, {"layers", layers}};
}

std::optional<Edit> cueEditFromJson(const std::string& op, const nlohmann::json& j)
{
    if (op == "addCue") {
        AddCue edit{std::nullopt, field<Cue>(j, "cue")};
        if (j.contains("index") && !j["index"].is_null())
            edit.index = field<size_t>(j, "index");
        return edit;
    }
    if (op == "removeCue")
        return RemoveCue{field<uint64_t>(j, "cue")};
    if (op == "setCue")
        return SetCue{field<Cue>(j, "cue")};
    if (op == "setCueEnabled")
        return SetCueEnabled{field<uint64_t>(j, "cue"), field<bool>(j, "enabled")};
    if (op == "setCueRepeat")
        return SetCueRepeat{field<uint64_t>(j, "cue"), everyField(j)};
    if (op == "setGigWindow") {
        SetGigWindow edit{field<std::string>(j, "date")};
        if (j.contains("window") && !j["window"].is_null())
            edit.window = field<GigWindow>(j, "window");
        return edit;
    }
    if (op == "setTimeline")
        return SetTimeline{field<Timeline>(j, "timeline")};
    if (op == "cancelTrigger") {
        CancelTrigger edit{field<double>(j, "beat"), {}};
        for (const nlohmann::json& r : field<nlohmann::json>(j, "layers"))
            edit.layers.push_back({field<uint64_t>(r, "layer"), playFromJson(r.value("playing", nlohmann::json()))});
        return edit;
    }
    return std::nullopt;
}

void to_json(nlohmann::json& j, const Cue& cue)
{
    nlohmann::json autopilot = nlohmann::json::array();
    for (const CueAutopilot& a : cue.autopilot) {
        nlohmann::json entry{{"layer", a.layer}};
        if (a.mode)
            entry["mode"] = kModeNames[size_t(*a.mode)];
        if (a.bars)
            entry["bars"] = *a.bars;
        autopilot.push_back(entry);
    }
    nlohmann::json macros = nlohmann::json::array();
    for (const CueMacro& m : cue.macros) {
        nlohmann::json entry{{"macro", m.macro}, {"value", m.value}};
        if (m.bars)
            entry["bars"] = m.bars;
        macros.push_back(entry);
    }
    j = {{"id", cue.id},
         {"name", cue.name},
         {"section", cue.section ? nlohmann::json(sectionName(*cue.section)) : nullptr},
         {"bar", cue.bar},
         {"autopilot", autopilot},
         {"macros", macros}};
    if (cue.column) {
        nlohmann::json column = nlohmann::json::object();
        if (cue.column->column)
            column["column"] = cue.column->column;
        if (cue.column->index)
            column["index"] = *cue.column->index;
        j["column"] = column;
    }
    if (!cue.enabled)
        j["enabled"] = false;
    if (cue.every)
        j["every"] = *cue.every;
    if (cue.at) {
        j["at"] = {{"time", clockText(cue.at->minute)}};
        if (cue.at->date)
            j["at"]["date"] = *cue.at->date;
    }
    if (cue.deck)
        j["deck"] = *cue.deck;
    if (!cue.gig.empty())
        j["gig"] = cue.gig;
    if (cue.prepared)
        j["prepared"] = *cue.prepared;
    if (cue.effects.empty())
        return;
    nlohmann::json effects = nlohmann::json::array();
    for (const CueEffect& e : cue.effects) {
        nlohmann::json entry{{"effect", e.effect}};
        if (e.mode)
            entry["mode"] = kModeNames[size_t(*e.mode)];
        if (e.bars)
            entry["bars"] = *e.bars;
        if (e.step)
            entry["step"] = *e.step;
        effects.push_back(entry);
    }
    j["effects"] = effects;
}

// Everything but the position is optional: {"section": "drop"} is a Cue on every drop that changes nothing.
void from_json(const nlohmann::json& j, Cue& cue)
{
    if (!j.is_object())
        throw ProtocolError("a cue is an object");
    cue = Cue{};
    cue.id = j.contains("id") ? field<uint64_t>(j, "id") : 0;
    cue.name = j.contains("name") ? field<std::string>(j, "name") : "";
    cue.section = sectionField(j);
    cue.bar = j.contains("bar") ? field<int>(j, "bar") : 1;
    for (const nlohmann::json& a : j.value("autopilot", nlohmann::json::array())) {
        CueAutopilot entry{field<uint64_t>(a, "layer"), std::nullopt, std::nullopt};
        if (a.contains("mode"))
            entry.mode = AutopilotMode(indexOf(kModeNames, field<std::string>(a, "mode"), "autopilot mode"));
        if (a.contains("bars"))
            entry.bars = field<int>(a, "bars");
        cue.autopilot.push_back(entry);
    }
    for (const nlohmann::json& m : j.value("macros", nlohmann::json::array()))
        cue.macros.push_back({field<uint64_t>(m, "macro"), field<double>(m, "value"),
                              m.contains("bars") ? field<int>(m, "bars") : 0});
    if (auto c = j.find("column"); c != j.end() && !c->is_null()) {
        if (!c->is_object())
            throw ProtocolError("bad field column");
        CueColumn column;
        if (c->contains("column"))
            column.column = field<uint64_t>(*c, "column");
        if (c->contains("index"))
            column.index = field<size_t>(*c, "index");
        cue.column = column;
    }
    cue.enabled = j.contains("enabled") ? field<bool>(j, "enabled") : true;
    cue.every = everyField(j);
    if (auto at = j.find("at"); at != j.end() && !at->is_null()) {
        const std::optional<int> minute = clockMinutes(field<std::string>(*at, "time"));
        if (!minute)
            throw ProtocolError("a cue's time of day reads HH:MM");
        cue.at = CueAt{*minute};
        if (at->contains("date"))
            cue.at->date = field<std::string>(*at, "date");
    }
    if (j.contains("deck") && !j["deck"].is_null())
        cue.deck = field<uint64_t>(j, "deck");
    cue.gig = j.contains("gig") ? field<std::string>(j, "gig") : "";
    if (auto prepared = j.find("prepared"); prepared != j.end() && !prepared->is_null())
        cue.prepared = *prepared;
    for (const nlohmann::json& e : j.value("effects", nlohmann::json::array())) {
        CueEffect entry{field<uint64_t>(e, "effect"), std::nullopt, std::nullopt};
        if (e.contains("mode"))
            entry.mode = AutopilotMode(indexOf(kModeNames, field<std::string>(e, "mode"), "autopilot mode"));
        if (e.contains("bars"))
            entry.bars = field<int>(e, "bars");
        if (e.contains("step"))
            entry.step = field<size_t>(e, "step");
        cue.effects.push_back(entry);
    }
}

void to_json(nlohmann::json& j, const GigWindow& window)
{
    j = {{"date", window.date}, {"start", clockText(window.start)}, {"end", clockText(window.end)}};
}

void from_json(const nlohmann::json& j, GigWindow& window)
{
    if (!j.is_object())
        throw ProtocolError("a Gig window is an object");
    const std::optional<int> start = clockMinutes(field<std::string>(j, "start"));
    const std::optional<int> end = clockMinutes(field<std::string>(j, "end"));
    if (!start || !end)
        throw ProtocolError("a Gig window's start and end read HH:MM");
    window = {field<std::string>(j, "date"), *start, *end};
}

void to_json(nlohmann::json& j, const Timeline& timeline)
{
    nlohmann::json sections = nlohmann::json::array();
    for (const SectionMark& mark : timeline.sections)
        sections.push_back({{"section", sectionName(mark.section)}, {"beat", mark.beat}});
    nlohmann::json skips = nlohmann::json::array();
    for (const CueSkip& skip : timeline.skips)
        skips.push_back({{"cue", skip.cue}, {"beat", skip.beat}});
    j = {{"sections", sections}, {"skips", skips}};
}

void from_json(const nlohmann::json& j, Timeline& timeline)
{
    if (!j.is_object())
        throw ProtocolError("a timeline is an object");
    timeline = Timeline{};
    for (const nlohmann::json& mark : j.value("sections", nlohmann::json::array()))
        timeline.sections.push_back({sectionFromName(field<std::string>(mark, "section")), field<double>(mark, "beat")});
    for (const nlohmann::json& skip : j.value("skips", nlohmann::json::array()))
        timeline.skips.push_back({field<uint64_t>(skip, "cue"), field<double>(skip, "beat")});
}

void cueStateAt(const Composition& composition, double beat, CueState& state)
{
    const std::vector<SectionMark>& marks = composition.timeline.sections;
    const Window w = windowOf(marks, markAt(marks, beat));
    state.section = w.mark ? std::optional(w.mark->section) : std::nullopt;
    state.start = w.start;
    state.end = w.end;
    state.active.clear();
    state.autopilot.clear();
    state.macros.clear();
    state.ramps.clear();
    state.columns.clear();
    state.effects.clear();
    state.steps.clear();
    for (const Cue& cue : composition.cues) {
        const std::optional<Firings> f = firingsIn(cue, w);
        if (const std::optional<double> at = f ? lastFiring(*f, composition.timeline, cue.id, beat) : std::nullopt)
            state.active.push_back({cue.id, *at, w.end});
    }
    sortByBeat(state.active);
    for (const CueFiring& firing : state.active)
        for (const Cue& cue : composition.cues)
            if (cue.id == firing.cue)
                merge(state, composition, cue, firing.beat);
    for (const CueState::Ramp& r : state.ramps)
        state.macros.push_back({r.macro, rampAt(r, beat), 0});
}

std::vector<CueFiring> upcomingCues(const Composition& composition, double beat)
{
    const std::vector<SectionMark>& marks = composition.timeline.sections;
    std::vector<CueFiring> firings;
    for (ptrdiff_t index = markAt(marks, beat); index < ptrdiff_t(marks.size()); ++index) {
        const Window w = windowOf(marks, index);
        for (const Cue& cue : composition.cues) {
            const std::optional<Firings> f = firingsIn(cue, w);
            if (const std::optional<double> at = f ? nextFiring(*f, composition.timeline, cue.id, beat) : std::nullopt)
                firings.push_back({cue.id, *at, w.end});
        }
    }
    sortByBeat(firings);
    return firings;
}

Autopilot cuedAutopilot(const Layer& layer, uint64_t deck, const std::vector<CueAutopilot>& cued)
{
    Autopilot autopilot = deckAutopilot(layer, deck);
    for (const CueAutopilot& a : cued) {
        if (a.layer != layer.id)
            continue;
        autopilot.mode = a.mode.value_or(autopilot.mode);
        autopilot.bars = a.bars.value_or(autopilot.bars);
    }
    return autopilot;
}

Autopilot cuedAutopilot(const Effect& effect, const std::vector<CueEffect>& cued)
{
    Autopilot autopilot = effect.autopilot;
    for (const CueEffect& e : cued) {
        if (e.effect != effect.id)
            continue;
        autopilot.mode = e.mode.value_or(autopilot.mode);
        autopilot.bars = e.bars.value_or(autopilot.bars);
    }
    return autopilot;
}

Timeline markSection(const Timeline& timeline, Section section, double at, double now)
{
    Timeline next = timeline;
    std::vector<SectionMark>& marks = next.sections;
    const ptrdiff_t playing = markAt(marks, now);
    if (playing > 0) {
        const double start = marks[size_t(playing)].beat;
        marks.erase(marks.begin(), marks.begin() + playing);
        std::erase_if(next.skips, [&](const CueSkip& s) { return s.beat < start - kSameBeat; });
    }
    std::erase_if(marks, [&](const SectionMark& m) { return std::abs(m.beat - at) < kSameBeat; });
    auto it = std::find_if(marks.begin(), marks.end(), [&](const SectionMark& m) { return m.beat > at; });
    marks.insert(it, {section, at});
    return next;
}

Timeline rebasedTimeline(const Timeline& timeline, double beat)
{
    const ptrdiff_t playing = markAt(timeline.sections, beat);
    if (playing < 0)
        return {};
    const Window w = windowOf(timeline.sections, playing);
    Timeline next{{{w.mark->section, 0.0}}, {}};
    for (const CueSkip& skip : timeline.skips)
        if (skip.beat >= w.start - kSameBeat && skip.beat < w.end - kSameBeat)
            next.skips.push_back({skip.cue, skip.beat - w.start});
    return next;
}

} // namespace azl
