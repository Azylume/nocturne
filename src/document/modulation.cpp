#include "document/modulation_edits.hpp"

#include "effects/effect_registry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

namespace azl {

namespace {

constexpr std::array<const char*, 13> kFeatureNames = {"level", "sub",  "bass",  "lowMid",  "mid",     "highMid", "high",
                                                       "kick",  "snare", "hat", "lowBand", "midBand", "highBand"};
constexpr std::array<const char*, 2> kSourceNames = {"lfo", "audio"};
constexpr std::array<const char*, 6> kShapeNames = {"sine", "saw", "square", "ramp", "random", "custom"};
constexpr std::array<const char*, 2> kCombineNames = {"add", "multiply"};
constexpr std::array<const char*, 7> kEnvelopeNames = {"none",    "meter", "punchy", "smooth",
                                                       "breathe", "gate",  "custom"};

struct PresetTimes {
    double attack;
    double hold;
    double release;
};

// Tuned offline on music against an IEC 60268-10 Type II PPM (ticket 77). Meter falls 12 dB/s, between the PPM's 8.6
// and DIN's 13.3; Punchy keeps the raw feature's pumping with fewer flickers; Smooth seldom turns back within a fall;
// Breathe follows phrases. Gate stays full for two or three frames and is gone after 220 ms.
std::optional<PresetTimes> presetTimes(EnvelopePreset preset)
{
    switch (preset) {
    case EnvelopePreset::Meter:
        return PresetTimes{10, 40, 500};
    case EnvelopePreset::Punchy:
        return PresetTimes{5, 40, 150};
    case EnvelopePreset::Smooth:
        return PresetTimes{60, 0, 800};
    case EnvelopePreset::Breathe:
        return PresetTimes{300, 0, 2000};
    case EnvelopePreset::Gate:
        return PresetTimes{0, 40, 180};
    default:
        return std::nullopt;
    }
}

template <typename E, size_t N>
E named(const std::array<const char*, N>& names, const std::string& name, const char* what)
{
    for (size_t i = 0; i < N; ++i)
        if (name == names[i])
            return E(i);
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

std::optional<size_t> indexField(const nlohmann::json& j)
{
    if (!j.contains("index") || j["index"].is_null())
        return std::nullopt;
    return field<size_t>(j, "index");
}

nlohmann::json indexJson(const std::optional<size_t>& index)
{
    return index ? nlohmann::json(*index) : nlohmann::json(nullptr);
}

bool inGroup(const Composition& composition, uint64_t group, uint64_t layer)
{
    const Layer* g = layerById(composition, group);
    if (!g)
        return false;
    return layer == group ||
           std::any_of(g->layers.begin(), g->layers.end(), [&](const Layer& l) { return l.id == layer; });
}

bool finite(double v)
{
    return std::isfinite(v);
}

void checkCurve(const Curve& curve)
{
    const auto& p = curve.points;
    if (!std::all_of(p.begin(), p.end(), finite) || p[0] < 0.0 || p[0] > 1.0 || p[2] < 0.0 || p[2] > 1.0 ||
        p[1] < -1.0 || p[1] > 2.0 || p[3] < -1.0 || p[3] > 2.0)
        throw ProtocolError("a curve's control points need x within 0..1 and y within -1..2");
}

void checkPoints(const Modulator& m)
{
    if (m.points.size() > kMaxLfoPoints)
        throw ProtocolError("a custom LFO holds at most " + std::to_string(kMaxLfoPoints) + " points");
    if (m.shape == LfoShape::Custom && m.points.empty())
        throw ProtocolError("a custom LFO needs at least one point");
    double last = -1.0;
    for (const LfoPoint& point : m.points) {
        if (!(point.phase >= 0.0 && point.phase < 1.0) || !(point.value >= 0.0 && point.value <= 1.0))
            throw ProtocolError("an LFO point needs its phase within 0..1 (below 1) and its value within 0..1");
        if (point.phase <= last)
            throw ProtocolError("an LFO's points must be in phase order, no two at the same phase");
        last = point.phase;
    }
}

// Offsets and factors reach at most one span of the target's range either way.
void checkSpan(double min, double max, std::pair<double, double> range)
{
    const double span = range.second - range.first;
    if (!finite(min) || !finite(max) || std::abs(min) > span || std::abs(max) > span)
        throw ProtocolError("min and max must be within -" + std::to_string(span) + ".." + std::to_string(span));
}

// Targets whose layer or effect is gone (a show may hold them) are kept and ignored until it returns; a parameter its
// effect does not have is an error unless `missingOk`.
std::optional<std::pair<double, double>> checkParam(const Composition& composition, const ParamRef& param,
                                                    bool missingOk)
{
    const bool palette =
        param.layer == 0 && param.effect == 0 && (param.param == kPalettePosition || paletteHueSlot(param.param));
    if (palette) {
        const auto range = paramRange(composition, param);
        if (!range && !missingOk)
            throw ProtocolError("no palette colour for " + param.param);
        return range;
    }
    if (param.effect == 0 && param.param != "opacity" && !textParamFromName(param.param))
        throw ProtocolError("a layer's own parameters are opacity and its text clip's textSize, textX, textY, "
                            "textRed, textGreen, textBlue and textAlpha; the composition's also palettePosition and "
                            "paletteHueN");
    if (param.effect == 0)
        return paramRange(composition, param);
    const Effect* effect = effectById(composition, param.effect);
    if (!effect)
        return std::nullopt;
    const auto range = paramRange(composition, param);
    if (!range && !missingOk)
        throw ProtocolError(effect->type + " has no parameter " + param.param);
    return range;
}

void checkTarget(const Composition& composition, const Macro& macro, const MacroTarget& target, bool missingOk = false)
{
    if (const auto range = checkParam(composition, target.param, missingOk))
        checkSpan(target.min, target.max, *range);
    checkCurve(target.curve);
    uint64_t layer = target.param.layer;
    if (target.param.effect && !effectById(composition, target.param.effect, &layer))
        return;
    if (macro.group && layerById(composition, layer) && !inGroup(composition, macro.group, layer))
        throw ProtocolError("a group's macro drives only the group and its layers");
}

void checkMacro(const Composition& composition, const std::vector<Macro>& macros, const Macro& macro,
                bool missingOk = false)
{
    if (!(macro.value >= 0.0 && macro.value <= 1.0))
        throw ProtocolError("a macro's value must be within 0..1");
    if (macro.group) {
        const Layer* group = layerById(composition, macro.group);
        if (group && !group->group)
            throw ProtocolError("layer " + std::to_string(macro.group) + " is not a group");
    }
    const auto owned = std::count_if(macros.begin(), macros.end(), [&](const Macro& m) { return m.group == macro.group; });
    if (size_t(owned) > kMaxMacros)
        throw ProtocolError("the composition and each group hold at most " + std::to_string(kMaxMacros) + " macros");
    if (macro.targets.size() > kMaxMacroTargets)
        throw ProtocolError("a macro drives at most " + std::to_string(kMaxMacroTargets) + " parameters");
    for (const MacroTarget& target : macro.targets)
        checkTarget(composition, macro, target, missingOk);
}

void checkModulator(const Composition& composition, const Modulator& m, bool missingOk = false)
{
    if (!(m.beats >= kMinLfoBeats && m.beats <= kMaxLfoBeats))
        throw ProtocolError("an LFO's period must be within 1/16..64 beats");
    if (!(m.phase >= 0.0 && m.phase <= 1.0))
        throw ProtocolError("an LFO's phase must be within 0..1");
    named<int>(kFeatureNames, m.feature, "audio feature");
    for (const double ms : {m.attack, m.release, m.hold})
        if (!(ms >= 0.0 && ms <= kMaxEnvelopeMs))
            throw ProtocolError("attack, hold and release must be within 0..10000 ms");
    checkCurve(m.curve);
    checkPoints(m);
    if (m.target.macro) {
        checkSpan(m.min, m.max, {0.0, 1.0});
        return;
    }
    if (const auto range = checkParam(composition, m.target.param, missingOk))
        checkSpan(m.min, m.max, *range);
}

template <typename T>
uint64_t freshId(const std::vector<T>& items)
{
    uint64_t next = 1;
    for (const T& item : items)
        next = std::max(next, item.id + 1);
    return next;
}

template <typename T>
typename std::vector<T>::iterator findById(std::vector<T>& items, uint64_t id, const char* what)
{
    auto it = std::find_if(items.begin(), items.end(), [id](const T& item) { return item.id == id; });
    if (it == items.end())
        throw ProtocolError(std::string("no ") + what + " " + std::to_string(id));
    return it;
}

template <typename T>
void completeIds(std::vector<T>& items, const char* what)
{
    std::set<uint64_t> used;
    for (const T& item : items)
        if (item.id && !used.insert(item.id).second)
            throw ProtocolError(std::string(what) + " id " + std::to_string(item.id) + " is in use");
    uint64_t next = used.empty() ? 1 : *used.rbegin() + 1;
    for (T& item : items)
        if (!item.id)
            item.id = next++;
}

void completeMacro(Macro& macro)
{
    if (macro.name.empty())
        macro.name = "Macro " + std::to_string(macro.id);
}

// Times that are not its preset's make the envelope Custom.
void settleEnvelope(Modulator& m)
{
    const auto times = presetTimes(m.envelope);
    if (times && (m.attack != times->attack || m.hold != times->hold || m.release != times->release))
        m.envelope = EnvelopePreset::Custom;
}

void completeModulator(Modulator& modulator)
{
    if (modulator.name.empty())
        modulator.name = "Modulator " + std::to_string(modulator.id);
    settleEnvelope(modulator);
}

size_t insertIndex(const std::optional<size_t>& index, size_t size, const char* what)
{
    const size_t at = index.value_or(size);
    if (at > size)
        throw ProtocolError(std::string(what) + " index out of range");
    return at;
}

} // namespace

const Layer* layerById(const Composition& composition, uint64_t id)
{
    const Layer* found = nullptr;
    forEachLayer(composition.layers, [&](const Layer& layer) {
        if (layer.id == id)
            found = &layer;
    });
    return found;
}

double curveAt(const Curve& curve, double x)
{
    x = std::clamp(x, 0.0, 1.0);
    const auto& p = curve.points;
    auto bezier = [](double a, double b, double t) {
        const double u = 1.0 - t;
        return 3.0 * u * u * t * a + 3.0 * u * t * t * b + t * t * t;
    };
    double low = 0.0;
    double high = 1.0;
    double t = x;
    for (int i = 0; i < 40; ++i) {
        const double at = bezier(p[0], p[2], t);
        if (std::abs(at - x) < 1e-7)
            break;
        (at < x ? low : high) = t;
        t = 0.5 * (low + high);
    }
    return bezier(p[1], p[3], t);
}

namespace {

double customAt(const std::vector<LfoPoint>& points, double p)
{
    if (points.empty())
        return 0.0;
    auto next = std::upper_bound(points.begin(), points.end(), p,
                                 [](double at, const LfoPoint& point) { return at < point.phase; });
    const LfoPoint& a = next == points.begin() ? points.back() : *std::prev(next);
    const LfoPoint& b = next == points.end() ? points.front() : *next;
    const double span = b.phase - a.phase + (b.phase > a.phase ? 0.0 : 1.0);
    const double t = std::fmod(p - a.phase + 1.0, 1.0) / span;
    const double eased = a.smooth ? t * t * (3.0 - 2.0 * t) : t;
    return a.value + (b.value - a.value) * eased;
}

} // namespace

double lfoAt(LfoShape shape, double phase, const std::vector<LfoPoint>& points)
{
    const double p = phase - std::floor(phase);
    switch (shape) {
    case LfoShape::Sine:
        return 0.5 + 0.5 * std::cos(2.0 * M_PI * p);
    case LfoShape::Saw:
        return 1.0 - p;
    case LfoShape::Square:
        return p < 0.5 ? 1.0 : 0.0;
    case LfoShape::Ramp:
        return p;
    case LfoShape::Random: {
        uint64_t x = uint64_t(int64_t(std::floor(phase))) + 0x9e3779b97f4a7c15ull;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
        x ^= x >> 31;
        return double(x >> 11) / double(1ull << 53);
    }
    case LfoShape::Custom:
        return customAt(points, p);
    }
    return 0.0;
}

const char* envelopePresetName(EnvelopePreset preset)
{
    return kEnvelopeNames[size_t(preset)];
}

std::optional<EnvelopePreset> envelopePresetFromName(std::string_view name)
{
    for (size_t i = 0; i < kEnvelopeNames.size(); ++i)
        if (name == kEnvelopeNames[i])
            return EnvelopePreset(i);
    return std::nullopt;
}

void setEnvelope(Modulator& modulator, EnvelopePreset preset)
{
    modulator.envelope = preset;
    const auto times = presetTimes(preset);
    if (!times)
        return;
    modulator.attack = times->attack;
    modulator.hold = times->hold;
    modulator.release = times->release;
}

bool validBandEdges(const std::array<double, 2>& edges)
{
    return edges[0] >= kMinBandEdge && edges[1] <= kMaxBandEdge && edges[1] >= edges[0] * kMinMidBandRatio;
}

SetMacros setEnvelopes(const Composition& composition, EnvelopePreset preset)
{
    SetMacros edit{composition.macros, composition.modulators};
    for (Modulator& m : edit.modulators)
        if (m.source == ModulatorSource::Audio)
            setEnvelope(m, preset);
    return edit;
}

std::optional<size_t> paletteHueSlot(std::string_view param)
{
    if (!param.starts_with(kPaletteHue) || param.size() == kPaletteHue.size())
        return std::nullopt;
    size_t slot = 0;
    for (const char c : param.substr(kPaletteHue.size())) {
        if (c < '0' || c > '9' || slot > kMaxPaletteColours)
            return std::nullopt;
        slot = slot * 10 + size_t(c - '0');
    }
    return slot >= 1 ? std::optional(slot - 1) : std::nullopt;
}

std::string paletteHueParam(size_t slot)
{
    return std::string(kPaletteHue) + std::to_string(slot + 1);
}

std::optional<std::pair<double, double>> paramRange(const Composition& composition, const ParamRef& param)
{
    if (param.layer == 0 && param.effect == 0 && param.param == "opacity")
        return std::pair(0.0, 1.0);
    if (param.layer == 0 && param.effect == 0 && param.param == kPalettePosition)
        return composition.palettes.empty() ? std::nullopt : std::optional(std::pair(0.0, 1.0));
    if (const auto slot = param.layer == 0 && param.effect == 0 ? paletteHueSlot(param.param) : std::nullopt)
        return std::any_of(composition.palettes.begin(), composition.palettes.end(),
                           [&](const ColourPalette& p) { return *slot < p.colours.size(); })
                   ? std::optional(std::pair(0.0, 1.0))
                   : std::nullopt;
    if (param.effect == 0 && !layerById(composition, param.layer))
        return std::nullopt;
    if (param.effect == 0 && param.param == "opacity")
        return std::pair(0.0, 1.0);
    if (param.effect == 0) {
        const auto text = textParamFromName(param.param);
        return text ? std::optional(textParamRange(*text)) : std::nullopt;
    }
    const ParamDef* def = paramDef(composition, param);
    return def ? std::optional(std::pair(def->min, def->max)) : std::nullopt;
}

const ParamDef* paramDef(const Composition& composition, const ParamRef& param)
{
    const Effect* effect = param.effect ? effectById(composition, param.effect) : nullptr;
    const EffectDef* def = effect ? findEffect(effect->type) : nullptr;
    if (!def)
        return nullptr;
    for (const ParamDef& p : def->params)
        if (p.name == param.param)
            return &p;
    return nullptr;
}

Edit apply(Composition& composition, AddMacro& edit)
{
    const size_t index = insertIndex(edit.index, composition.macros.size(), "macro");
    Macro macro = edit.macro;
    if (macro.id && std::any_of(composition.macros.begin(), composition.macros.end(),
                                [&](const Macro& m) { return m.id == macro.id; }))
        throw ProtocolError("macro id " + std::to_string(macro.id) + " is in use");
    if (!macro.id)
        macro.id = freshId(composition.macros);
    completeMacro(macro);
    std::vector<Macro> macros = composition.macros;
    macros.insert(macros.begin() + std::ptrdiff_t(index), macro);
    checkMacro(composition, macros, macro);
    composition.macros = std::move(macros);
    edit.index = index;
    edit.macro = macro;
    return RemoveMacro{macro.id};
}

Edit apply(Composition& composition, RemoveMacro& edit)
{
    auto it = findById(composition.macros, edit.macro, "macro");
    AddMacro inverse{size_t(it - composition.macros.begin()), *it};
    composition.macros.erase(it);
    return inverse;
}

Edit apply(Composition& composition, SetMacroValue& edit)
{
    Macro& macro = *findById(composition.macros, edit.macro, "macro");
    if (!(edit.value >= 0.0 && edit.value <= 1.0))
        throw ProtocolError("a macro's value must be within 0..1");
    const SetMacroValue inverse{macro.id, macro.value};
    macro.value = edit.value;
    return inverse;
}

Edit apply(Composition& composition, AddMacroTarget& edit)
{
    Macro& macro = *findById(composition.macros, edit.macro, "macro");
    const size_t index = insertIndex(edit.index, macro.targets.size(), "target");
    if (macro.targets.size() >= kMaxMacroTargets)
        throw ProtocolError("a macro drives at most " + std::to_string(kMaxMacroTargets) + " parameters");
    checkTarget(composition, macro, edit.target);
    macro.targets.insert(macro.targets.begin() + std::ptrdiff_t(index), edit.target);
    edit.index = index;
    return RemoveMacroTarget{macro.id, index};
}

Edit apply(Composition& composition, RemoveMacroTarget& edit)
{
    Macro& macro = *findById(composition.macros, edit.macro, "macro");
    if (edit.index >= macro.targets.size())
        throw ProtocolError("target index out of range");
    AddMacroTarget inverse{macro.id, edit.index, macro.targets[edit.index]};
    macro.targets.erase(macro.targets.begin() + std::ptrdiff_t(edit.index));
    return inverse;
}

Edit apply(Composition& composition, SetMacroTarget& edit)
{
    Macro& macro = *findById(composition.macros, edit.macro, "macro");
    if (edit.index >= macro.targets.size())
        throw ProtocolError("target index out of range");
    checkTarget(composition, macro, edit.target);
    SetMacroTarget inverse{macro.id, edit.index, macro.targets[edit.index]};
    macro.targets[edit.index] = edit.target;
    return inverse;
}

Edit apply(Composition& composition, AddModulator& edit)
{
    if (composition.modulators.size() >= kMaxModulators)
        throw ProtocolError("a composition holds at most " + std::to_string(kMaxModulators) + " modulators");
    const size_t index = insertIndex(edit.index, composition.modulators.size(), "modulator");
    Modulator modulator = edit.modulator;
    if (modulator.id && std::any_of(composition.modulators.begin(), composition.modulators.end(),
                                    [&](const Modulator& m) { return m.id == modulator.id; }))
        throw ProtocolError("modulator id " + std::to_string(modulator.id) + " is in use");
    if (!modulator.id)
        modulator.id = freshId(composition.modulators);
    completeModulator(modulator);
    checkModulator(composition, modulator);
    composition.modulators.insert(composition.modulators.begin() + std::ptrdiff_t(index), modulator);
    edit.index = index;
    edit.modulator = modulator;
    return RemoveModulator{modulator.id};
}

Edit apply(Composition& composition, RemoveModulator& edit)
{
    auto it = findById(composition.modulators, edit.modulator, "modulator");
    AddModulator inverse{size_t(it - composition.modulators.begin()), *it};
    composition.modulators.erase(it);
    return inverse;
}

Edit apply(Composition& composition, SetModulator& edit)
{
    Modulator& modulator = *findById(composition.modulators, edit.modulator.id, "modulator");
    Modulator changed = edit.modulator;
    if (changed.name.empty())
        changed.name = modulator.name;
    settleEnvelope(changed);
    checkModulator(composition, changed);
    SetModulator inverse{modulator};
    modulator = changed;
    edit.modulator = changed;
    return inverse;
}

Edit apply(Composition& composition, SetMacros& edit)
{
    std::vector<Macro> macros = edit.macros;
    std::vector<Modulator> modulators = edit.modulators;
    if (modulators.size() > kMaxModulators)
        throw ProtocolError("a composition holds at most " + std::to_string(kMaxModulators) + " modulators");
    completeIds(macros, "macro");
    completeIds(modulators, "modulator");
    for (Macro& macro : macros) {
        completeMacro(macro);
        checkMacro(composition, macros, macro, true);
    }
    for (Modulator& modulator : modulators) {
        completeModulator(modulator);
        checkModulator(composition, modulator, true);
    }
    SetMacros inverse{composition.macros, composition.modulators};
    composition.macros = macros;
    composition.modulators = modulators;
    edit.macros = std::move(macros);
    edit.modulators = std::move(modulators);
    return inverse;
}

std::optional<SetMacros> dropTargets(Composition& composition, const std::set<uint64_t>& layers,
                                     const std::set<uint64_t>& effects, std::vector<std::string>& dropped)
{
    auto on = [&](const ParamRef& param) {
        return param.effect ? effects.contains(param.effect) : layers.contains(param.layer);
    };
    auto where = [](const ParamRef& param) {
        return (param.effect ? "effect " + std::to_string(param.effect) : "layer " + std::to_string(param.layer)) +
               " " + param.param;
    };
    SetMacros restore{composition.macros, composition.modulators};
    for (Macro& macro : composition.macros)
        std::erase_if(macro.targets, [&](const MacroTarget& target) {
            if (!on(target.param))
                return false;
            dropped.push_back("macro " + macro.name + "'s target " + where(target.param));
            return true;
        });
    std::erase_if(composition.modulators, [&](const Modulator& modulator) {
        if (modulator.target.macro || !on(modulator.target.param))
            return false;
        dropped.push_back("modulator " + modulator.name + " on " + where(modulator.target.param));
        return true;
    });
    if (composition.macros == restore.macros && composition.modulators == restore.modulators)
        return std::nullopt;
    return restore;
}

nlohmann::json toJson(const AddMacro& e)
{
    return {{"op", "addMacro"}, {"index", indexJson(e.index)}, {"macro", e.macro}};
}

nlohmann::json toJson(const RemoveMacro& e)
{
    return {{"op", "removeMacro"}, {"macro", e.macro}};
}

nlohmann::json toJson(const SetMacroValue& e)
{
    return {{"op", "setMacroValue"}, {"macro", e.macro}, {"value", e.value}};
}

nlohmann::json toJson(const AddMacroTarget& e)
{
    return {{"op", "addMacroTarget"}, {"macro", e.macro}, {"index", indexJson(e.index)}, {"target", e.target}};
}

nlohmann::json toJson(const RemoveMacroTarget& e)
{
    return {{"op", "removeMacroTarget"}, {"macro", e.macro}, {"index", e.index}};
}

nlohmann::json toJson(const SetMacroTarget& e)
{
    return {{"op", "setMacroTarget"}, {"macro", e.macro}, {"index", e.index}, {"target", e.target}};
}

nlohmann::json toJson(const AddModulator& e)
{
    return {{"op", "addModulator"}, {"index", indexJson(e.index)}, {"modulator", e.modulator}};
}

nlohmann::json toJson(const RemoveModulator& e)
{
    return {{"op", "removeModulator"}, {"modulator", e.modulator}};
}

nlohmann::json toJson(const SetModulator& e)
{
    return {{"op", "setModulator"}, {"modulator", e.modulator}};
}

nlohmann::json toJson(const SetMacros& e)
{
    return {{"op", "setMacros"}, {"macros", e.macros}, {"modulators", e.modulators}};
}

std::optional<Edit> modulationEditFromJson(const std::string& op, const nlohmann::json& j)
{
    if (op == "addMacro")
        return AddMacro{indexField(j), j.contains("macro") ? field<Macro>(j, "macro") : Macro{}};
    if (op == "removeMacro")
        return RemoveMacro{field<uint64_t>(j, "macro")};
    if (op == "setMacroValue")
        return SetMacroValue{field<uint64_t>(j, "macro"), field<double>(j, "value")};
    if (op == "addMacroTarget")
        return AddMacroTarget{field<uint64_t>(j, "macro"), indexField(j), field<MacroTarget>(j, "target")};
    if (op == "removeMacroTarget")
        return RemoveMacroTarget{field<uint64_t>(j, "macro"), field<size_t>(j, "index")};
    if (op == "setMacroTarget")
        return SetMacroTarget{field<uint64_t>(j, "macro"), field<size_t>(j, "index"), field<MacroTarget>(j, "target")};
    if (op == "addModulator")
        return AddModulator{indexField(j), field<Modulator>(j, "modulator")};
    if (op == "removeModulator")
        return RemoveModulator{field<uint64_t>(j, "modulator")};
    if (op == "setModulator")
        return SetModulator{field<Modulator>(j, "modulator")};
    if (op == "setMacros")
        return SetMacros{field<std::vector<Macro>>(j, "macros"), field<std::vector<Modulator>>(j, "modulators")};
    return std::nullopt;
}

void to_json(nlohmann::json& j, const Curve& curve)
{
    j = curve.points;
}

void from_json(const nlohmann::json& j, Curve& curve)
{
    curve.points = j.get<std::array<double, 4>>();
}

void to_json(nlohmann::json& j, const LfoPoint& point)
{
    j = {{"phase", point.phase}, {"value", point.value}};
    if (point.smooth)
        j["smooth"] = true;
}

void from_json(const nlohmann::json& j, LfoPoint& point)
{
    point = LfoPoint{};
    point.phase = j.at("phase").get<double>();
    point.value = j.at("value").get<double>();
    point.smooth = j.value("smooth", false);
}

void to_json(nlohmann::json& j, const ParamRef& param)
{
    if (param.effect)
        j = {{"effect", param.effect}, {"param", param.param}};
    else
        j = {{"layer", param.layer}, {"param", param.param}};
}

// A layer's opacity needs only the layer. The layer an older target gave next to its effect is dropped.
void from_json(const nlohmann::json& j, ParamRef& param)
{
    param.effect = j.value("effect", uint64_t(0));
    param.layer = param.effect ? 0 : j.at("layer").get<uint64_t>();
    param.param = j.value("param", std::string(param.effect ? "" : "opacity"));
}

void to_json(nlohmann::json& j, const MacroTarget& target)
{
    j = target.param;
    j["curve"] = target.curve;
    j["min"] = target.min;
    j["max"] = target.max;
}

void from_json(const nlohmann::json& j, MacroTarget& target)
{
    target = MacroTarget{};
    target.param = j.get<ParamRef>();
    target.curve = j.value("curve", target.curve);
    target.min = j.value("min", target.min);
    target.max = j.value("max", target.max);
}

void to_json(nlohmann::json& j, const Macro& macro)
{
    j = {{"id", macro.id},
         {"name", macro.name},
         {"group", macro.group},
         {"value", macro.value},
         {"targets", macro.targets}};
}

void from_json(const nlohmann::json& j, Macro& macro)
{
    macro = Macro{};
    macro.id = j.value("id", uint64_t(0));
    macro.name = j.value("name", std::string());
    macro.group = j.value("group", uint64_t(0));
    macro.value = j.value("value", 0.0);
    macro.targets = j.value("targets", std::vector<MacroTarget>{});
}

void to_json(nlohmann::json& j, const Modulator& m)
{
    nlohmann::json target = m.target.macro ? nlohmann::json{{"macro", m.target.macro}} : nlohmann::json(m.target.param);
    j = {{"id", m.id},
         {"name", m.name},
         {"source", kSourceNames[size_t(m.source)]},
         {"shape", kShapeNames[size_t(m.shape)]},
         {"beats", m.beats},
         {"phase", m.phase},
         {"feature", m.feature},
         {"target", target},
         {"combine", kCombineNames[size_t(m.combine)]},
         {"curve", m.curve},
         {"min", m.min},
         {"max", m.max}};
    if (m.attack > 0.0)
        j["attack"] = m.attack;
    if (m.release > 0.0)
        j["release"] = m.release;
    if (m.hold > 0.0)
        j["hold"] = m.hold;
    if (m.envelope != EnvelopePreset::None)
        j["envelope"] = kEnvelopeNames[size_t(m.envelope)];
    if (!m.points.empty())
        j["points"] = m.points;
}

// Missing fields take their defaults: {"target": {"macro": 1}} is a sine over one beat.
void from_json(const nlohmann::json& j, Modulator& m)
{
    m = Modulator{};
    m.id = j.value("id", uint64_t(0));
    m.name = j.value("name", std::string());
    m.source = named<ModulatorSource>(kSourceNames, j.value("source", std::string("lfo")), "modulator source");
    m.shape = named<LfoShape>(kShapeNames, j.value("shape", std::string("sine")), "LFO shape");
    m.points = j.value("points", m.points);
    m.beats = j.value("beats", m.beats);
    m.phase = j.value("phase", m.phase);
    m.feature = j.value("feature", m.feature);
    m.envelope = named<EnvelopePreset>(kEnvelopeNames, j.value("envelope", std::string("none")), "envelope");
    const PresetTimes times = presetTimes(m.envelope).value_or(PresetTimes{0, 0, 0});
    m.attack = j.value("attack", times.attack);
    m.hold = j.value("hold", times.hold);
    m.release = j.value("release", times.release);
    const nlohmann::json& target = j.at("target");
    if (target.contains("macro"))
        m.target.macro = target.at("macro").get<uint64_t>();
    else
        m.target.param = target.get<ParamRef>();
    m.combine = named<Combine>(kCombineNames, j.value("combine", std::string("add")), "combine mode");
    m.curve = j.value("curve", m.curve);
    m.min = j.value("min", m.min);
    m.max = j.value("max", m.max);
}

} // namespace azl
