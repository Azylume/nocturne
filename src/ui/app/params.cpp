#include "ui/app/params.hpp"

#include "document/modulation_edits.hpp"
#include "effects/effect_registry.hpp"

#include <algorithm>
#include <cmath>

namespace azl::ui {

namespace {

const Layer* findLayer(const Composition& composition, uint64_t id)
{
    return layerById(composition, id);
}

const Effect* findEffect(const Composition& composition, const ParamRef& ref)
{
    return effectById(composition, ref.effect);
}

} // namespace

const std::vector<Effect>* chainOf(const Composition& composition, const ChainOwner& owner)
{
    if (owner.layer == 0)
        return owner.column ? nullptr : &composition.effects;
    const Layer* layer = findLayer(composition, owner.layer);
    if (!layer)
        return nullptr;
    if (!owner.column)
        return &deckEffects(*layer, composition.deck);
    auto clip = layer->clips.find(*owner.column);
    return clip == layer->clips.end() ? nullptr : &clip->second.effects;
}

namespace {

std::optional<DeckLayer> ownSettings(const Composition& composition, uint64_t layer)
{
    const Layer* l = findLayer(composition, layer);
    if (!l)
        return std::nullopt;
    auto it = l->decks.find(composition.deck);
    return it == l->decks.end() ? std::nullopt : std::optional<DeckLayer>(it->second);
}

} // namespace

Edit blendEdit(const Composition& composition, uint64_t layer, BlendMode blend)
{
    std::optional<DeckLayer> own = ownSettings(composition, layer);
    if (!own || !own->blend)
        return SetLayerBlend{layer, blend};
    own->blend = blend;
    return SetDeckLayer{layer, composition.deck, *own};
}

Edit autopilotEdit(const Composition& composition, uint64_t layer, std::optional<AutopilotMode> mode,
                   std::optional<int> bars)
{
    std::optional<DeckLayer> own = ownSettings(composition, layer);
    const bool ownMode = own && own->autopilotMode && mode;
    const bool ownBars = own && own->autopilotBars && bars;
    if (!ownMode && !ownBars)
        return SetLayerAutopilot{layer, mode, bars};
    own->autopilotMode = ownMode ? mode : own->autopilotMode;
    own->autopilotBars = ownBars ? bars : own->autopilotBars;
    return SetDeckLayer{layer, composition.deck, *own};
}

AddEffect addEffectTo(const Composition& composition, const ChainOwner& owner, std::optional<size_t> index,
                      Effect effect)
{
    AddEffect edit{owner.layer, index, std::move(effect), owner.column};
    const Layer* layer = owner.layer && !owner.column ? findLayer(composition, owner.layer) : nullptr;
    if (layer && &deckEffects(*layer, composition.deck) != &layer->effects)
        edit.deck = composition.deck;
    return edit;
}

std::vector<Edit> addEffectsTo(const Composition& composition, const ChainOwner& owner, std::optional<size_t> index,
                               const std::vector<Effect>& effects)
{
    std::vector<Edit> edits;
    for (size_t i = 0; i < effects.size(); ++i)
        edits.push_back(addEffectTo(composition, owner, index ? std::optional<size_t>(*index + i) : std::nullopt,
                                    effects[i]));
    return edits;
}

std::optional<Edit> moveEffectTo(const Composition& composition, uint64_t effect, const ChainOwner& owner,
                                 std::optional<size_t> index, bool copy)
{
    const Effect* source = effectById(composition, effect);
    const std::vector<Effect>* chain = chainOf(composition, owner);
    if (!source || !chain)
        return std::nullopt;
    const bool within = std::any_of(chain->begin(), chain->end(), [&](const Effect& e) { return e.id == effect; });
    const size_t end = chain->size() - (within && !copy ? 1 : 0);
    Effect copied = *source;
    copied.id = 0;
    AddEffect add = addEffectTo(composition, owner, std::min(index.value_or(end), end), copied);
    if (copy)
        return add;
    return MoveEffect{effect, *add.index, add.layer, add.column, add.deck};
}

std::optional<ChainOwner> ownerOf(const Composition& composition, const ParamRef& ref)
{
    std::optional<ChainOwner> owner;
    if (ref.effect)
        forEachChain(composition, [&](const std::vector<Effect>& chain, uint64_t layer, std::optional<uint64_t> column) {
            for (const Effect& e : chain)
                if (!owner && e.id == ref.effect)
                    owner = ChainOwner{layer, column};
        });
    else if (ref.layer == 0 || findLayer(composition, ref.layer))
        owner = ChainOwner{ref.layer};
    return owner;
}

const std::vector<std::string>& ParamInfo::options() const
{
    static const std::vector<std::string> none;
    return def ? def->options : none;
}

std::optional<ParamInfo> paramInfo(const Composition& composition, const ParamRef& ref)
{
    if (ref.layer == 0 && ref.effect == 0 && ref.param == "opacity")
        return ParamInfo{ref, "Master", 0.0, 1.0, 1.0, composition.opacity};
    if (ref.effect == 0) {
        const Layer* layer = findLayer(composition, ref.layer);
        if (!layer || ref.param != "opacity")
            return std::nullopt;
        return ParamInfo{ref, "Opacity", 0.0, 1.0, 1.0, layer->opacity};
    }
    const Effect* effect = findEffect(composition, ref);
    const EffectDef* def = effect ? azl::findEffect(effect->type) : nullptr;
    if (!def)
        return std::nullopt;
    for (const ParamDef& param : def->params) {
        if (param.name != ref.param)
            continue;
        auto value = effect->params.find(param.name);
        const std::string label = !param.label.empty() ? param.label : param.name == "mix" ? "Dry/wet" : param.name;
        return ParamInfo{ref,
                         label,
                         param.min,
                         param.max,
                         param.defaultValue,
                         value == effect->params.end() ? param.defaultValue : value->second,
                         param.unit,
                         param.wraps,
                         &param};
    }
    return std::nullopt;
}

std::string paramLabel(const Composition& composition, const ParamRef& ref)
{
    const std::optional<ChainOwner> chain = ownerOf(composition, ref);
    std::string owner = chain ? "Composition" : "?";
    if (const Layer* layer = chain && chain->layer ? findLayer(composition, chain->layer) : nullptr) {
        owner = layer->name;
        if (chain->column)
            owner += " / " + layer->clips.at(*chain->column).name;
    }
    const auto info = paramInfo(composition, ref);
    const std::string name = info ? info->label : ref.param;
    if (ref.effect == 0)
        return owner + " / " + name;
    const Effect* effect = findEffect(composition, ref);
    return owner + " / " + (effect ? effect->type : std::string("?")) + " / " + name;
}

Edit setParam(const ParamRef& ref, double value)
{
    if (ref.effect == 0)
        return SetLayerOpacity{ref.layer, value};
    return SetEffectParam{ref.effect, ref.param, value};
}

const Modulator* animationOf(const Composition& composition, const ParamRef& ref)
{
    for (const Modulator& m : composition.modulators)
        if (m.target.macro == 0 && m.target.param == ref)
            return &m;
    return nullptr;
}

std::string animationSource(const Modulator& modulator)
{
    return modulator.source == ModulatorSource::Lfo ? "beat" : modulator.feature;
}

double ParamInfo::fit(double v) const
{
    if (wraps && (v < min || v > max))
        return wrapInto(v, min, max);
    return std::clamp(v, min, max);
}

Ends endsOf(const ParamInfo& info, double min, double max)
{
    return {info.value + min, info.value + max};
}

double offsetTo(const ParamInfo& info, double value, double previous)
{
    const double span = info.max - info.min;
    double offset = info.fit(value) - info.value;
    if (info.wraps && span > 0.0)
        offset += span * std::round((previous - offset) / span);
    return std::clamp(offset, -span, span);
}

Range rangeOf(const ParamInfo& info, double min, double max)
{
    return {info.fraction(info.value + min), info.fraction(info.value + max)};
}

double modulatedAt(const Composition& composition, const Modulator& modulator, double value)
{
    const double curved = curveAt(modulator.curve, value);
    Range range{modulator.min, modulator.max};
    if (modulator.target.macro) {
        for (const Macro& macro : composition.macros)
            if (macro.id == modulator.target.macro)
                range = {macro.value + modulator.min, macro.value + modulator.max};
    } else if (const auto info = paramInfo(composition, modulator.target.param)) {
        range = rangeOf(*info, modulator.min, modulator.max);
        if (info->wraps)
            return wrapInto(range.from + (range.to - range.from) * curved, 0.0, 1.0);
    }
    return std::clamp(range.from + (range.to - range.from) * curved, 0.0, 1.0);
}

std::pair<double, double> offsetsOf(const ParamInfo& info, Range range)
{
    auto offset = [&](double fraction) { return info.at(std::clamp(fraction, 0.0, 1.0)) - info.value; };
    return {offset(range.from), offset(range.to)};
}

Modulator withSource(const Modulator& modulator, const ParamInfo& info, const std::string& source)
{
    Modulator m = modulator;
    if (source == "beat") {
        m.source = ModulatorSource::Lfo;
    } else {
        m.source = ModulatorSource::Audio;
        m.feature = source;
    }
    m.name = "Animate " + info.label;
    return m;
}

Modulator newAnimation(const ParamInfo& info, const std::string& source)
{
    Modulator m;
    m.target.param = info.ref;
    m.beats = 4.0;
    setEnvelope(m, EnvelopePreset::Meter);
    m = withSource(m, info, source);
    const double span = info.max - info.min;
    const double swing = info.wraps ? span / 4.0 : span / 8.0;
    const bool whole = !info.wraps && info.unit.suffix == units::percent.suffix;
    const double from = whole ? info.min : info.fit(info.value - swing);
    const double to = whole ? info.max : info.wraps ? info.value + swing : info.fit(info.value + swing);
    m.min = source == "beat" ? from - info.value : 0.0;
    m.max = to - info.value;
    return m;
}

std::vector<const Macro*> macroSlots(const Composition& composition)
{
    std::vector<const Macro*> slots;
    for (const Macro& macro : composition.macros)
        if (macro.group == 0 && slots.size() < size_t(kMacroSlots))
            slots.push_back(&macro);
    slots.resize(kMacroSlots, nullptr);
    return slots;
}

std::pair<std::vector<Edit>, uint64_t> ensureMacro(const Composition& composition, int slot)
{
    const auto slots = macroSlots(composition);
    if (slots[size_t(slot)])
        return {{}, slots[size_t(slot)]->id};
    uint64_t next = 1;
    for (const Macro& macro : composition.macros)
        next = std::max(next, macro.id + 1);
    std::vector<Edit> edits;
    for (int i = 0; i <= slot; ++i) {
        if (slots[size_t(i)])
            continue;
        Macro macro;
        macro.id = next++;
        macro.name = "Macro " + std::to_string(i + 1);
        edits.push_back(AddMacro{std::nullopt, macro});
    }
    return {edits, next - 1};
}

MacroTarget newTarget(const ParamInfo& info)
{
    MacroTarget target;
    target.param = info.ref;
    const bool up = info.max - info.value >= info.value - info.min;
    target.max = (up ? info.max : info.min) - info.value;
    return target;
}

const Modulator* macroModulator(const Composition& composition, uint64_t macro)
{
    for (const Modulator& m : composition.modulators)
        if (m.target.macro == macro)
            return &m;
    return nullptr;
}

const std::vector<NamedCurve>& namedCurves()
{
    static const std::vector<NamedCurve> curves = {
        {"Linear", {{0.0, 0.0, 1.0, 1.0}}},
        {"Ease in", {{0.42, 0.0, 1.0, 1.0}}},
        {"Ease out", {{0.0, 0.0, 0.58, 1.0}}},
        {"S-curve", {{0.42, 0.0, 0.58, 1.0}}},
        {"Late", {{0.8, 0.0, 1.0, 0.4}}},
        {"Early", {{0.0, 0.6, 0.2, 1.0}}},
    };
    return curves;
}

std::string curveName(const Curve& curve)
{
    for (const NamedCurve& named : namedCurves())
        if (named.curve == curve)
            return named.name;
    return "Custom";
}

} // namespace azl::ui
