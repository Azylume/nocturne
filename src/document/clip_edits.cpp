#include "document/edit_support.hpp"

#include "document/cue_edits.hpp"
#include "document/mapping_edits.hpp"
#include "document/modulation_edits.hpp"
#include "document/palette.hpp"
#include "document/text_edits.hpp"
#include "effects/isf.hpp"

#include <algorithm>
#include <cmath>

// Edits of clips and their cells, transport, triggers, Autopilot and transitions, and the live triggers.

namespace azl {

using namespace edits;

namespace {

std::optional<ClipRef> cellOf(const Layer& layer, uint64_t column)
{
    auto it = layer.clips.find(column);
    return it == layer.clips.end() ? std::nullopt : std::optional(it->second);
}

void setCell(Layer& layer, uint64_t column, const std::optional<ClipRef>& clip)
{
    if (clip)
        layer.clips[column] = *clip;
    else
        layer.clips.erase(column);
}

// The cell setLayerClip works on: the one the layer plays, else the shown deck's first column.
uint64_t layerClipColumn(Composition& composition, const Layer& layer)
{
    if (layer.playing && hasColumn(composition, layer.playing->column))
        return layer.playing->column;
    const Deck& deck = findDeck(composition, composition.deck);
    if (deck.columns.empty())
        throw ProtocolError("the shown deck has no column to put the clip in");
    return deck.columns.front().id;
}

} // namespace

Edit apply(Composition& composition, SetLayerClip& edit)
{
    Layer& layer = findLayer(composition, edit.layer);
    checkClip(layer, edit.clip);
    if (!edit.clip && !layer.playing)
        return SetLayerClip{layer.id, std::nullopt};
    const uint64_t column = layerClipColumn(composition, layer);
    const std::optional<ClipRef> old = cellOf(layer, column);
    if (edit.clip)
        completeClip(composition, *edit.clip, old ? &layer.clips.at(column) : nullptr);
    const bool restart = edit.clip && (!old || old->hash != edit.clip->hash);
    setCell(layer, column, edit.clip);
    if (restart || !layer.playing)
        layer.playing = Play{column, std::nullopt};
    return SetLayerClip{layer.id, old};
}

Edit apply(Composition& composition, SetSlot& edit)
{
    Layer& layer = findLayer(composition, edit.layer);
    checkClip(layer, edit.clip);
    requireColumn(composition, edit.column);
    const SetSlot inverse{layer.id, edit.column, cellOf(layer, edit.column)};
    if (edit.clip)
        completeClip(composition, *edit.clip, inverse.clip ? &layer.clips.at(edit.column) : nullptr);
    setCell(layer, edit.column, edit.clip);
    return inverse;
}

Edit apply(Composition& composition, SetTempo& edit)
{
    if (!(edit.bpm >= kMinBpm && edit.bpm <= kMaxBpm))
        throw ProtocolError("bpm must be within 20..500");
    const SetTempo inverse{composition.bpm};
    composition.bpm = edit.bpm;
    return inverse;
}

Edit apply(Composition& composition, SetClipTransport& edit)
{
    if (edit.beats)
        checkBeats(*edit.beats);
    if (edit.jumpInterval)
        checkJumpInterval(*edit.jumpInterval);
    if (edit.jumpDistance)
        checkJumpDistance(*edit.jumpDistance);
    Layer& layer = findLayer(composition, edit.layer);
    const uint64_t column = edit.column.value_or(layer.playing ? layer.playing->column : 0);
    auto cell = layer.clips.find(column);
    if (cell == layer.clips.end())
        throw ProtocolError("layer " + std::to_string(edit.layer) + " has no clip" +
                            (edit.column ? " in column " + std::to_string(column) : ""));
    edit.column = column;
    Transport& transport = cell->second.transport;
    Transport next = transport;
    next.speed = edit.speed.value_or(next.speed);
    next.in = edit.in.value_or(next.in);
    next.out = edit.out.value_or(next.out);
    checkSpeedRange(next);
    SetClipTransport inverse;
    inverse.layer = layer.id;
    inverse.column = column;
    if (edit.sync) {
        inverse.sync = transport.sync;
        transport.sync = *edit.sync;
    }
    if (edit.playMode) {
        inverse.playMode = transport.playMode;
        transport.playMode = *edit.playMode;
    }
    if (edit.beats) {
        inverse.beats = transport.beats;
        transport.beats = *edit.beats;
    }
    if (edit.jumpInterval) {
        inverse.jumpInterval = transport.jumpInterval;
        transport.jumpInterval = *edit.jumpInterval;
    }
    if (edit.jumpDistance) {
        inverse.jumpDistance = transport.jumpDistance;
        transport.jumpDistance = *edit.jumpDistance;
    }
    if (edit.speed)
        inverse.speed = transport.speed;
    if (edit.in)
        inverse.in = transport.in;
    if (edit.out)
        inverse.out = transport.out;
    transport.speed = next.speed;
    transport.in = next.in;
    transport.out = next.out;
    return inverse;
}

Edit apply(Composition& composition, SetClipTrigger& edit)
{
    if (edit.quantise)
        checkQuantise(*edit.quantise);
    Layer& layer = findLayer(composition, edit.layer);
    auto cell = layer.clips.find(edit.column);
    if (cell == layer.clips.end())
        throw ProtocolError("layer " + std::to_string(edit.layer) + " has no clip in column " +
                            std::to_string(edit.column));
    const SetClipTrigger inverse{layer.id, edit.column, cell->second.quantise};
    cell->second.quantise = edit.quantise;
    return inverse;
}

Edit apply(Composition& composition, SetLayerAutopilot& edit)
{
    checkAutopilotBars(edit.bars, edit.hold);
    Layer& layer = findLayer(composition, edit.layer);
    if (layer.group)
        throw ProtocolError("a group has no clips to step through");
    const SetLayerAutopilot inverse{layer.id, layer.autopilot.mode, layer.autopilot.bars, layer.autopilot.hold};
    layer.autopilot.mode = edit.mode.value_or(layer.autopilot.mode);
    layer.autopilot.bars = edit.bars.value_or(layer.autopilot.bars);
    layer.autopilot.hold = edit.hold.value_or(layer.autopilot.hold);
    return inverse;
}

Edit apply(Composition& composition, SetClipStep& edit)
{
    checkStep(edit.bars);
    Layer& layer = findLayer(composition, edit.layer);
    auto cell = layer.clips.find(edit.column);
    if (cell == layer.clips.end())
        throw ProtocolError("layer " + std::to_string(edit.layer) + " has no clip in column " +
                            std::to_string(edit.column));
    const SetClipStep inverse{layer.id, edit.column, cell->second.step};
    cell->second.step = edit.bars;
    return inverse;
}

Edit apply(Composition& composition, SetLayerTransition& edit)
{
    if (edit.beats)
        checkTransitionBeats(*edit.beats);
    Layer& layer = findLayer(composition, edit.layer);
    if (layer.group)
        throw ProtocolError("a group has no clips to transition between");
    const SetLayerTransition inverse{layer.id, layer.transition.mode, layer.transition.beats};
    layer.transition.mode = edit.mode.value_or(layer.transition.mode);
    layer.transition.beats = edit.beats.value_or(layer.transition.beats);
    return inverse;
}

Edit apply(Composition& composition, SetTransitionParam& edit)
{
    Layer& layer = findLayer(composition, edit.layer);
    if (layer.group)
        throw ProtocolError("a group has no clips to transition between");
    const std::string mode = transitionName(edit.mode);
    const auto transitions = isfTransitions();
    const IsfTransition* transition = findIsfTransition(*transitions, edit.mode);
    const auto& params = transition ? transition->params : std::vector<ParamDef>{};
    auto param = std::find_if(params.begin(), params.end(), [&](const ParamDef& p) { return p.name == edit.name; });
    if (param == params.end())
        throw ProtocolError("transition " + mode + " has no parameter " + edit.name);
    if (edit.value && !(*edit.value >= param->min && *edit.value <= param->max))
        throw ProtocolError(mode + "." + edit.name + " must be within " + std::to_string(param->min) + ".." +
                            std::to_string(param->max));
    auto& values = layer.transitionParams[mode];
    auto old = values.find(edit.name);
    const SetTransitionParam inverse{layer.id, edit.mode, edit.name,
                                     old == values.end() ? std::nullopt : std::optional<double>(old->second)};
    if (edit.value)
        values[edit.name] = *edit.value;
    else
        values.erase(edit.name);
    if (values.empty())
        layer.transitionParams.erase(mode);
    return inverse;
}

Edit apply(Composition& composition, SetClipTransition& edit)
{
    if (edit.beats)
        checkTransitionBeats(*edit.beats);
    Layer& layer = findLayer(composition, edit.layer);
    auto cell = layer.clips.find(edit.column);
    if (cell == layer.clips.end())
        throw ProtocolError("layer " + std::to_string(edit.layer) + " has no clip in column " +
                            std::to_string(edit.column));
    const SetClipTransition inverse{layer.id, edit.column, cell->second.transition};
    cell->second.transition = edit.beats;
    return inverse;
}

Edit apply(Composition& composition, TriggerClip& edit)
{
    Layer& layer = findLayer(composition, edit.layer);
    requireColumn(composition, edit.column);
    if (!layer.clips.count(edit.column))
        throw ProtocolError("layer " + std::to_string(edit.layer) + " has no clip in column " +
                            std::to_string(edit.column));
    edit.beat = quantised(edit.beat, layer.clips.at(edit.column).quantise.value_or(composition.quantise));
    layer.playing = Play{edit.column, edit.beat};
    return edit;
}

Edit apply(Composition& composition, StopLayer& edit)
{
    Layer& layer = findLayer(composition, edit.layer);
    if (layer.group)
        throw ProtocolError("a group plays no clip to stop");
    layer.playing.reset();
    return edit;
}

Edit apply(Composition& composition, TriggerColumn& edit)
{
    requireColumn(composition, edit.column);
    edit.beat = quantised(edit.beat, composition.quantise);
    forEachLayer(composition.layers, [&](Layer& layer) {
        if (!layer.group)
            layer.playing = Play{edit.column, edit.beat};
    });
    composition.column = edit.column;
    return edit;
}

Edit apply(Composition& composition, SelectDeck& edit)
{
    findDeck(composition, edit.deck);
    composition.deck = edit.deck;
    selectDeckPalette(composition, edit.deck, edit.beat);
    return edit;
}

Edit apply(Composition& composition, ArmClip& edit)
{
    Layer& layer = findLayer(composition, edit.layer);
    requireColumn(composition, edit.column);
    if (edit.armed)
        layer.armed.insert(edit.column);
    else
        layer.armed.erase(edit.column);
    return edit;
}

} // namespace azl
