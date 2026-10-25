#include "engine/modulation_plan.hpp"

#include "document/modulation_edits.hpp"
#include "document/palette.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>

namespace azl {

namespace {

constexpr size_t kNone = std::numeric_limits<size_t>::max();

// A colour group's parameter, which its effect always has.
size_t paramIndex(const EffectDef& def, const std::string& name)
{
    size_t i = 0;
    while (def.params[i].name != name)
        ++i;
    return i;
}

bool isPaletteTarget(const ParamRef& param)
{
    return param.layer == 0 && param.effect == 0 &&
           (param.param == kPalettePosition || paletteHueSlot(param.param).has_value());
}

// The parameter's base value and its index in its effect's parameters, or nothing when it is gone.
std::optional<std::pair<double, size_t>> baseOf(const Composition& composition, const ParamRef& param)
{
    if (param.layer == 0 && param.effect == 0 && param.param == "opacity")
        return std::pair(composition.opacity, size_t(0));
    if (isPaletteTarget(param)) {
        const size_t palettes = composition.palettes.size();
        const bool position = param.param == kPalettePosition;
        return std::pair(position && palettes > 1 ? double(activePaletteIndex(composition)) / double(palettes - 1) : 0.0,
                         size_t(0));
    }
    if (param.effect == 0) {
        const Layer* layer = layerById(composition, param.layer);
        if (!layer)
            return std::nullopt;
        if (param.param == "opacity")
            return std::pair(layer->opacity, size_t(0));
        const auto text = textParamFromName(param.param);
        if (!text)
            return std::nullopt;
        const ClipRef* clip = playingClip(*layer);
        return std::pair(textParamValue(clip && clip->text ? *clip->text : TextClip{}, *text), size_t(*text));
    }
    const Effect* effect = effectById(composition, param.effect);
    const EffectDef* def = effect ? findEffect(effect->type) : nullptr;
    for (size_t i = 0; def && i < def->params.size(); ++i) {
        if (def->params[i].name != param.param)
            continue;
        auto value = effect->params.find(param.param);
        return std::pair(value == effect->params.end() ? def->params[i].defaultValue : value->second, i);
    }
    return std::nullopt;
}

} // namespace

ModulationPlan::ModulationPlan(const Composition& composition)
{
    std::map<uint64_t, size_t> macroSlots;
    for (const Macro& macro : composition.macros) {
        macroSlots[macro.id] = slots_.size();
        macroIds_.push_back(macro.id);
        macroBases_.push_back(macro.value);
        slots_.push_back({macro.value, 0.0, 1.0});
    }
    std::map<std::tuple<uint64_t, uint64_t, std::string>, size_t> paramSlots;
    auto slotOf = [&](const ParamRef& param) {
        const auto key = std::make_tuple(param.layer, param.effect, param.param);
        if (auto it = paramSlots.find(key); it != paramSlots.end())
            return it->second;
        const auto base = baseOf(composition, param);
        const auto range = paramRange(composition, param);
        if (!base || !range)
            return kNone;
        const size_t slot = slots_.size();
        const ParamDef* def = paramDef(composition, param);
        slots_.push_back({base->first, range->first, range->second});
        slots_.back().wraps = (def && def->wraps) || (isPaletteTarget(param) && param.param != kPalettePosition);
        if (def && !def->options.empty())
            slots_.back().options = def;
        paramRefs_.push_back(param);
        paramSlots[key] = slot;
        if (param.param == kPalettePosition && isPaletteTarget(param))
            palettes_.position = slot;
        else if (isPaletteTarget(param))
            palettes_.hues.push_back({*paletteHueSlot(param.param), slot});
        else if (param.effect == 0 && param.param == "opacity")
            opacities_.push_back({param.layer, slot});
        else if (param.effect == 0)
            texts_.push_back({param.layer, TextParam(base->second), slot});
        else
            effectParams_.push_back({param.effect, base->second, slot});
        return slot;
    };

    for (const Macro& macro : composition.macros)
        for (const MacroTarget& target : macro.targets)
            if (const size_t slot = slotOf(target.param); slot != kNone)
                links_.push_back({macroSlots[macro.id], slot, lut(target.curve), target.min, target.max});
    for (const Modulator& m : composition.modulators) {
        if (!m.target.macro && !paramRange(composition, m.target.param))
            dangling_.push_back(m.id);
        auto macro = macroSlots.find(m.target.macro);
        const size_t slot = m.target.macro ? (macro == macroSlots.end() ? kNone : macro->second) : slotOf(m.target.param);
        if (slot == kNone)
            continue;
        const auto feature = audioFeatureFromName(m.feature);
        auto sensitivity = composition.sensitivity.find(m.source == ModulatorSource::Lfo ? std::string("lfo") : m.feature);
        sources_.push_back({m.id, m.source, m.shape, m.beats, m.phase, feature ? size_t(*feature) : 0,
                            sensitivity == composition.sensitivity.end() ? 1.0 : sensitivity->second,
                            m.envelope, {m.attack / 1000.0, m.hold / 1000.0, m.release / 1000.0}, m.combine, lut(m.curve), m.min, m.max, slot,
                            m.points, m.source == ModulatorSource::Audio && m.autoRange});
        if (sources_.back().autoRange)
            ranged_[sources_.back().feature] = true;
        usesAudio_ = usesAudio_ || m.source == ModulatorSource::Audio;
    }
    std::sort(effectParams_.begin(), effectParams_.end(),
              [](const EffectParam& a, const EffectParam& b) { return a.effect < b.effect; });
    auto derive = [&](const std::vector<Effect>& chain) {
        for (const Effect& effect : chain) {
            const EffectDef* def = findEffect(effect.type);
            auto raw = [&](size_t i) {
                auto value = effect.params.find(def->params[i].name);
                size_t slot = kNone;
                for (const EffectParam& p : effectParams_)
                    if (p.effect == effect.id && p.index == i)
                        slot = p.slot;
                return std::pair(value == effect.params.end() ? def->params[i].defaultValue : value->second, slot);
            };
            for (size_t i = 0; def && i < def->params.size(); ++i) {
                const ParamDef& param = def->params[i];
                if (param.drive == ParamDrive::Value) {
                    const auto [base, slot] = raw(i);
                    if (!param.choice && slot == kNone) {
                        Glide g{effect.id, i, base, base, base};
                        g.wraps = param.wraps;
                        g.min = param.min;
                        g.max = param.max;
                        glides_.push_back(g);
                    }
                    continue;
                }
                Derived d{effect.id, i, param.drive};
                std::tie(d.base, d.slot) = raw(i);
                for (size_t j = 0; j < def->params.size(); ++j)
                    if (def->params[j].name == param.scaledBy)
                        std::tie(d.scaleBase, d.scaleSlot) = raw(j);
                derived_.push_back(d);
                usesAudio_ = usesAudio_ || param.drive == ParamDrive::Audio;
            }
        }
    };
    forEachChain(composition,
                 [&](const std::vector<Effect>& chain, uint64_t, std::optional<uint64_t>) { derive(chain); });
    std::sort(derived_.begin(), derived_.end(), [](const Derived& a, const Derived& b) { return a.effect < b.effect; });
    std::sort(glides_.begin(), glides_.end(), [](const Glide& a, const Glide& b) { return a.effect < b.effect; });
    buildSteps(composition);
    buildPalettes(composition);
    for (size_t i = 0; i < stepped_.size(); ++i)
        steppedByEffect_.push_back(i);
    std::stable_sort(steppedByEffect_.begin(), steppedByEffect_.end(),
                     [&](size_t a, size_t b) { return stepped_[a].effect < stepped_[b].effect; });
    sourceValues_.resize(sources_.size());
    envelopes_.resize(sources_.size());
    cycles_.resize(sources_.size(), std::numeric_limits<double>::quiet_NaN());
    publishedRanges_ = std::make_unique<std::atomic<float>[]>(2 * kAudioFeatures);
    for (size_t f = 0; f < kAudioFeatures; ++f) {
        publishedRanges_[2 * f].store(0.0f, std::memory_order_relaxed);
        publishedRanges_[2 * f + 1].store(1.0f, std::memory_order_relaxed);
    }
    published_ = std::make_unique<std::atomic<float>[]>(slots_.size() + sources_.size() + stepped_.size() +
                                                        4 * palettes_.drawn.size());
    for (size_t i = 0; i < slots_.size(); ++i) {
        slots_[i].value = slots_[i].base;
        published_[i].store(float(slots_[i].base), std::memory_order_relaxed);
    }
    for (size_t i = 0; i < stepped_.size(); ++i)
        published_[slots_.size() + sources_.size() + i].store(float(stepped_[i].doc), std::memory_order_relaxed);
    publishPalettes();
}

void ModulationPlan::attach(Stepped& p) const
{
    for (const EffectParam& e : effectParams_)
        if (e.effect == p.effect && e.index == p.index)
            p.slot = e.slot;
    for (size_t g = 0; g < glides_.size(); ++g)
        if (glides_[g].effect == p.effect && glides_[g].index == p.index)
            p.glide = g;
    for (size_t d = 0; d < derived_.size(); ++d)
        if (derived_[d].effect == p.effect && derived_[d].index == p.index)
            p.derived = d;
}

// Every colour linked to a palette colour moves with the palettes, unless its effect's own steps set it.
void ModulationPlan::buildPalettes(const Composition& composition)
{
    if (composition.palettes.empty())
        return;
    size_t slots = 0;
    for (const ColourPalette& palette : composition.palettes)
        slots = std::max(slots, palette.colours.size());
    palettes_.active = activePaletteIndex(composition);
    const ColourPalette& active = composition.palettes[palettes_.active];
    for (const ColourPalette& palette : composition.palettes) {
        std::vector<Rgba>& row = palettes_.colours.emplace_back();
        for (size_t s = 0; s < slots; ++s) {
            const std::vector<PaletteColour>& own = s < palette.colours.size() ? palette.colours : active.colours;
            row.push_back(s < own.size() ? rgbaOf(own[s].colour) : Rgba{1.0, 1.0, 1.0, 1.0});
        }
        for (size_t s = 0; s < palette.colours.size(); ++s)
            palettes_.ids.push_back({palette.colours[s].id, s});
    }
    palettes_.beats = composition.paletteTransition;
    palettes_.step = palettes_.active;
    palettes_.from = palettes_.drawn = palettes_.colours[palettes_.active];
    forEachChain(composition, [&](const std::vector<Effect>& chain, uint64_t, std::optional<uint64_t>) {
        for (const Effect& effect : chain) {
            const EffectDef* def = findEffect(effect.type);
            for (const auto& [key, id] : effect.links) {
                const auto group = def ? colourGroup(*def, key) : std::nullopt;
                auto slot = std::find_if(palettes_.ids.begin(), palettes_.ids.end(),
                                         [&](const auto& entry) { return entry.first == id; });
                if (!group || slot == palettes_.ids.end())
                    continue;
                const std::vector<std::string> names = groupParamNames(*group);
                const bool stepped = std::any_of(effect.steps.begin(), effect.steps.end(), [&](const EffectStep& step) {
                    return std::any_of(names.begin(), names.end(),
                                       [&](const std::string& name) { return step.params.contains(name); });
                });
                if (stepped)
                    continue;
                PaletteLink link{{}, kNone, group->model, slot->second, {}};
                for (size_t c = 0; c < names.size(); ++c) {
                    const size_t index = paramIndex(*def, names[c]);
                    const ParamDef& param = def->params[index];
                    Stepped p{effect.id, index, {0, effect.id, param.name}};
                    auto doc = effect.params.find(param.name);
                    p.doc = p.from = p.base = p.drawn = doc == effect.params.end() ? param.defaultValue : doc->second;
                    p.wraps = param.wraps;
                    p.palette = true;
                    p.min = param.min;
                    p.max = param.max;
                    attach(p);
                    (c < 3 ? link.channels[c] : link.alpha) = stepped_.size();
                    stepped_.push_back(std::move(p));
                }
                link.from = drawnColour(link);
                palettes_.links.push_back(link);
            }
        }
    });
}

void ModulationPlan::buildSteps(const Composition& composition)
{
    forEachChain(composition, [&](const std::vector<Effect>& chain, uint64_t, std::optional<uint64_t>) {
        for (const Effect& effect : chain) {
            const EffectDef* def = findEffect(effect.type);
            if (!def || effect.steps.empty())
                continue;
            StepGroup group{effect.id, effect.transition, stepped_.size()};
            for (size_t i = 0; i < def->params.size(); ++i) {
                const ParamDef& param = def->params[i];
                Stepped p{effect.id, i, {0, effect.id, param.name}};
                bool set = false;
                for (const EffectStep& step : effect.steps) {
                    auto value = step.params.find(param.name);
                    set = set || value != step.params.end();
                    p.values.push_back(value == step.params.end() ? std::numeric_limits<double>::quiet_NaN()
                                                                  : value->second);
                }
                if (!set)
                    continue;
                auto doc = effect.params.find(param.name);
                p.doc = doc == effect.params.end() ? param.defaultValue : doc->second;
                p.from = p.base = p.drawn = p.doc;
                p.choice = param.choice || !param.options.empty();
                p.wraps = param.wraps;
                p.min = param.min;
                p.max = param.max;
                attach(p);
                stepped_.push_back(std::move(p));
            }
            group.count = stepped_.size() - group.first;
            auto at = [&](const std::string& name) {
                for (size_t i = group.first; i < stepped_.size(); ++i)
                    if (stepped_[i].param.param == name)
                        return i;
                return kNone;
            };
            for (const ColourGroup& colour : colourGroups(*def)) {
                StepColour c{{at(colour.channels[0]), at(colour.channels[1]), at(colour.channels[2])}, colour.model};
                if (std::none_of(c.channels.begin(), c.channels.end(), [](size_t i) { return i == kNone; }))
                    group.colours.push_back(c);
            }
            if (group.count)
                stepGroups_.push_back(std::move(group));
        }
    });
}

void ModulationPlan::step(const std::vector<StepPosition>& positions)
{
    for (StepGroup& group : stepGroups_) {
        auto it = std::lower_bound(positions.begin(), positions.end(), group.effect,
                                   [](const StepPosition& p, uint64_t id) { return p.effect < id; });
        group.target = it != positions.end() && it->effect == group.effect ? std::optional(*it) : std::nullopt;
    }
    const bool palette = !positions.empty() && positions.front().effect == kPaletteEffect;
    palettes_.target = palette ? std::optional(positions.front()) : std::nullopt;
}

double ModulationPlan::target(const Stepped& p, std::optional<size_t> step)
{
    const double value = step && *step < p.values.size() ? p.values[*step] : p.doc;
    return std::isnan(value) ? p.doc : value;
}

double ModulationPlan::glided(const Stepped& p, double to, double t)
{
    if (p.choice)
        return t < 0.5 ? p.from : to;
    if (!p.wraps || t >= 1.0)
        return p.from + (to - p.from) * t;
    const double half = (p.max - p.min) * 0.5;
    return wrapInto(p.from + wrapInto(to - p.from, -half, half) * t, p.min, p.max);
}

// A move that starts takes every parameter from where the last frame drew it; the steps' values hold from then on.
void ModulationPlan::glideSteps(double beat)
{
    for (StepGroup& group : stepGroups_) {
        const std::optional<size_t> step = group.target ? std::optional(group.target->step) : std::nullopt;
        const double moved = group.target ? group.target->beat : group.moved;
        if (step != group.step || moved != group.moved) {
            for (size_t i = group.first; i < group.first + group.count; ++i) {
                stepped_[i].from = stepped_[i].drawn;
                stepped_[i].manual = false;
            }
            group.step = step;
            group.moved = moved;
        }
        const double t = group.beats > 0.0 ? std::clamp((beat - group.moved) / group.beats, 0.0, 1.0) : 1.0;
        for (size_t i = group.first; i < group.first + group.count; ++i)
            if (!stepped_[i].manual)
                stepped_[i].base = glided(stepped_[i], target(stepped_[i], group.step), t);
        mixColours(group, t);
    }
}

void ModulationPlan::mixColours(const StepGroup& group, double t)
{
    for (const StepColour& colour : group.colours) {
        auto channel = [&](size_t i) -> Stepped& { return stepped_[colour.channels[i]]; };
        if (channel(0).manual || channel(1).manual || channel(2).manual)
            continue;
        auto rgba = [&](auto value) {
            const double a = value(channel(0)), b = value(channel(1)), c = value(channel(2));
            return colour.model == ColourModel::Rgb ? Rgba{a, b, c} : toRgba(Hsva{a, b, c});
        };
        const Rgba from = rgba([](const Stepped& p) { return p.from; });
        const Rgba to = rgba([&](const Stepped& p) { return target(p, group.step); });
        const Rgba mixed = mixOklab(from, to, t);
        if (colour.model == ColourModel::Rgb) {
            channel(0).base = mixed.r;
            channel(1).base = mixed.g;
            channel(2).base = mixed.b;
            continue;
        }
        const Hsva hsv = toHsva(mixed, channel(0).base);
        channel(0).base = hsv.h;
        channel(1).base = hsv.s;
        channel(2).base = hsv.v;
    }
}

// A move onto another palette starts every colour from where the last frame drew it; the palette position and the hue
// targets, settled here, move where the colours head while they crossfade and after.
void ModulationPlan::glidePalettes(double beat)
{
    if (palettes_.colours.empty())
        return;
    const size_t count = palettes_.colours.size();
    const size_t step = std::min(palettes_.target ? palettes_.target->step : palettes_.active, count - 1);
    const double moved = palettes_.target ? palettes_.target->beat : palettes_.moved;
    if (step != palettes_.step || moved != palettes_.moved) {
        palettes_.from = palettes_.drawn;
        for (PaletteLink& link : palettes_.links) {
            link.from = drawnColour(link);
            for (size_t i : link.channels)
                stepped_[i].manual = false;
            if (link.alpha != kNone)
                stepped_[link.alpha].manual = false;
        }
        palettes_.step = step;
        palettes_.moved = moved;
    }
    auto settle = [&](size_t i) {
        Slot& slot = slots_[i];
        slot.value = fit(slot, (slot.base + slot.add) * slot.mul);
    };
    if (palettes_.position != kNone) {
        slots_[palettes_.position].base = count > 1 ? double(step) / double(count - 1) : 0.0;
        settle(palettes_.position);
    }
    for (const auto& [paletteSlot, slot] : palettes_.hues)
        settle(slot);
    const double t = palettes_.beats > 0.0 ? std::clamp((beat - moved) / palettes_.beats, 0.0, 1.0) : 1.0;
    auto mix = [&](const Rgba& from, const Rgba& to) {
        if (t >= 1.0 || from == to)
            return to;
        return t <= 0.0 ? from : mixOklab(from, to, t);
    };
    for (size_t s = 0; s < palettes_.drawn.size(); ++s)
        palettes_.drawn[s] = mix(palettes_.from[s], paletteTarget(s, step));
    for (const PaletteLink& link : palettes_.links) {
        auto channel = [&](size_t i) -> Stepped& { return stepped_[link.channels[i]]; };
        if (channel(0).manual || channel(1).manual || channel(2).manual)
            continue;
        const Rgba mixed = mix(link.from, paletteTarget(link.slot, step));
        const Hsva hsv = toHsva(mixed, channel(0).base);
        const bool rgb = link.model == ColourModel::Rgb;
        channel(0).base = rgb ? mixed.r : hsv.h;
        channel(1).base = rgb ? mixed.g : hsv.s;
        channel(2).base = rgb ? mixed.b : hsv.v;
        if (link.alpha != kNone && !stepped_[link.alpha].manual)
            stepped_[link.alpha].base = mixed.a;
    }
}

Rgba ModulationPlan::paletteTarget(size_t slot, size_t step) const
{
    const std::vector<std::vector<Rgba>>& colours = palettes_.colours;
    Rgba colour = colours[step][slot];
    if (palettes_.position != kNone && colours.size() > 1) {
        const double at = slots_[palettes_.position].value * double(colours.size() - 1);
        const size_t below = std::min(size_t(at), colours.size() - 2);
        const double t = std::clamp(at - double(below), 0.0, 1.0);
        colour = t <= 0.0   ? colours[below][slot]
                 : t >= 1.0 ? colours[below + 1][slot]
                            : mixOklab(colours[below][slot], colours[below + 1][slot], t);
    }
    for (const auto& [paletteSlot, target] : palettes_.hues)
        if (paletteSlot == slot && slots_[target].value != 0.0)
            colour = rotateHue(colour, slots_[target].value);
    return colour;
}

Rgba ModulationPlan::drawnColour(const PaletteLink& link) const
{
    const double a = stepped_[link.channels[0]].drawn, b = stepped_[link.channels[1]].drawn,
                 c = stepped_[link.channels[2]].drawn;
    const double alpha = link.alpha == kNone ? 1.0 : stepped_[link.alpha].drawn;
    if (link.model == ColourModel::Rgb)
        return {a, b, c, alpha};
    return toRgba(Hsva{a, b, c, alpha});
}

void ModulationPlan::publishPalettes()
{
    const size_t first = slots_.size() + sources_.size() + stepped_.size();
    for (size_t s = 0; s < palettes_.drawn.size(); ++s) {
        const Rgba& c = palettes_.drawn[s];
        for (size_t i = 0; i < 4; ++i)
            published_[first + 4 * s + i].store(float(std::array{c.r, c.g, c.b, c.a}[i]), std::memory_order_relaxed);
    }
}

std::vector<Rgba> ModulationPlan::paletteColours() const
{
    const size_t first = slots_.size() + sources_.size() + stepped_.size();
    std::vector<Rgba> colours;
    for (size_t s = 0; s < palettes_.drawn.size(); ++s) {
        auto at = [&](size_t i) { return double(published_[first + 4 * s + i].load(std::memory_order_relaxed)); };
        colours.push_back({at(0), at(1), at(2), at(3)});
    }
    return colours;
}

std::optional<Rgba> ModulationPlan::linkedColour(uint64_t id) const
{
    for (const auto& [colour, slot] : palettes_.ids)
        if (colour == id)
            return palettes_.drawn[slot];
    return std::nullopt;
}

double ModulationPlan::drawnBase(const Stepped& p) const
{
    if (p.glide != kNone)
        return glides_[p.glide].value;
    return stepBase(p);
}

void ModulationPlan::carry(const ModulationPlan& previous)
{
    for (const MacroValue& m : previous.modulators())
        for (size_t i = 0; i < sources_.size(); ++i)
            if (sources_[i].id == m.id)
                sourceValues_[i] = m.value;
    for (size_t i = 0; i < sources_.size(); ++i)
        for (size_t j = 0; j < previous.sources_.size(); ++j)
            if (previous.sources_[j].id == sources_[i].id) {
                cycles_[i] = previous.cycles_[j];
                envelopes_[i] = previous.envelopes_[j];
            }
    for (size_t f = 0; f < kAudioFeatures; ++f)
        if (ranged_[f] && previous.ranged_[f])
            ranges_[f] = previous.ranges_[f];
    beat_ = previous.beat_;
    for (Derived& d : derived_)
        for (const Derived& p : previous.derived_)
            if (p.effect == d.effect && p.index == d.index && p.started) {
                d.value = p.value;
                d.started = true;
            }
    derivedTime_ = previous.derivedTime_;
    for (StepGroup& group : stepGroups_)
        for (const StepGroup& p : previous.stepGroups_)
            if (p.effect == group.effect) {
                group.step = p.step;
                group.moved = p.moved;
            }
    // A palette edit or selection moves the linked colours with the palettes; any other change of one takes it over.
    const bool palettesEdited =
        palettes_.colours != previous.palettes_.colours || palettes_.active != previous.palettes_.active;
    for (Stepped& s : stepped_)
        for (const Stepped& p : previous.stepped_) {
            if (p.effect != s.effect || p.index != s.index)
                continue;
            s.from = p.from;
            s.base = p.base;
            s.drawn = p.drawn;
            s.manual = s.palette && palettesEdited ? false : p.manual || s.doc != p.doc;
        }
    if (!previous.palettes_.colours.empty() && !palettes_.colours.empty()) {
        palettes_.step = previous.palettes_.step;
        palettes_.moved = previous.palettes_.moved;
        for (size_t s = 0; s < std::min(palettes_.drawn.size(), previous.palettes_.drawn.size()); ++s) {
            palettes_.from[s] = previous.palettes_.from[s];
            palettes_.drawn[s] = previous.palettes_.drawn[s];
        }
    }
    for (PaletteLink& link : palettes_.links)
        for (const PaletteLink& p : previous.palettes_.links) {
            const Stepped &mine = stepped_[link.channels[0]], &theirs = previous.stepped_[p.channels[0]];
            if (mine.effect == theirs.effect && mine.index == theirs.index)
                link.from = p.from;
        }
    for (Glide& g : glides_)
        for (const Glide& p : previous.glides_) {
            if (p.effect != g.effect || p.index != g.index)
                continue;
            const double base = g.base;
            g = p;
            g.base = base;
            if (base != p.base) {
                g.from = p.value;
                g.pending = true;
            }
        }
}

void ModulationPlan::cue(const std::vector<CueMacro>& macros)
{
    for (size_t i = 0; i < macroIds_.size(); ++i) {
        slots_[i].base = macroBases_[i];
        for (const CueMacro& m : macros)
            if (m.macro == macroIds_[i])
                slots_[i].base = m.value;
    }
}

void ModulationPlan::evaluate(double seconds, double beat, const AudioFrame& audio)
{
    for (Slot& slot : slots_) {
        slot.add = 0.0;
        slot.mul = 1.0;
    }
    const double dt = audioTime_ < 0.0 ? 0.0 : audio.time - audioTime_;
    audioTime_ = audio.time;
    // A resync or a jump onto another engine's timeline starts the cycles again from the beat.
    const double beats = beat - beat_;
    const bool jumped = beats < 0.0 || beats > 1.0;
    beat_ = beat;
    // Music is live input above -60 dB. Each ranged feature learns once a frame, whatever reads it.
    const bool music = audio.live && audio.values[size_t(AudioFeature::Level)] > 1e-3f;
    for (size_t f = 0; f < kAudioFeatures; ++f)
        if (ranged_[f]) {
            ranges_[f].learn(audio.values[f], dt, music);
            publishedRanges_[2 * f].store(float(ranges_[f].range().low), std::memory_order_relaxed);
            publishedRanges_[2 * f + 1].store(float(ranges_[f].range().high), std::memory_order_relaxed);
        }
    for (size_t i = 0; i < sources_.size(); ++i) {
        const Source& s = sources_[i];
        cycles_[i] = std::isnan(cycles_[i]) || jumped ? beat / s.beats : cycles_[i] + beats / s.beats;
        double v = s.kind == ModulatorSource::Lfo ? lfoAt(s.shape, cycles_[i] + s.phase, s.points) : double(audio.values[s.feature]);
        if (s.autoRange)
            v = ranges_[s.feature].map(v);
        v = std::clamp(v * s.sensitivity, 0.0, 1.0);
        if (s.kind == ModulatorSource::Audio && s.envelope == EnvelopePreset::None) {
            const double tau = v > sourceValues_[i] ? s.times.attack : s.times.release;
            if (tau > 0.0 && dt < 0.5)
                v = sourceValues_[i] + (v - sourceValues_[i]) * (1.0 - std::exp(-std::max(dt, 0.0) / tau));
        } else if (s.kind == ModulatorSource::Audio) {
            v = follow(i, v, dt, audio);
        }
        sourceValues_[i] = v;
        const double out = s.min + (s.max - s.min) * at(s.curve, v);
        Slot& slot = slots_[s.slot];
        if (s.combine == Combine::Add)
            slot.add += out;
        else
            slot.mul *= out;
    }
    glideSteps(beat);
    auto settle = [](Slot& slot) { slot.value = fit(slot, (slot.base + slot.add) * slot.mul); };
    for (size_t i = 0; i < macroIds_.size(); ++i)
        settle(slots_[i]);
    for (const Link& link : links_) {
        const double macro = slots_[link.macro].value;
        slots_[link.slot].add += link.min + (link.max - link.min) * at(link.curve, macro);
    }
    glidePalettes(beat);
    for (const Stepped& p : stepped_) {
        if (p.slot != kNone)
            slots_[p.slot].base = stepBase(p);
        if (p.derived != kNone)
            derived_[p.derived].base = stepBase(p);
    }
    for (size_t i = macroIds_.size(); i < slots_.size(); ++i)
        settle(slots_[i]);
    auto valueOf = [&](double base, size_t slot) { return slot == kNone ? base : slots_[slot].value; };
    for (Derived& d : derived_) {
        const double v = valueOf(d.base, d.slot) * valueOf(d.scaleBase, d.scaleSlot);
        d.rate = v;
        if (d.drive == ParamDrive::Phase)
            // A phase that starts now starts where time times its rate would be.
            d.value = d.started ? d.value + v * (seconds - derivedTime_) : v * seconds;
        else if (d.drive == ParamDrive::Beats)
            d.value = v > 0.0 ? beat / std::max(v, 0.25) : 0.0;
        else
            d.value = audio.values[std::min(size_t(std::max(v, 0.0) + 0.5), kAudioFeatures - 1)];
        d.started = true;
    }
    const double frame = std::max(seconds - derivedTime_, 0.0);
    for (Glide& g : glides_) {
        if (g.pending) {
            const double gap = g.changed < 0.0 ? 0.0 : seconds - g.changed;
            g.duration = gap > 2.5 * frame ? frame : std::clamp(gap, frame, 2.0 * frame);
            g.start = seconds - frame;
            g.changed = seconds;
            g.pending = false;
        }
        const double t = g.duration > 0.0 ? std::min((seconds - g.start) / g.duration, 1.0) : 1.0;
        if (!g.wraps) {
            g.value = g.from + (g.base - g.from) * t;
            continue;
        }
        const double half = (g.max - g.min) * 0.5;
        g.value = t >= 1.0 ? g.base : wrapInto(g.from + wrapInto(g.base - g.from, -half, half) * t, g.min, g.max);
    }
    derivedTime_ = seconds;
    for (Stepped& p : stepped_) {
        if (p.glide != kNone && !p.manual)
            glides_[p.glide].value = p.base;
        p.drawn = drawnBase(p);
    }

    for (size_t i = 0; i < slots_.size(); ++i)
        published_[i].store(float(slots_[i].value), std::memory_order_relaxed);
    for (size_t i = 0; i < sources_.size(); ++i)
        published_[slots_.size() + i].store(float(sourceValues_[i]), std::memory_order_relaxed);
    for (size_t i = 0; i < stepped_.size(); ++i)
        published_[slots_.size() + sources_.size() + i].store(float(stepped_[i].drawn), std::memory_order_relaxed);
    publishPalettes();
}

double ModulationPlan::follow(size_t source, double input, double dt, const AudioFrame& audio)
{
    const Source& s = sources_[source];
    EnvelopeState& state = envelopes_[source];
    state.value = sourceValues_[source];
    if (s.envelope == EnvelopePreset::Gate) {
        const size_t kick = size_t(AudioFeature::Kick);
        const bool drum = s.feature >= kick && s.feature < kick + kOnsetKinds;
        const auto onsets = drum ? std::optional(audio.onsets[s.feature - kick]) : std::nullopt;
        return followGate(state, input, onsets, std::max(dt, 0.0), s.times);
    }
    // After a gap in the audio the meter starts again from the feature.
    if (dt >= 0.5) {
        state.held = 0.0;
        return input;
    }
    return followMeter(state, input, std::max(dt, 0.0), s.times);
}

float ModulationPlan::opacity(uint64_t layer, float base) const
{
    for (const Opacity& o : opacities_)
        if (o.layer == layer)
            return float(slots_[o.slot].value);
    return base;
}

float ModulationPlan::text(uint64_t layer, TextParam param, float base) const
{
    for (const Text& t : texts_) {
        if (t.layer != layer || t.param != param)
            continue;
        const Slot& slot = slots_[t.slot];
        return float(fit(slot, (base + slot.add) * slot.mul));
    }
    return base;
}

void ModulationPlan::apply(const PlannedPass& pass, const std::vector<const Effect*>& effects,
                           std::vector<float>& values) const
{
    size_t offset = 0;
    for (const PlannedStage& stage : pass.stages) {
        const uint64_t effect = effects[stage.chainIndex]->id;
        auto put = [&](const auto& list, auto valueOf) {
            auto it = std::lower_bound(list.begin(), list.end(), effect,
                                       [](const auto& p, uint64_t id) { return p.effect < id; });
            for (; it != list.end() && it->effect == effect; ++it)
                if (offset + it->index < values.size())
                    values[offset + it->index] = float(valueOf(*it));
        };
        put(effectParams_, [&](const EffectParam& p) { return slots_[p.slot].value; });
        put(derived_, [](const Derived& d) { return d.value; });
        put(glides_, [](const Glide& g) { return g.value; });
        auto it = std::lower_bound(steppedByEffect_.begin(), steppedByEffect_.end(), effect,
                                   [&](size_t i, uint64_t id) { return stepped_[i].effect < id; });
        for (; it != steppedByEffect_.end() && stepped_[*it].effect == effect; ++it)
            if (offset + stepped_[*it].index < values.size())
                values[offset + stepped_[*it].index] = float(outputOf(stepped_[*it]));
        offset += stage.effect->params.size();
    }
}

bool ModulationPlan::drives(uint64_t effect) const
{
    auto listed = [&](const auto& list) {
        auto it = std::lower_bound(list.begin(), list.end(), effect,
                                   [](const auto& p, uint64_t id) { return p.effect < id; });
        return it != list.end() && it->effect == effect;
    };
    return listed(effectParams_) || listed(derived_) ||
           std::any_of(stepped_.begin(), stepped_.end(), [&](const Stepped& p) { return p.effect == effect; });
}

void ModulationPlan::rates(const PlannedPass& pass, const std::vector<const Effect*>& effects,
                           std::vector<float>& values) const
{
    size_t offset = 0;
    for (const PlannedStage& stage : pass.stages) {
        const uint64_t effect = effects[stage.chainIndex]->id;
        auto it = std::lower_bound(derived_.begin(), derived_.end(), effect,
                                   [](const Derived& d, uint64_t id) { return d.effect < id; });
        for (; it != derived_.end() && it->effect == effect; ++it)
            if (offset + it->index < values.size())
                values[offset + it->index] = float(it->rate);
        offset += stage.effect->params.size();
    }
}

std::vector<ModulationPlan::MacroValue> ModulationPlan::macros() const
{
    std::vector<MacroValue> values;
    for (size_t i = 0; i < macroIds_.size(); ++i)
        values.push_back({macroIds_[i], published_[i].load(std::memory_order_relaxed)});
    return values;
}

std::vector<ModulationPlan::ParamValue> ModulationPlan::params() const
{
    std::vector<ParamValue> values;
    for (size_t i = 0; i < paramRefs_.size(); ++i)
        values.push_back({paramRefs_[i], published_[macroIds_.size() + i].load(std::memory_order_relaxed)});
    for (size_t i = 0; i < stepped_.size(); ++i)
        if (stepped_[i].slot == kNone)
            values.push_back({stepped_[i].param,
                              published_[slots_.size() + sources_.size() + i].load(std::memory_order_relaxed)});
    return values;
}

double ModulationPlan::outputOf(const Stepped& p) const
{
    if (p.slot != kNone)
        return slots_[p.slot].value;
    if (p.derived != kNone)
        return derived_[p.derived].value;
    return p.drawn;
}

std::vector<ModulationPlan::MacroValue> ModulationPlan::modulators() const
{
    std::vector<MacroValue> values;
    for (size_t i = 0; i < sources_.size(); ++i)
        values.push_back({sources_[i].id, published_[slots_.size() + i].load(std::memory_order_relaxed)});
    return values;
}

std::vector<ModulationPlan::RangeValue> ModulationPlan::ranges() const
{
    std::vector<RangeValue> values;
    for (const Source& s : sources_)
        if (s.autoRange)
            values.push_back({s.id, publishedRanges_[2 * s.feature].load(std::memory_order_relaxed),
                              publishedRanges_[2 * s.feature + 1].load(std::memory_order_relaxed)});
    return values;
}

double ModulationPlan::fit(const Slot& slot, double value)
{
    if (slot.options)
        return optionValue(*slot.options, optionIndex(*slot.options, value));
    if (slot.wraps && (value < slot.min || value > slot.max))
        return wrapInto(value, slot.min, slot.max);
    return std::clamp(value, slot.min, slot.max);
}

ModulationPlan::Lut ModulationPlan::lut(const Curve& curve)
{
    Lut table{};
    for (size_t i = 0; i < kCurvePoints; ++i)
        table[i] = float(curveAt(curve, double(i) / double(kCurvePoints - 1)));
    return table;
}

double ModulationPlan::at(const Lut& lut, double x)
{
    const double f = std::clamp(x, 0.0, 1.0) * double(kCurvePoints - 1);
    const size_t i = std::min(size_t(f), kCurvePoints - 2);
    const double t = f - double(i);
    return lut[i] + (lut[i + 1] - lut[i]) * t;
}

} // namespace azl
