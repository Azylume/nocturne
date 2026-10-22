#include "document/composition.hpp"
#include "document/json_fields.hpp"

#include "document/cue_edits.hpp"
#include "document/edit_support.hpp"
#include "document/id_pool.hpp"
#include "document/mapping_edits.hpp"
#include "document/modulation_edits.hpp"
#include "document/palette.hpp"
#include "document/text_edits.hpp"

#include <array>
#include <cstdlib>
#include <set>
#include <utility>

// The file format: the JSON of the document's types.

namespace azl {

using namespace json_fields;

nlohmann::json deckLayersJson(const std::map<uint64_t, DeckLayer>& settings)
{
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [id, s] : settings)
        j[std::to_string(id)] = s;
    return j;
}

std::map<uint64_t, DeckLayer> deckLayersFromJson(const nlohmann::json& j, const char* what)
{
    std::map<uint64_t, DeckLayer> settings;
    for (const auto& [key, s] : j.items()) {
        const uint64_t id = std::strtoull(key.c_str(), nullptr, 10);
        if (id == 0)
            throw ProtocolError("bad " + std::string(what) + " " + key);
        settings[id] = s.get<DeckLayer>();
    }
    return settings;
}

void to_json(nlohmann::json& j, const Transport& transport)
{
    j = {{"sync", nameOf(kSyncNames, transport.sync)},
         {"playMode", nameOf(kPlayModeNames, transport.playMode)},
         {"beats", transport.beats},
         {"jumpInterval", transport.jumpInterval},
         {"jumpDistance", transport.jumpDistance}};
    if (transport.speed != 1.0)
        j["speed"] = transport.speed;
    if (transport.in != 0.0)
        j["in"] = transport.in;
    if (transport.out != 1.0)
        j["out"] = transport.out;
}

void from_json(const nlohmann::json& j, Transport& transport)
{
    transport = transportFromJson(j);
}

void to_json(nlohmann::json& j, const ClipRef& clip)
{
    j = {{"hash", clip.hash}, {"name", clip.name}, {"transport", clip.transport}, {"step", clip.step},
         {"transition", beatsOrNull(clip.transition)}, {"effects", clip.effects}};
    if (clip.text)
        j["text"] = *clip.text;
    if (clip.quantise)
        j["quantise"] = *clip.quantise;
}

void from_json(const nlohmann::json& j, ClipRef& clip)
{
    clip.hash = j.at("hash").get<std::string>();
    if (!clip.hash.empty() && !edits::isContentHash(clip.hash))
        throw ProtocolError("clip hash must be 64 lowercase hex digits (SHA-256)");
    clip.name = j.at("name").get<std::string>();
    clip.transport = transportFromJson(j.value("transport", nlohmann::json()));
    clip.step = j.value("step", 0);
    clip.transition = nullableBeats(j, "transition");
    clip.effects = j.value("effects", std::vector<Effect>{});
    if (j.contains("text") && !j["text"].is_null())
        clip.text = j["text"].get<TextClip>();
    if (j.contains("quantise") && !j["quantise"].is_null())
        clip.quantise = j["quantise"].get<int>();
}

void to_json(nlohmann::json& j, const Effect& effect)
{
    j = {{"id", effect.id}, {"type", effect.type}, {"params", effect.params}};
    if (effect.bypass)
        j["bypass"] = true;
    if (!effect.preset.empty())
        j["preset"] = effect.preset;
    if (!effect.links.empty())
        j["links"] = effect.links;
    if (!effect.steps.empty())
        j["steps"] = effect.steps;
    if (effect.autopilot != Autopilot{})
        j["autopilot"] = {{"mode", nameOf(kAutopilotNames, effect.autopilot.mode)},
                          {"bars", effect.autopilot.bars},
                          {"hold", effect.autopilot.hold}};
    if (effect.transition != kDefaultStepTransition)
        j["transition"] = effect.transition;
    if (effect.playing)
        j["playing"] = {{"step", effect.playing->step},
                        {"beat", effect.playing->beat ? nlohmann::json(*effect.playing->beat) : nullptr}};
}

void from_json(const nlohmann::json& j, Effect& effect)
{
    effect.id = j.value("id", uint64_t(0));
    effect.type = j.at("type").get<std::string>();
    effect.params = j.value("params", std::map<std::string, double>{});
    effect.bypass = j.value("bypass", false);
    effect.preset = j.value("preset", std::string());
    effect.links = j.value("links", std::map<std::string, uint64_t>{});
    effect.steps = j.value("steps", std::vector<EffectStep>{});
    if (auto autopilot = j.find("autopilot"); autopilot != j.end() && autopilot->is_object()) {
        effect.autopilot.mode = autopilotModeField(*autopilot).value_or(effect.autopilot.mode);
        effect.autopilot.bars = autopilot->value("bars", effect.autopilot.bars);
        effect.autopilot.hold = autopilot->value("hold", effect.autopilot.hold);
        checkAutopilotBars(effect.autopilot.bars, effect.autopilot.hold);
    }
    effect.transition = j.value("transition", kDefaultStepTransition);
    if (auto playing = j.find("playing"); playing != j.end() && playing->is_object())
        effect.playing = StepPlay{field<size_t>(*playing, "step"), nullableBeats(*playing, "beat")};
}

void to_json(nlohmann::json& j, const EffectStep& step)
{
    j = {{"params", step.params}};
    if (!step.name.empty())
        j["name"] = step.name;
}

void from_json(const nlohmann::json& j, EffectStep& step)
{
    if (!j.is_object())
        throw ProtocolError("a step is an object");
    step.name = j.value("name", std::string());
    step.params = j.value("params", std::map<std::string, double>{});
}

void to_json(nlohmann::json& j, const Deck& deck)
{
    nlohmann::json columns = nlohmann::json::array();
    for (const Column& column : deck.columns)
        columns.push_back({{"id", column.id}, {"name", column.name}});
    j = {{"id", deck.id}, {"name", deck.name}, {"columns", columns}};
    if (deck.palette)
        j["palette"] = deck.palette;
}

void from_json(const nlohmann::json& j, Deck& deck)
{
    deck = Deck{j.value("id", uint64_t(0)), j.value("name", std::string()), {}, j.value("palette", uint64_t(0))};
    for (const nlohmann::json& column : j.value("columns", nlohmann::json::array()))
        deck.columns.push_back({column.value("id", uint64_t(0)), column.value("name", std::string())});
}

// "clip" repeats the clip the layer plays for clients reading the layer alone; it is not read back.
void to_json(nlohmann::json& j, const Layer& layer)
{
    nlohmann::json clips = nlohmann::json::object();
    for (const auto& [column, clip] : layer.clips)
        clips[std::to_string(column)] = clip;
    nlohmann::json playing = nullptr;
    if (layer.playing)
        playing = {{"column", layer.playing->column},
                   {"beat", layer.playing->beat ? nlohmann::json(*layer.playing->beat) : nullptr}};
    const ClipRef* clip = playingClip(layer);
    j = {{"id", layer.id},
         {"name", layer.name},
         {"opacity", layer.opacity},
         {"blend", blendModeName(layer.blend)},
         {"bypass", layer.bypass},
         {"solo", layer.solo},
         {"clip", clip ? nlohmann::json(*clip) : nullptr},
         {"clips", clips},
         {"playing", playing},
         {"armed", layer.armed},
         {"autopilot", {{"mode", nameOf(kAutopilotNames, layer.autopilot.mode)}, {"bars", layer.autopilot.bars}}},
         {"transition", {{"mode", transitionName(layer.transition.mode)}, {"beats", layer.transition.beats}}},
         {"effects", layer.effects}};
    if (layer.autopilot.hold != Autopilot{}.hold)
        j["autopilot"]["hold"] = layer.autopilot.hold;
    if (!layer.transitionParams.empty())
        j["transition"]["params"] = layer.transitionParams;
    if (!layer.decks.empty())
        j["decks"] = deckLayersJson(layer.decks);
    if (layer.fullResolution)
        j["fullResolution"] = true;
    if (layer.group) {
        j["group"] = true;
        j["outputOnly"] = layer.outputOnly;
        j["layers"] = layer.layers;
    }
}

std::map<uint64_t, ClipRef> clipsFromJson(const nlohmann::json& j)
{
    std::map<uint64_t, ClipRef> clips;
    for (const auto& [column, clip] : j.items()) {
        const uint64_t id = std::strtoull(column.c_str(), nullptr, 10);
        if (id == 0)
            throw ProtocolError("bad column " + column + " in clips");
        clips[id] = clip.get<ClipRef>();
    }
    return clips;
}

std::optional<Play> playFromJson(const nlohmann::json& j)
{
    if (!j.is_object())
        return std::nullopt;
    Play play{j.at("column").get<uint64_t>(), std::nullopt};
    if (auto beat = j.find("beat"); beat != j.end() && !beat->is_null())
        play.beat = beat->get<double>();
    return play;
}

// Missing fields take their defaults, so a client can add a layer by giving only what it cares about.
void from_json(const nlohmann::json& j, Layer& layer)
{
    layer = Layer{};
    layer.id = j.value("id", uint64_t(0));
    layer.name = j.value("name", std::string());
    layer.opacity = j.value("opacity", 1.0);
    layer.blend = blendModeFromName(j.value("blend", std::string("normal")));
    layer.bypass = j.value("bypass", false);
    layer.solo = j.value("solo", false);
    layer.effects = j.value("effects", std::vector<Effect>{});
    layer.group = j.value("group", false);
    layer.outputOnly = layer.group && j.value("outputOnly", false);
    layer.fullResolution = j.value("fullResolution", false);
    layer.layers = j.value("layers", std::vector<Layer>{});
    layer.clips = clipsFromJson(j.value("clips", nlohmann::json::object()));
    layer.playing = playFromJson(j.value("playing", nlohmann::json()));
    layer.armed = j.value("armed", std::set<uint64_t>{});
    if (auto autopilot = j.find("autopilot"); autopilot != j.end() && autopilot->is_object()) {
        layer.autopilot.mode = autopilotModeField(*autopilot).value_or(layer.autopilot.mode);
        layer.autopilot.bars = autopilot->value("bars", layer.autopilot.bars);
        layer.autopilot.hold = autopilot->value("hold", layer.autopilot.hold);
    }
    if (auto transition = j.find("transition"); transition != j.end() && transition->is_object()) {
        layer.transition.mode = transitionModeField(*transition).value_or(layer.transition.mode);
        layer.transition.beats = transition->value("beats", layer.transition.beats);
        layer.transitionParams =
            transition->value("params", std::map<std::string, std::map<std::string, double>>{});
    }
    layer.decks = deckLayersFromJson(j.value("decks", nlohmann::json::object()), "deck");
}

void to_json(nlohmann::json& j, const DeckLayer& settings)
{
    j = nlohmann::json::object();
    if (settings.effects)
        j["effects"] = *settings.effects;
    if (settings.blend)
        j["blend"] = blendModeName(*settings.blend);
    if (settings.transitionMode)
        j["transition"]["mode"] = transitionName(*settings.transitionMode);
    if (settings.transitionBeats)
        j["transition"]["beats"] = *settings.transitionBeats;
    if (settings.autopilotMode)
        j["autopilot"]["mode"] = nameOf(kAutopilotNames, *settings.autopilotMode);
    if (settings.autopilotBars)
        j["autopilot"]["bars"] = *settings.autopilotBars;
}

void from_json(const nlohmann::json& j, DeckLayer& settings)
{
    if (!j.is_object())
        throw ProtocolError("deck settings are not an object");
    settings = DeckLayer{};
    if (j.contains("effects"))
        settings.effects = j["effects"].get<std::vector<Effect>>();
    if (j.contains("blend"))
        settings.blend = blendModeFromName(j["blend"].get<std::string>());
    if (auto transition = j.find("transition"); transition != j.end() && transition->is_object()) {
        settings.transitionMode = transitionModeField(*transition);
        settings.transitionBeats = optionalField<double>(*transition, "beats");
    }
    if (auto autopilot = j.find("autopilot"); autopilot != j.end() && autopilot->is_object()) {
        settings.autopilotMode = autopilotModeField(*autopilot);
        settings.autopilotBars = optionalField<int>(*autopilot, "bars");
    }
}

void to_json(nlohmann::json& j, const Slice& slice)
{
    j = {{"id", slice.id},
         {"name", slice.name},
         {"input", slice.input},
         {"inputRect", slice.inputRect},
         {"corners", slice.corners},
         {"warp", slice.warp},
         {"softEdge", slice.softEdge},
         {"color", slice.color},
         {"masks", slice.masks}};
}

// Missing fields take their defaults: {} is a full slice of the composition.
void from_json(const nlohmann::json& j, Slice& slice)
{
    slice = Slice{};
    slice.id = j.value("id", uint64_t(0));
    slice.name = j.value("name", std::string());
    slice.input = j.value("input", uint64_t(0));
    slice.inputRect = j.value("inputRect", slice.inputRect);
    slice.corners = j.value("corners", slice.corners);
    slice.warp = j.value("warp", slice.warp);
    slice.softEdge = j.value("softEdge", slice.softEdge);
    slice.color = j.value("color", slice.color);
    slice.masks = j.value("masks", slice.masks);
}

void to_json(nlohmann::json& j, const Screen& screen)
{
    j = {{"id", screen.id},
         {"name", screen.name},
         {"display", screen.display},
         {"testCard", screen.testCard},
         {"slices", screen.slices},
         {"color", screen.color},
         {"masks", screen.masks}};
    if (screen.mapPosition)
        j["mapPosition"] = *screen.mapPosition;
    if (screen.hidden)
        j["hidden"] = true;
}

void from_json(const nlohmann::json& j, Screen& screen)
{
    screen = Screen{};
    screen.id = j.value("id", uint64_t(0));
    screen.name = j.value("name", std::string());
    screen.display = j.value("display", std::string());
    screen.testCard = j.value("testCard", false);
    screen.slices = j.value("slices", std::vector<Slice>{});
    screen.color = j.value("color", screen.color);
    screen.masks = j.value("masks", screen.masks);
    screen.mapPosition = positionField(j, "mapPosition");
    screen.hidden = j.value("hidden", false);
}

void to_json(nlohmann::json& j, const Composition& composition)
{
    j = {{"layers", composition.layers}, {"bpm", composition.bpm},       {"decks", composition.decks},
         {"deck", composition.deck},     {"column", composition.column}, {"quantise", composition.quantise},
         {"screens", composition.screens}, {"testCard", composition.testCard}, {"guides", composition.guides},
         {"macros", composition.macros},   {"modulators", composition.modulators}, {"effects", composition.effects}};
    if (!composition.cues.empty())
        j["cues"] = composition.cues;
    if (composition.timeline != Timeline{})
        j["timeline"] = composition.timeline;
    if (!composition.gigs.empty())
        j["gigs"] = composition.gigs;
    if (!composition.sensitivity.empty())
        j["sensitivity"] = composition.sensitivity;
    if (composition.bandEdges != kDefaultBandEdges)
        j["bandEdges"] = composition.bandEdges;
    if (composition.opacity != 1.0)
        j["opacity"] = composition.opacity;
    if (composition.pace != 1.0)
        j["pace"] = composition.pace;
    if (composition.columnAutopilot != Autopilot{})
        j["columnAutopilot"] = {{"mode", nameOf(kAutopilotNames, composition.columnAutopilot.mode)},
                                {"bars", composition.columnAutopilot.bars},
                                {"hold", composition.columnAutopilot.hold}};
    if (!composition.frameGuard)
        j["frameGuard"] = false;
    if (!composition.palettes.empty())
        j["palettes"] = composition.palettes;
    if (composition.palette)
        j["palette"] = composition.palette;
    if (composition.palettePilot != Autopilot{})
        j["palettePilot"] = {{"mode", nameOf(kAutopilotNames, composition.palettePilot.mode)},
                             {"bars", composition.palettePilot.bars},
                             {"hold", composition.palettePilot.hold}};
    if (composition.paletteTransition != kDefaultStepTransition)
        j["paletteTransition"] = composition.paletteTransition;
    if (composition.palettePlaying)
        j["palettePlaying"] = {{"step", composition.palettePlaying->step},
                               {"beat", composition.palettePlaying->beat ? nlohmann::json(*composition.palettePlaying->beat)
                                                                          : nullptr}};
}

namespace {

// Gives every effect an id unique across the document: the first effect with an id keeps it, a repeat or a zero id
// gets a fresh one. Targets saved as layer and effect then name the first effect with that id on that layer.
void uniqueEffectIds(Composition& composition, nlohmann::json& macros, nlohmann::json& modulators)
{
    IdPool ids;
    forEachChain(composition, [&](const std::vector<Effect>& effects, uint64_t, std::optional<uint64_t>) {
        for (const Effect& effect : effects)
            ids.reserve(effect.id);
    });
    std::set<uint64_t> seen;
    std::map<std::pair<uint64_t, uint64_t>, uint64_t> renamed;
    forEachChain(composition, [&](std::vector<Effect>& effects, uint64_t layer, std::optional<uint64_t>) {
        for (Effect& effect : effects) {
            const uint64_t old = effect.id;
            if (old == 0 || !seen.insert(old).second)
                effect.id = ids.take(0, "effect");
            renamed.emplace(std::pair(layer, old), effect.id);
        }
    });
    auto retarget = [&](nlohmann::json& ref) {
        if (!ref.is_object() || !ref.contains("layer") || ref.value("effect", uint64_t(0)) == 0)
            return;
        auto it = renamed.find({ref["layer"].get<uint64_t>(), ref["effect"].get<uint64_t>()});
        if (it != renamed.end())
            ref["effect"] = it->second;
    };
    for (nlohmann::json& macro : macros)
        if (macro.is_object() && macro.contains("targets"))
            for (nlohmann::json& target : macro["targets"])
                retarget(target);
    for (nlohmann::json& modulator : modulators)
        if (modulator.is_object() && modulator.contains("target"))
            retarget(modulator["target"]);
}

} // namespace

void from_json(const nlohmann::json& j, Composition& composition)
{
    const Composition defaults;
    composition.layers = j.at("layers").get<std::vector<Layer>>();
    composition.bpm = j.value("bpm", defaults.bpm);
    composition.decks = j.value("decks", defaults.decks);
    composition.deck = j.value("deck", defaults.deck);
    composition.column = j.value("column", defaults.column);
    composition.quantise = j.value("quantise", defaults.quantise);
    composition.cues = j.value("cues", defaults.cues);
    composition.timeline = j.value("timeline", defaults.timeline);
    composition.gigs = j.value("gigs", defaults.gigs);
    composition.screens = j.value("screens", defaults.screens);
    composition.testCard = j.value("testCard", defaults.testCard);
    composition.guides = j.value("guides", defaults.guides);
    composition.effects = j.value("effects", defaults.effects);
    nlohmann::json macros = j.value("macros", nlohmann::json::array());
    nlohmann::json modulators = j.value("modulators", nlohmann::json::array());
    uniqueEffectIds(composition, macros, modulators);
    composition.macros = macros.get<std::vector<Macro>>();
    composition.modulators = modulators.get<std::vector<Modulator>>();
    composition.sensitivity = j.value("sensitivity", defaults.sensitivity);
    composition.bandEdges = j.value("bandEdges", defaults.bandEdges);
    if (!validBandEdges(composition.bandEdges))
        composition.bandEdges = defaults.bandEdges;
    composition.opacity = j.value("opacity", defaults.opacity);
    composition.pace = j.value("pace", defaults.pace);
    if (auto autopilot = j.find("columnAutopilot"); autopilot != j.end()) {
        Autopilot& column = composition.columnAutopilot;
        column.mode = autopilotModeField(*autopilot).value_or(column.mode);
        column.bars = autopilot->value("bars", column.bars);
        column.hold = autopilot->value("hold", column.hold);
        checkAutopilotBars(column.bars, column.hold);
    }
    composition.frameGuard = j.value("frameGuard", defaults.frameGuard);
    composition.palettes = j.value("palettes", defaults.palettes);
    composition.palette = j.value("palette", defaults.palette);
    if (auto autopilot = j.find("palettePilot"); autopilot != j.end() && autopilot->is_object()) {
        Autopilot& pilot = composition.palettePilot;
        pilot.mode = autopilotModeField(*autopilot).value_or(pilot.mode);
        pilot.bars = autopilot->value("bars", pilot.bars);
        pilot.hold = autopilot->value("hold", pilot.hold);
        checkAutopilotBars(pilot.bars, pilot.hold);
    }
    composition.paletteTransition = j.value("paletteTransition", defaults.paletteTransition);
    if (auto playing = j.find("palettePlaying"); playing != j.end() && playing->is_object())
        composition.palettePlaying = StepPlay{field<size_t>(*playing, "step"), nullableBeats(*playing, "beat")};
}

} // namespace azl
