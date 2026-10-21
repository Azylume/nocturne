#include "document/edit_support.hpp"

#include "document/text_edits.hpp"

#include <algorithm>
#include <cmath>

namespace azl::edits {

namespace {

constexpr int kMaxBeats = 1024;

constexpr double kMaxSpeed = 4.0;

constexpr int kMaxQuantise = 64;

// Keeps a chain's parameters within one fragment-bytes upload.
constexpr size_t kMaxEffectsPerLayer = 32;

} // namespace

std::optional<Place> locate(std::vector<Layer>& layers, uint64_t id, uint64_t parent)
{
    for (size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].id == id)
            return Place{&layers, i, parent};
        if (auto place = locate(layers[i].layers, id, layers[i].id))
            return place;
    }
    return std::nullopt;
}

Place place(Composition& composition, uint64_t id)
{
    auto found = locate(composition.layers, id, 0);
    if (!found)
        throw ProtocolError("no layer " + std::to_string(id));
    return *found;
}

Layer& findLayer(Composition& composition, uint64_t id)
{
    const Place p = place(composition, id);
    return (*p.siblings)[p.index];
}

std::vector<Layer>& children(Composition& composition, uint64_t parent)
{
    if (parent == 0)
        return composition.layers;
    Layer& group = findLayer(composition, parent);
    if (!group.group)
        throw ProtocolError("layer " + std::to_string(parent) + " is not a group");
    return group.layers;
}

bool isContentHash(const std::string& s)
{
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

void checkBeats(int beats)
{
    if (beats < 0 || beats > kMaxBeats)
        throw ProtocolError("beats must be within 0 (automatic)..1024");
}

void checkJumpInterval(double interval)
{
    for (double allowed = 0.125; allowed <= 8.0; allowed *= 2.0)
        if (interval == allowed)
            return;
    throw ProtocolError("jumpInterval must be a power of two within 1/8..8 beats");
}

void checkJumpDistance(double distance)
{
    if (!(distance >= 0.0 && distance <= kMaxBeats))
        throw ProtocolError("jumpDistance must be within 0..1024 beats");
}

void checkSpeedRange(const Transport& t)
{
    if (!(t.speed >= 0.0 && t.speed <= kMaxSpeed))
        throw ProtocolError("speed must be within 0..4");
    if (!(t.in >= 0.0 && t.out <= 1.0 && t.out - t.in >= 0.001))
        throw ProtocolError("in and out must lie within 0..1 with in before out");
}

void checkQuantise(int beats)
{
    if (!(beats == 0 || beats == 1 || beats == 2 || (beats % 4 == 0 && beats > 0 && beats <= kMaxQuantise)))
        throw ProtocolError("quantise must be 0 (off), 1, 2 or a multiple of 4 up to 64 beats");
}

void checkStep(int bars)
{
    if (bars < 0 || bars > kMaxStepBars)
        throw ProtocolError("step must be within 0 (the layer's)..64 bars");
}

double quantised(double beat, int quantise)
{
    if (quantise <= 0)
        return beat;
    return std::ceil(beat / quantise - kBeatEpsilon) * quantise;
}

void checkTransitionBeats(double beats)
{
    if (!(beats >= 0.0 && beats <= kMaxTransitionBeats))
        throw ProtocolError("transition beats must be within 0 (a cut)..64");
}

void checkSources(const std::vector<Effect>& effects, bool generatorClip)
{
    for (size_t i = 0; i < effects.size(); ++i)
        if (isGenerator(effects[i].type) && !(generatorClip && i == 0))
            throw ProtocolError("a generator can only start a generator clip's chain");
    if (generatorClip && (effects.empty() || !isGenerator(effects.front().type)))
        throw ProtocolError("a generator clip's chain starts with its generator");
}

void checkClip(const Layer& layer, const std::optional<ClipRef>& clip)
{
    if (!clip)
        return;
    if (layer.group)
        throw ProtocolError("a group has no clip");
    if (clip->text && !clip->hash.empty())
        throw ProtocolError("a text clip has no media hash");
    if (clip->text)
        checkText(*clip->text);
    else if (!isContentHash(clip->hash) && !isGeneratorClip(*clip))
        throw ProtocolError("clip hash must be 64 lowercase hex digits (SHA-256)");
    checkSources(clip->effects, isGeneratorClip(*clip));
    checkBeats(clip->transport.beats);
    checkStep(clip->step);
    if (clip->transition)
        checkTransitionBeats(*clip->transition);
    checkJumpInterval(clip->transport.jumpInterval);
    checkJumpDistance(clip->transport.jumpDistance);
    checkSpeedRange(clip->transport);
    if (clip->quantise)
        checkQuantise(*clip->quantise);
}

Deck& findDeck(Composition& composition, uint64_t id)
{
    for (Deck& deck : composition.decks)
        if (deck.id == id)
            return deck;
    throw ProtocolError("no deck " + std::to_string(id));
}

ColumnPlace placeColumn(Composition& composition, uint64_t id)
{
    for (Deck& deck : composition.decks)
        for (size_t i = 0; i < deck.columns.size(); ++i)
            if (deck.columns[i].id == id)
                return {&deck, i};
    throw ProtocolError("no column " + std::to_string(id));
}

bool hasColumn(const Composition& composition, uint64_t id)
{
    for (const Deck& deck : composition.decks)
        for (const Column& column : deck.columns)
            if (column.id == id)
                return true;
    return false;
}

void requireColumn(const Composition& composition, uint64_t id)
{
    if (!hasColumn(composition, id))
        throw ProtocolError("no column " + std::to_string(id));
}

const EffectDef& effectDef(const std::string& type)
{
    const EffectDef* def = azl::findEffect(type);
    if (!def)
        throw ProtocolError("unknown effect " + type);
    return *def;
}

const ParamDef& paramDef(const EffectDef& def, const std::string& name)
{
    auto it = std::find_if(def.params.begin(), def.params.end(), [&](const ParamDef& p) { return p.name == name; });
    if (it == def.params.end())
        throw ProtocolError(def.type + " has no parameter " + name);
    return *it;
}

void checkParam(const EffectDef& def, const std::string& name, double value)
{
    const ParamDef& param = paramDef(def, name);
    if (!(value >= param.min && value <= param.max))
        throw ProtocolError(def.type + "." + name + " must be within " + std::to_string(param.min) + ".." +
                            std::to_string(param.max));
}

std::map<std::string, double> completeParams(const EffectDef& def, const std::map<std::string, double>& given)
{
    for (const auto& [name, value] : given)
        checkParam(def, name, value);
    std::map<std::string, double> params;
    for (const ParamDef& param : def.params)
        params[param.name] = given.contains(param.name) ? given.at(param.name) : param.defaultValue;
    return params;
}

IdPool effectIds(const Composition& composition, const std::vector<Effect>* replaced)
{
    IdPool ids;
    forEachChain(composition, [&](const std::vector<Effect>& effects, uint64_t, std::optional<uint64_t>) {
        if (&effects == replaced)
            return;
        for (const Effect& effect : effects)
            ids.reserve(effect.id);
    });
    return ids;
}

IdPool layerIds(const Composition& composition)
{
    IdPool ids;
    forEachLayer(composition.layers, [&](const Layer& layer) { ids.reserve(layer.id); });
    return ids;
}

IdPool columnIds(const Composition& composition)
{
    IdPool ids;
    for (const Deck& deck : composition.decks)
        for (const Column& column : deck.columns)
            ids.reserve(column.id);
    return ids;
}

IdPool deckIds(const Composition& composition)
{
    IdPool ids;
    for (const Deck& deck : composition.decks)
        ids.reserve(deck.id);
    return ids;
}

void checkSteps(const EffectDef& def, const std::vector<EffectStep>& steps)
{
    if (steps.size() > kMaxEffectSteps)
        throw ProtocolError("an effect holds at most " + std::to_string(kMaxEffectSteps) + " steps");
    for (const EffectStep& step : steps)
        for (const auto& [name, value] : step.params)
            checkParam(def, name, value);
}

Effect completeEffect(const Effect& given, IdPool& ids)
{
    const EffectDef& def = effectDef(given.type);
    checkSteps(def, given.steps);
    Effect effect = given;
    effect.id = ids.take(given.id, "effect");
    effect.type = def.type;
    effect.params = completeParams(def, given.params);
    return effect;
}

void checkChainRoom(size_t size)
{
    if (size > kMaxEffectsPerLayer)
        throw ProtocolError("an effect chain holds at most " + std::to_string(kMaxEffectsPerLayer) + " effects");
}

void completeEffects(std::vector<Effect>& effects, IdPool& ids)
{
    checkChainRoom(effects.size());
    for (Effect& effect : effects)
        effect = completeEffect(effect, ids);
}

void completeClip(const Composition& composition, ClipRef& clip, const ClipRef* replaced)
{
    IdPool ids = effectIds(composition, replaced ? &replaced->effects : nullptr);
    completeEffects(clip.effects, ids);
}

void completeCells(const Composition& composition, std::vector<Cell>& cells)
{
    IdPool ids = effectIds(composition);
    for (Cell& cell : cells)
        completeEffects(cell.clip.effects, ids);
}

void requireDeck(const Composition& composition, uint64_t id)
{
    if (std::none_of(composition.decks.begin(), composition.decks.end(), [&](const Deck& d) { return d.id == id; }))
        throw ProtocolError("no deck " + std::to_string(id));
}

void completeDeckLayer(const Layer& layer, DeckLayer& settings, IdPool& effects)
{
    if (layer.group)
        throw ProtocolError("a group has no settings per deck");
    if (settings.transitionBeats)
        checkTransitionBeats(*settings.transitionBeats);
    if (settings.autopilotBars && (*settings.autopilotBars < 1 || *settings.autopilotBars > kMaxStepBars))
        throw ProtocolError("autopilot bars must be within 1..64");
    if (settings.effects)
        completeEffects(*settings.effects, effects);
}

} // namespace azl::edits
