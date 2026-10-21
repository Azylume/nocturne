#include "document/composition.hpp"
#include "document/json_fields.hpp"

#include "document/cue_edits.hpp"
#include "document/id_pool.hpp"
#include "document/mapping_edits.hpp"
#include "document/modulation_edits.hpp"
#include "document/palette.hpp"
#include "document/text_edits.hpp"

#include <array>
#include <cstdlib>
#include <set>
#include <utility>

// The protocol format of the edits: each edit's JSON and the edit a JSON message holds.

namespace azl {

using namespace json_fields;

nlohmann::json toJson(const SetLayerOpacity& e)
{
    return {{"op", "setLayerOpacity"}, {"layer", e.layer}, {"opacity", e.opacity}};
}

nlohmann::json toJson(const SetLayerClip& e)
{
    return {{"op", "setLayerClip"}, {"layer", e.layer}, {"clip", e.clip ? nlohmann::json(*e.clip) : nullptr}};
}

nlohmann::json toJson(const SetTempo& e)
{
    return {{"op", "setTempo"}, {"bpm", e.bpm}};
}

nlohmann::json toJson(const SetClipTransport& e)
{
    nlohmann::json j{{"op", "setClipTransport"}, {"layer", e.layer}};
    if (e.sync)
        j["sync"] = nameOf(kSyncNames, *e.sync);
    if (e.playMode)
        j["playMode"] = nameOf(kPlayModeNames, *e.playMode);
    if (e.beats)
        j["beats"] = *e.beats;
    if (e.jumpInterval)
        j["jumpInterval"] = *e.jumpInterval;
    if (e.jumpDistance)
        j["jumpDistance"] = *e.jumpDistance;
    if (e.column)
        j["column"] = *e.column;
    if (e.speed)
        j["speed"] = *e.speed;
    if (e.in)
        j["in"] = *e.in;
    if (e.out)
        j["out"] = *e.out;
    return j;
}

nlohmann::json quantiseOrNull(const std::optional<int>& quantise)
{
    return quantise ? nlohmann::json(*quantise) : nlohmann::json(nullptr);
}

nlohmann::json toJson(const SetClipTrigger& e)
{
    return {{"op", "setClipTrigger"}, {"layer", e.layer}, {"column", e.column}, {"quantise", quantiseOrNull(e.quantise)}};
}

nlohmann::json toJson(const StopLayer& e)
{
    return {{"op", "stopLayer"}, {"layer", e.layer}};
}

nlohmann::json optionalJson(const std::optional<ClipRef>& clip)
{
    return clip ? nlohmann::json(*clip) : nlohmann::json(nullptr);
}

nlohmann::json indexJson(const std::optional<size_t>& index)
{
    return index ? nlohmann::json(*index) : nlohmann::json(nullptr);
}

nlohmann::json cellsJson(const std::vector<Cell>& cells)
{
    nlohmann::json j = nlohmann::json::array();
    for (const Cell& cell : cells)
        j.push_back({{"layer", cell.layer}, {"column", cell.column}, {"clip", cell.clip}});
    return j;
}

nlohmann::json toJson(const SetSlot& e)
{
    return {{"op", "setSlot"}, {"layer", e.layer}, {"column", e.column}, {"clip", optionalJson(e.clip)}};
}

nlohmann::json toJson(const AddColumn& e)
{
    return {{"op", "addColumn"},
            {"deck", e.deck},
            {"index", indexJson(e.index)},
            {"column", {{"id", e.column.id}, {"name", e.column.name}}},
            {"cells", cellsJson(e.cells)}};
}

nlohmann::json toJson(const RemoveColumn& e)
{
    return {{"op", "removeColumn"}, {"column", e.column}};
}

nlohmann::json toJson(const AddDeck& e)
{
    nlohmann::json j{{"op", "addDeck"}, {"index", indexJson(e.index)}, {"deck", e.deck}, {"cells", cellsJson(e.cells)}};
    if (!e.layers.empty())
        j["layers"] = deckLayersJson(e.layers);
    return j;
}

nlohmann::json toJson(const SetDeckLayer& e)
{
    return {{"op", "setDeckLayer"}, {"layer", e.layer}, {"deck", e.deck}, {"settings", e.settings}};
}

nlohmann::json toJson(const RemoveDeck& e)
{
    return {{"op", "removeDeck"}, {"deck", e.deck}};
}

nlohmann::json toJson(const MoveDeck& e)
{
    return {{"op", "moveDeck"}, {"deck", e.deck}, {"index", e.index}};
}

nlohmann::json toJson(const SetEffectBypass& e)
{
    return {{"op", "setEffectBypass"}, {"effect", e.effect}, {"bypass", e.bypass}};
}

nlohmann::json toJson(const SetEffectSteps& e)
{
    return {{"op", "setEffectSteps"}, {"effect", e.effect}, {"steps", e.steps}};
}

nlohmann::json toJson(const SetEffectAutopilot& e)
{
    nlohmann::json j{{"op", "setEffectAutopilot"}, {"effect", e.effect}};
    if (e.mode)
        j["mode"] = nameOf(kAutopilotNames, *e.mode);
    if (e.bars)
        j["bars"] = *e.bars;
    if (e.hold)
        j["hold"] = *e.hold;
    if (e.transition)
        j["transition"] = *e.transition;
    return j;
}

nlohmann::json toJson(const TriggerStep& e)
{
    return {{"op", "triggerStep"}, {"effect", e.effect}, {"step", e.step}, {"beat", e.beat}};
}

nlohmann::json toJson(const SetSensitivity& e)
{
    return {{"op", "setSensitivity"}, {"source", e.source}, {"value", e.value}};
}

nlohmann::json toJson(const SetBandEdges& e)
{
    return {{"op", "setBandEdges"}, {"edges", e.edges}};
}

nlohmann::json toJson(const Rename& e)
{
    return {{"op", "rename"}, {"kind", e.kind}, {"id", e.id}, {"name", e.name}};
}

nlohmann::json toJson(const SetAutopilotPace& e)
{
    return {{"op", "setAutopilotPace"}, {"pace", e.pace}};
}

nlohmann::json toJson(const SetColumnAutopilot& e)
{
    nlohmann::json j{{"op", "setColumnAutopilot"}};
    if (e.mode)
        j["mode"] = nameOf(kAutopilotNames, *e.mode);
    if (e.bars)
        j["bars"] = *e.bars;
    if (e.hold)
        j["hold"] = *e.hold;
    return j;
}

nlohmann::json toJson(const SetFrameGuard& e)
{
    return {{"op", "setFrameGuard"}, {"on", e.on}};
}

nlohmann::json toJson(const SetQuantise& e)
{
    return {{"op", "setQuantise"}, {"beats", e.beats}};
}

nlohmann::json toJson(const TriggerClip& e)
{
    return {{"op", "triggerClip"}, {"layer", e.layer}, {"column", e.column}, {"beat", e.beat}};
}

nlohmann::json toJson(const TriggerColumn& e)
{
    return {{"op", "triggerColumn"}, {"column", e.column}, {"beat", e.beat}};
}

nlohmann::json toJson(const SelectDeck& e)
{
    nlohmann::json j{{"op", "selectDeck"}, {"deck", e.deck}};
    if (e.beat)
        j["beat"] = *e.beat;
    return j;
}

nlohmann::json toJson(const ArmClip& e)
{
    return {{"op", "armClip"}, {"layer", e.layer}, {"column", e.column}, {"armed", e.armed}};
}

nlohmann::json toJson(const AddEffect& e)
{
    nlohmann::json j{{"op", "addEffect"},
            {"layer", e.layer},
            {"index", e.index ? nlohmann::json(*e.index) : nullptr},
            {"effect", e.effect},
            {"column", e.column ? nlohmann::json(*e.column) : nullptr}};
    if (e.deck)
        j["deck"] = *e.deck;
    return j;
}

nlohmann::json toJson(const RemoveEffect& e)
{
    return {{"op", "removeEffect"}, {"effect", e.effect}};
}

nlohmann::json toJson(const MoveEffect& e)
{
    nlohmann::json j{{"op", "moveEffect"}, {"effect", e.effect}, {"index", e.index}};
    if (e.layer)
        j["layer"] = *e.layer;
    if (e.column)
        j["column"] = *e.column;
    if (e.deck)
        j["deck"] = *e.deck;
    return j;
}

nlohmann::json toJson(const SetEffectParam& e)
{
    return {{"op", "setEffectParam"}, {"effect", e.effect}, {"param", e.param}, {"value", e.value}};
}

nlohmann::json toJson(const SetEffectParams& e)
{
    nlohmann::json j{{"op", "setEffectParams"}, {"effect", e.effect}, {"params", e.params}};
    if (e.preset)
        j["preset"] = *e.preset;
    return j;
}

nlohmann::json toJson(const SetLayerBlend& e)
{
    return {{"op", "setLayerBlend"}, {"layer", e.layer}, {"blend", blendModeName(e.blend)}};
}

nlohmann::json toJson(const SetLayerBypass& e)
{
    return {{"op", "setLayerBypass"}, {"layer", e.layer}, {"bypass", e.bypass}};
}

nlohmann::json toJson(const SetLayerSolo& e)
{
    return {{"op", "setLayerSolo"}, {"layer", e.layer}, {"solo", e.solo}};
}

nlohmann::json toJson(const SetLayerFullResolution& e)
{
    return {{"op", "setLayerFullResolution"}, {"layer", e.layer}, {"fullResolution", e.fullResolution}};
}

nlohmann::json toJson(const AddLayer& e)
{
    return {{"op", "addLayer"},
            {"parent", e.parent},
            {"index", e.index ? nlohmann::json(*e.index) : nullptr},
            {"layer", e.layer}};
}

nlohmann::json toJson(const RemoveLayer& e)
{
    return {{"op", "removeLayer"}, {"layer", e.layer}};
}

nlohmann::json toJson(const MoveLayer& e)
{
    return {{"op", "moveLayer"}, {"layer", e.layer}, {"parent", e.parent}, {"index", e.index}};
}

nlohmann::json toJson(const SetGroupOutputOnly& e)
{
    return {{"op", "setGroupOutputOnly"}, {"layer", e.layer}, {"outputOnly", e.outputOnly}};
}

nlohmann::json toJson(const AssignDisplay& e)
{
    return {{"op", "assignDisplay"},
            {"group", e.group},
            {"display", e.display},
            {"screen", e.screen ? nlohmann::json(*e.screen) : nullptr}};
}

nlohmann::json toJson(const AddScreen& e)
{
    return {{"op", "addScreen"}, {"index", e.index ? nlohmann::json(*e.index) : nullptr}, {"screen", e.screen}};
}

nlohmann::json toJson(const RemoveScreen& e)
{
    return {{"op", "removeScreen"}, {"screen", e.screen}};
}

nlohmann::json toJson(const SetScreenDisplay& e)
{
    return {{"op", "setScreenDisplay"}, {"screen", e.screen}, {"display", e.display}};
}

nlohmann::json toJson(const SetScreenTestCard& e)
{
    return {{"op", "setScreenTestCard"}, {"screen", e.screen}, {"testCard", e.testCard}};
}

nlohmann::json toJson(const SetScreenPosition& e)
{
    return {{"op", "setScreenPosition"},
            {"screen", e.screen},
            {"position", e.position ? nlohmann::json(*e.position) : nullptr}};
}

nlohmann::json toJson(const SetScreenHidden& e)
{
    return {{"op", "setScreenHidden"}, {"screen", e.screen}, {"hidden", e.hidden}};
}

nlohmann::json toJson(const MoveScreen& e)
{
    return {{"op", "moveScreen"}, {"screen", e.screen}, {"index", e.index}};
}

nlohmann::json toJson(const AddSlice& e)
{
    return {{"op", "addSlice"},
            {"screen", e.screen},
            {"index", e.index ? nlohmann::json(*e.index) : nullptr},
            {"slice", e.slice}};
}

nlohmann::json toJson(const RemoveSlice& e)
{
    return {{"op", "removeSlice"}, {"slice", e.slice}};
}

nlohmann::json toJson(const SetSliceCorners& e)
{
    return {{"op", "setSliceCorners"}, {"slice", e.slice}, {"corners", e.corners}};
}

nlohmann::json toJson(const SetSliceInput& e)
{
    nlohmann::json j{{"op", "setSliceInput"}, {"slice", e.slice}};
    if (e.input)
        j["input"] = *e.input;
    if (e.inputRect)
        j["inputRect"] = *e.inputRect;
    return j;
}

nlohmann::json toJson(const SetLayerAutopilot& e)
{
    nlohmann::json j{{"op", "setLayerAutopilot"}, {"layer", e.layer}};
    if (e.mode)
        j["mode"] = nameOf(kAutopilotNames, *e.mode);
    if (e.bars)
        j["bars"] = *e.bars;
    if (e.hold)
        j["hold"] = *e.hold;
    return j;
}

nlohmann::json toJson(const SetClipStep& e)
{
    return {{"op", "setClipStep"}, {"layer", e.layer}, {"column", e.column}, {"bars", e.bars}};
}

nlohmann::json toJson(const SetLayerTransition& e)
{
    nlohmann::json j{{"op", "setLayerTransition"}, {"layer", e.layer}};
    if (e.mode)
        j["mode"] = transitionName(*e.mode);
    if (e.beats)
        j["beats"] = *e.beats;
    return j;
}

nlohmann::json toJson(const SetTransitionParam& e)
{
    return {{"op", "setTransitionParam"}, {"layer", e.layer}, {"mode", transitionName(e.mode)}, {"name", e.name},
            {"value", e.value ? nlohmann::json(*e.value) : nullptr}};
}

nlohmann::json toJson(const SetClipTransition& e)
{
    return {{"op", "setClipTransition"}, {"layer", e.layer}, {"column", e.column}, {"beats", beatsOrNull(e.beats)}};
}

nlohmann::json toJson(const LoadComposition& e)
{
    return {{"op", "loadComposition"}, {"composition", e.composition}};
}

nlohmann::json toJson(const Batch& e);

nlohmann::json editToJson(const Edit& edit)
{
    return std::visit([](const auto& e) { return toJson(e); }, edit);
}

nlohmann::json toJson(const Batch& e)
{
    nlohmann::json edits = nlohmann::json::array();
    for (const Edit& edit : e.edits)
        edits.push_back(editToJson(edit));
    return {{"op", "batch"}, {"edits", edits}};
}

// The effect needs only a type; id, params and index are optional.
AddEffect addEffectFromJson(const nlohmann::json& j)
{
    const auto effect = field<nlohmann::json>(j, "effect");
    if (!effect.is_object())
        throw ProtocolError("bad field effect");
    AddEffect edit{field<uint64_t>(j, "layer"), std::nullopt, {0, field<std::string>(effect, "type"), {}}};
    if (effect.contains("id"))
        edit.effect.id = field<uint64_t>(effect, "id");
    if (effect.contains("params"))
        edit.effect.params = field<std::map<std::string, double>>(effect, "params");
    if (effect.contains("preset"))
        edit.effect.preset = field<std::string>(effect, "preset");
    if (effect.contains("links"))
        edit.effect.links = field<std::map<std::string, uint64_t>>(effect, "links");
    const Effect stepped = effect.get<Effect>();
    edit.effect.steps = stepped.steps;
    edit.effect.autopilot = stepped.autopilot;
    edit.effect.transition = stepped.transition;
    edit.effect.playing = stepped.playing;
    if (j.contains("index") && !j["index"].is_null())
        edit.index = field<size_t>(j, "index");
    if (j.contains("column") && !j["column"].is_null())
        edit.column = field<uint64_t>(j, "column");
    if (j.contains("deck") && !j["deck"].is_null())
        edit.deck = field<uint64_t>(j, "deck");
    return edit;
}

std::optional<ClipRef> clipField(const nlohmann::json& j)
{
    const auto clip = field<nlohmann::json>(j, "clip");
    if (clip.is_null())
        return std::nullopt;
    ClipRef ref{field<std::string>(clip, "hash"), field<std::string>(clip, "name"),
                transportFromJson(clip.value("transport", nlohmann::json())),
                optionalField<int>(clip, "step").value_or(0), nullableBeats(clip, "transition"),
                optionalField<std::vector<Effect>>(clip, "effects").value_or(std::vector<Effect>{})};
    if (clip.contains("text") && !clip["text"].is_null())
        ref.text = field<TextClip>(clip, "text");
    if (clip.contains("quantise") && !clip["quantise"].is_null())
        ref.quantise = field<int>(clip, "quantise");
    return ref;
}

std::optional<size_t> indexField(const nlohmann::json& j)
{
    if (j.contains("index") && !j["index"].is_null())
        return field<size_t>(j, "index");
    return std::nullopt;
}

std::vector<Cell> cellsField(const nlohmann::json& j)
{
    std::vector<Cell> cells;
    if (!j.contains("cells"))
        return cells;
    const auto list = field<nlohmann::json>(j, "cells");
    if (!list.is_array())
        throw ProtocolError("bad field cells");
    for (const nlohmann::json& cell : list)
        cells.push_back({field<uint64_t>(cell, "layer"), cell.value("column", uint64_t(0)), *clipField(cell)});
    return cells;
}

AddColumn addColumnFromJson(const nlohmann::json& j)
{
    AddColumn edit{field<uint64_t>(j, "deck"), indexField(j), {}, cellsField(j)};
    if (auto column = j.find("column"); column != j.end() && column->is_object()) {
        edit.column.id = column->value("id", uint64_t(0));
        edit.column.name = column->value("name", std::string());
    }
    return edit;
}

// Every field of the layer is optional: {"group": true} adds an empty group.
AddLayer addLayerFromJson(const nlohmann::json& j)
{
    AddLayer edit{field<uint64_t>(j, "parent"), std::nullopt, field<Layer>(j, "layer")};
    if (j.contains("index") && !j["index"].is_null())
        edit.index = field<size_t>(j, "index");
    return edit;
}

std::optional<Screen> screenField(const nlohmann::json& j)
{
    if (j.contains("screen") && !j["screen"].is_null())
        return field<Screen>(j, "screen");
    return std::nullopt;
}

std::optional<Edit> screenEditFromJson(const std::string& op, const nlohmann::json& j)
{
    if (op == "setGroupOutputOnly")
        return SetGroupOutputOnly{field<uint64_t>(j, "layer"), field<bool>(j, "outputOnly")};
    if (op == "assignDisplay")
        return AssignDisplay{field<uint64_t>(j, "group"), field<std::string>(j, "display"), screenField(j)};
    if (op == "addScreen")
        return AddScreen{indexField(j), field<Screen>(j, "screen")};
    if (op == "removeScreen")
        return RemoveScreen{field<uint64_t>(j, "screen")};
    if (op == "setScreenDisplay")
        return SetScreenDisplay{field<uint64_t>(j, "screen"), field<std::string>(j, "display")};
    if (op == "setScreenTestCard")
        return SetScreenTestCard{field<uint64_t>(j, "screen"), field<bool>(j, "testCard")};
    if (op == "setScreenPosition")
        return SetScreenPosition{field<uint64_t>(j, "screen"), positionField(j, "position")};
    if (op == "setScreenHidden")
        return SetScreenHidden{field<uint64_t>(j, "screen"), field<bool>(j, "hidden")};
    if (op == "moveScreen")
        return MoveScreen{field<uint64_t>(j, "screen"), field<size_t>(j, "index")};
    if (op == "addSlice")
        return AddSlice{field<uint64_t>(j, "screen"), indexField(j), field<Slice>(j, "slice")};
    if (op == "removeSlice")
        return RemoveSlice{field<uint64_t>(j, "slice")};
    if (op == "setSliceCorners")
        return SetSliceCorners{field<uint64_t>(j, "slice"), field<std::array<double, 8>>(j, "corners")};
    if (op == "setSliceInput")
        return SetSliceInput{field<uint64_t>(j, "slice"), optionalField<uint64_t>(j, "input"),
                             optionalField<std::array<double, 4>>(j, "inputRect")};
    return std::nullopt;
}

Edit editFromJson(const nlohmann::json& j)
{
    if (!j.is_object())
        throw ProtocolError("message is not an object");
    const auto op = field<std::string>(j, "op");
    if (op == "batch") {
        Batch batch;
        for (const nlohmann::json& edit : field<std::vector<nlohmann::json>>(j, "edits"))
            batch.edits.push_back(editFromJson(edit));
        return batch;
    }
    if (op == "setLayerOpacity")
        return SetLayerOpacity{field<uint64_t>(j, "layer"), field<double>(j, "opacity")};
    if (op == "setLayerClip")
        return SetLayerClip{field<uint64_t>(j, "layer"), clipField(j)};
    if (op == "setSlot")
        return SetSlot{field<uint64_t>(j, "layer"), field<uint64_t>(j, "column"), clipField(j)};
    if (op == "addColumn")
        return addColumnFromJson(j);
    if (op == "removeColumn")
        return RemoveColumn{field<uint64_t>(j, "column")};
    if (op == "addDeck")
        return AddDeck{indexField(j), j.contains("deck") ? field<Deck>(j, "deck") : Deck{}, cellsField(j),
                       deckLayersFromJson(j.value("layers", nlohmann::json::object()), "layer")};
    if (op == "setDeckLayer")
        return SetDeckLayer{field<uint64_t>(j, "layer"), field<uint64_t>(j, "deck"), field<DeckLayer>(j, "settings")};
    if (op == "removeDeck")
        return RemoveDeck{field<uint64_t>(j, "deck")};
    if (op == "moveDeck")
        return MoveDeck{field<uint64_t>(j, "deck"), field<size_t>(j, "index")};
    if (op == "setEffectBypass")
        return SetEffectBypass{field<uint64_t>(j, "effect"), field<bool>(j, "bypass")};
    if (op == "setEffectSteps")
        return SetEffectSteps{field<uint64_t>(j, "effect"), field<std::vector<EffectStep>>(j, "steps")};
    if (op == "setEffectAutopilot")
        return SetEffectAutopilot{field<uint64_t>(j, "effect"), autopilotModeField(j), optionalField<int>(j, "bars"),
                                  optionalField<int>(j, "hold"), optionalField<double>(j, "transition")};
    if (op == "triggerStep")
        return TriggerStep{field<uint64_t>(j, "effect"), field<size_t>(j, "step"), field<double>(j, "beat")};
    if (op == "setSensitivity")
        return SetSensitivity{field<std::string>(j, "source"), field<double>(j, "value")};
    if (op == "setBandEdges")
        return SetBandEdges{field<std::array<double, 2>>(j, "edges")};
    if (op == "rename")
        return Rename{field<std::string>(j, "kind"), field<uint64_t>(j, "id"), field<std::string>(j, "name")};
    if (op == "setQuantise")
        return SetQuantise{field<int>(j, "beats")};
    if (op == "setFrameGuard")
        return SetFrameGuard{field<bool>(j, "on")};
    if (op == "setAutopilotPace")
        return SetAutopilotPace{field<double>(j, "pace")};
    if (op == "setColumnAutopilot")
        return SetColumnAutopilot{autopilotModeField(j), optionalField<int>(j, "bars"), optionalField<int>(j, "hold")};
    if (op == "triggerClip")
        return TriggerClip{field<uint64_t>(j, "layer"), field<uint64_t>(j, "column"), field<double>(j, "beat")};
    if (op == "triggerColumn")
        return TriggerColumn{field<uint64_t>(j, "column"), field<double>(j, "beat")};
    if (op == "selectDeck")
        return SelectDeck{field<uint64_t>(j, "deck"), optionalField<double>(j, "beat")};
    if (op == "armClip")
        return ArmClip{field<uint64_t>(j, "layer"), field<uint64_t>(j, "column"), field<bool>(j, "armed")};
    if (op == "setTempo")
        return SetTempo{field<double>(j, "bpm")};
    if (op == "setClipTransport")
        return SetClipTransport{field<uint64_t>(j, "layer"),
                                syncField(j),
                                playModeField(j),
                                optionalField<int>(j, "beats"),
                                optionalField<double>(j, "jumpInterval"),
                                optionalField<double>(j, "jumpDistance"),
                                optionalField<uint64_t>(j, "column"),
                                optionalField<double>(j, "speed"),
                                optionalField<double>(j, "in"),
                                optionalField<double>(j, "out")};
    if (op == "setClipTrigger")
        return SetClipTrigger{field<uint64_t>(j, "layer"), field<uint64_t>(j, "column"),
                              j.contains("quantise") && !j["quantise"].is_null() ? std::optional(field<int>(j, "quantise"))
                                                                                  : std::nullopt};
    if (op == "stopLayer")
        return StopLayer{field<uint64_t>(j, "layer")};
    if (op == "setLayerAutopilot")
        return SetLayerAutopilot{field<uint64_t>(j, "layer"), autopilotModeField(j), optionalField<int>(j, "bars"),
                                 optionalField<int>(j, "hold")};
    if (op == "setClipStep")
        return SetClipStep{field<uint64_t>(j, "layer"), field<uint64_t>(j, "column"), field<int>(j, "bars")};
    if (op == "setLayerTransition")
        return SetLayerTransition{field<uint64_t>(j, "layer"), transitionModeField(j),
                                  optionalField<double>(j, "beats")};
    if (op == "setTransitionParam")
        return SetTransitionParam{field<uint64_t>(j, "layer"), transitionFromName(field<std::string>(j, "mode")),
                                  field<std::string>(j, "name"), nullableBeats(j, "value")};
    if (op == "setClipTransition")
        return SetClipTransition{field<uint64_t>(j, "layer"), field<uint64_t>(j, "column"), nullableBeats(j, "beats")};
    if (op == "addEffect")
        return addEffectFromJson(j);
    if (op == "removeEffect")
        return RemoveEffect{field<uint64_t>(j, "effect")};
    if (op == "moveEffect")
        return MoveEffect{field<uint64_t>(j, "effect"), field<size_t>(j, "index"), optionalField<uint64_t>(j, "layer"),
                          optionalField<uint64_t>(j, "column"), optionalField<uint64_t>(j, "deck")};
    if (op == "setEffectParam")
        return SetEffectParam{field<uint64_t>(j, "effect"), field<std::string>(j, "param"), field<double>(j, "value")};
    if (op == "setEffectParams")
        return SetEffectParams{field<uint64_t>(j, "effect"), field<std::map<std::string, double>>(j, "params"),
                               optionalField<std::string>(j, "preset")};
    if (op == "setLayerBlend")
        return SetLayerBlend{field<uint64_t>(j, "layer"), blendModeFromName(field<std::string>(j, "blend"))};
    if (op == "setLayerBypass")
        return SetLayerBypass{field<uint64_t>(j, "layer"), field<bool>(j, "bypass")};
    if (op == "setLayerSolo")
        return SetLayerSolo{field<uint64_t>(j, "layer"), field<bool>(j, "solo")};
    if (op == "setLayerFullResolution")
        return SetLayerFullResolution{field<uint64_t>(j, "layer"), field<bool>(j, "fullResolution")};
    if (op == "addLayer")
        return addLayerFromJson(j);
    if (op == "removeLayer")
        return RemoveLayer{field<uint64_t>(j, "layer")};
    if (op == "moveLayer")
        return MoveLayer{field<uint64_t>(j, "layer"), field<uint64_t>(j, "parent"), field<size_t>(j, "index")};
    if (op == "loadComposition")
        return LoadComposition{field<Composition>(j, "composition")};
    if (auto edit = screenEditFromJson(op, j))
        return *edit;
    if (auto edit = modulationEditFromJson(op, j))
        return *edit;
    if (auto edit = mappingEditFromJson(op, j))
        return *edit;
    if (auto edit = cueEditFromJson(op, j))
        return *edit;
    if (auto edit = textEditFromJson(op, j))
        return *edit;
    if (auto edit = paletteEditFromJson(op, j))
        return *edit;
    throw ProtocolError("unknown op " + op);
}

} // namespace azl
