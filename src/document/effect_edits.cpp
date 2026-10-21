#include "document/edit_support.hpp"

#include "document/cue_edits.hpp"
#include "document/mapping_edits.hpp"
#include "document/modulation_edits.hpp"
#include "document/palette.hpp"
#include "document/text_edits.hpp"

#include <algorithm>
#include <cmath>

// Edits of effect chains: adding, removing, moving and setting effects, and stepping them.

namespace azl {

using namespace edits;

namespace {

// An effect chain of the document: the composition's (layer 0), a layer's or a group's, or with a column a clip's.
struct Chain {
    std::vector<Effect>* effects = nullptr;
    uint64_t layer = 0;
    std::optional<uint64_t> column;
    std::optional<uint64_t> deck{};
};

Chain chainAt(Composition& composition, uint64_t layerId, std::optional<uint64_t> column,
              std::optional<uint64_t> deck)
{
    if (layerId == 0 && (column || deck))
        throw ProtocolError("the composition has no clips");
    if (layerId == 0)
        return {&composition.effects, 0, std::nullopt};
    Layer& layer = findLayer(composition, layerId);
    if (column && deck)
        throw ProtocolError("an effect goes onto a clip or onto a deck's layer chain, not both");
    if (deck) {
        auto own = layer.decks.find(*deck);
        if (own == layer.decks.end() || !own->second.effects)
            throw ProtocolError("layer " + std::to_string(layer.id) + " has no chain of its own in deck " +
                                std::to_string(*deck));
        return {&*own->second.effects, layer.id, std::nullopt, deck};
    }
    if (!column)
        return {&layer.effects, layer.id, std::nullopt};
    auto cell = layer.clips.find(*column);
    if (cell == layer.clips.end())
        throw ProtocolError("layer " + std::to_string(layer.id) + " has no clip in column " + std::to_string(*column));
    return {&cell->second.effects, layer.id, column};
}

bool holds(const std::vector<Effect>& effects, uint64_t id)
{
    return std::any_of(effects.begin(), effects.end(), [id](const Effect& e) { return e.id == id; });
}

Chain chainHolding(Composition& composition, uint64_t id)
{
    Chain chain;
    forEachChain(composition, [&](std::vector<Effect>& effects, uint64_t layer, std::optional<uint64_t> column) {
        if (!chain.effects && holds(effects, id))
            chain = {&effects, layer, column};
    });
    if (!chain.effects)
        throw ProtocolError("no effect " + std::to_string(id));
    if (chain.layer && !chain.column)
        for (const auto& [deck, settings] : findLayer(composition, chain.layer).decks)
            if (settings.effects && &*settings.effects == chain.effects)
                chain.deck = deck;
    return chain;
}

// The chain as `effects` would leave it keeps a generator clip's generator first and generators out of other chains.
void checkSources(Composition& composition, const Chain& chain, const std::vector<Effect>& effects)
{
    bool generatorClip = false;
    if (chain.column) {
        const ClipRef& clip = findLayer(composition, chain.layer).clips.at(*chain.column);
        generatorClip = clip.hash.empty() && !clip.text;
    }
    edits::checkSources(effects, generatorClip);
}

std::vector<Effect>::iterator findEffect(const Chain& chain, uint64_t id)
{
    return std::find_if(chain.effects->begin(), chain.effects->end(), [id](const Effect& e) { return e.id == id; });
}

} // namespace

Edit apply(Composition& composition, AddEffect& edit)
{
    const Chain chain = chainAt(composition, edit.layer, edit.column, edit.deck);
    std::vector<Effect>& effects = *chain.effects;
    checkChainRoom(effects.size() + 1);
    const size_t index = edit.index.value_or(effects.size());
    if (index > effects.size())
        throw ProtocolError("effect index out of range");

    IdPool ids = effectIds(composition);
    const Effect effect = completeEffect(edit.effect, ids);
    std::vector<Effect> result = effects;
    result.insert(result.begin() + std::ptrdiff_t(index), effect);
    checkSources(composition, chain, result);
    effects = std::move(result);
    edit.index = index;
    edit.effect = effect;
    return RemoveEffect{effect.id};
}

Edit apply(Composition& composition, RemoveEffect& edit)
{
    const Chain chain = chainHolding(composition, edit.effect);
    auto it = findEffect(chain, edit.effect);
    const AddEffect inverse{chain.layer, size_t(it - chain.effects->begin()), *it, chain.column, chain.deck};
    std::vector<Effect> result = *chain.effects;
    result.erase(result.begin() + (it - chain.effects->begin()));
    checkSources(composition, chain, result);
    *chain.effects = std::move(result);
    return inverse;
}

Edit apply(Composition& composition, MoveEffect& edit)
{
    const Chain from = chainHolding(composition, edit.effect);
    const Chain to = edit.layer ? chainAt(composition, *edit.layer, edit.column, edit.deck) : from;
    const bool across = to.effects != from.effects;
    if (across)
        checkChainRoom(to.effects->size() + 1);
    if (edit.index > to.effects->size() - (across ? 0 : 1))
        throw ProtocolError("effect index out of range");
    auto it = findEffect(from, edit.effect);
    const size_t index = size_t(it - from.effects->begin());
    const MoveEffect inverse = edit.layer ? MoveEffect{edit.effect, index, from.layer, from.column, from.deck}
                                          : MoveEffect{edit.effect, index};
    std::vector<Effect> left = *from.effects;
    left.erase(left.begin() + std::ptrdiff_t(index));
    std::vector<Effect> arrived = across ? *to.effects : left;
    arrived.insert(arrived.begin() + std::ptrdiff_t(edit.index), *it);
    checkSources(composition, to, arrived);
    if (across)
        checkSources(composition, from, left);
    if (across)
        *from.effects = std::move(left);
    *to.effects = std::move(arrived);
    return inverse;
}

Edit apply(Composition& composition, SetEffectParam& edit)
{
    const Chain chain = chainHolding(composition, edit.effect);
    Effect& effect = *findEffect(chain, edit.effect);
    checkParam(effectDef(effect.type), edit.param, edit.value);
    const SetEffectParam inverse{effect.id, edit.param, effect.params[edit.param]};
    effect.params[edit.param] = edit.value;
    return inverse;
}

Edit apply(Composition& composition, SetEffectBypass& edit)
{
    const Chain chain = chainHolding(composition, edit.effect);
    Effect& effect = *findEffect(chain, edit.effect);
    const SetEffectBypass inverse{effect.id, effect.bypass};
    effect.bypass = edit.bypass;
    return inverse;
}

Edit apply(Composition& composition, SetSensitivity& edit)
{
    static const std::set<std::string> sources{"kick", "snare", "hat", "bass", "lowBand", "midBand", "highBand", "lfo"};
    if (!sources.contains(edit.source))
        throw ProtocolError("sensitivity is for kick, snare, hat, bass, lowBand, midBand, highBand or lfo, not " +
                            edit.source);
    if (!(edit.value >= 0.0 && edit.value <= kMaxSensitivity))
        throw ProtocolError("a sensitivity must be within 0..4");
    auto it = composition.sensitivity.find(edit.source);
    const SetSensitivity inverse{edit.source, it == composition.sensitivity.end() ? 1.0 : it->second};
    composition.sensitivity[edit.source] = edit.value;
    return inverse;
}

Edit apply(Composition& composition, SetBandEdges& edit)
{
    if (!validBandEdges(edit.edges))
        throw ProtocolError("band edges must be within 40..12000 Hz, Mid/High at least 1.25 times Low/Mid");
    const SetBandEdges inverse{composition.bandEdges};
    composition.bandEdges = edit.edges;
    return inverse;
}

Edit apply(Composition& composition, SetEffectParams& edit)
{
    const Chain chain = chainHolding(composition, edit.effect);
    Effect& effect = *findEffect(chain, edit.effect);
    const EffectDef& def = effectDef(effect.type);
    SetEffectParams inverse{effect.id, {}};
    for (const auto& [name, value] : edit.params) {
        checkParam(def, name, value);
        inverse.params[name] = effect.params[name];
    }
    for (const auto& [name, value] : edit.params)
        effect.params[name] = value;
    if (edit.preset) {
        inverse.preset = effect.preset;
        effect.preset = *edit.preset;
    }
    return inverse;
}

Edit apply(Composition& composition, SetEffectSteps& edit)
{
    const Chain chain = chainHolding(composition, edit.effect);
    Effect& effect = *findEffect(chain, edit.effect);
    checkSteps(effectDef(effect.type), edit.steps);
    SetEffectSteps inverse{effect.id, effect.steps};
    effect.steps = edit.steps;
    return inverse;
}

Edit apply(Composition& composition, SetEffectAutopilot& edit)
{
    checkAutopilotBars(edit.bars, edit.hold);
    if (edit.transition)
        checkTransitionBeats(*edit.transition);
    const Chain chain = chainHolding(composition, edit.effect);
    Effect& effect = *findEffect(chain, edit.effect);
    Autopilot& autopilot = effect.autopilot;
    const SetEffectAutopilot inverse{effect.id, autopilot.mode, autopilot.bars, autopilot.hold, effect.transition};
    autopilot.mode = edit.mode.value_or(autopilot.mode);
    autopilot.bars = edit.bars.value_or(autopilot.bars);
    autopilot.hold = edit.hold.value_or(autopilot.hold);
    effect.transition = edit.transition.value_or(effect.transition);
    return inverse;
}

Edit apply(Composition& composition, TriggerStep& edit)
{
    const Chain chain = chainHolding(composition, edit.effect);
    Effect& effect = *findEffect(chain, edit.effect);
    if (edit.step >= effect.steps.size())
        throw ProtocolError("effect " + std::to_string(effect.id) + " has no step " + std::to_string(edit.step + 1));
    edit.beat = quantised(edit.beat, composition.quantise);
    effect.playing = StepPlay{edit.step, edit.beat};
    return edit;
}

} // namespace azl
