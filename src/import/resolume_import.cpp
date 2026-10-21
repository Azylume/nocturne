#include "import/resolume_import.hpp"

#include "document/composition_file.hpp"
#include "document/mapping_edits.hpp"
#include "effects/effect_registry.hpp"
#include "media/media_library.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <sstream>

namespace azl {

namespace fs = std::filesystem;

namespace {

std::optional<double> number(const std::string& text)
{
    if (text.empty())
        return std::nullopt;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size())
        return std::nullopt;
    return value;
}

std::string fmt(double value)
{
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(4);
    out << value;
    std::string text = out.str();
    text.erase(text.find_last_not_of('0') + 1);
    if (text.back() == '.')
        text.pop_back();
    return text == "-0" ? "0" : text;
}

bool isParam(const XmlNode& node)
{
    return node.name.rfind("Param", 0) == 0 && node.has("name");
}

const XmlNode* param(const XmlNode* params, const std::string& name)
{
    if (!params)
        return nullptr;
    for (const auto& c : params->children)
        if (isParam(*c) && c->attr("name") == name)
            return c.get();
    return nullptr;
}

std::string paramText(const XmlNode* params, const std::string& name, const std::string& fallback = {})
{
    const XmlNode* p = param(params, name);
    return p && !p->attr("value").empty() ? p->attr("value") : fallback;
}

double paramValue(const XmlNode* params, const std::string& name, double fallback)
{
    const XmlNode* p = param(params, name);
    return p ? number(p->attr("value")).value_or(fallback) : fallback;
}

// The source animating a parameter, empty when it holds a fixed value.
std::string animation(const XmlNode& p)
{
    for (const auto& c : p.children)
        if (c->name.rfind("PhaseSource", 0) == 0 && c->name != "PhaseSourceStatic")
            return c->name;
    return {};
}

bool atDefault(const XmlNode& p)
{
    if (!p.has("value"))
        return true;
    const std::string value = p.attr("value");
    if (!p.has("default"))
        return value.empty();
    const std::optional<double> a = number(value);
    const std::optional<double> b = number(p.attr("default"));
    if (a && b)
        return std::abs(*a - *b) < 1e-9;
    return value == p.attr("default");
}

std::string describeParam(const XmlNode& p)
{
    std::string text = p.attr("name") + " = " + p.attr("value");
    if (p.has("default"))
        text += " (default " + p.attr("default") + ")";
    return text;
}

std::string lower(std::string text)
{
    std::erase_if(text, [](char c) { return c == ' ' || c == '_'; });
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

std::optional<BlendMode> blendFrom(const std::string& resolumeType)
{
    static const std::map<std::string, BlendMode> modes{
        {"alpha", BlendMode::Normal},       {"add", BlendMode::Add},           {"screen", BlendMode::Screen},
        {"multiply", BlendMode::Multiply},  {"difference", BlendMode::Difference},
        {"subtract", BlendMode::Subtract},  {"exclusion", BlendMode::Exclusion}, {"lighten", BlendMode::Lighten},
        {"darken", BlendMode::Darken},      {"overlay", BlendMode::Overlay},   {"hardlight", BlendMode::HardLight},
        {"softlight", BlendMode::SoftLight}, {"dodge", BlendMode::Dodge},      {"colordodge", BlendMode::Dodge},
        {"burn", BlendMode::Burn},          {"colorburn", BlendMode::Burn}};
    auto it = modes.find(lower(resolumeType));
    if (it == modes.end())
        return std::nullopt;
    return it->second;
}

const XmlNode* mixerPass(const XmlNode* mixer)
{
    return mixer ? mixer->child("RenderPass") : nullptr;
}

std::map<std::string, double> defaultParams(const std::string& type)
{
    std::map<std::string, double> params;
    if (const EffectDef* def = findEffect(type))
        for (const ParamDef& p : def->params)
            params[p.name] = p.defaultValue;
    return params;
}

double clampParam(const std::string& type, const std::string& name, double value)
{
    if (const EffectDef* def = findEffect(type))
        for (const ParamDef& p : def->params)
            if (p.name == name)
                return fitParam(p, value);
    return value;
}

// The effect's parameters that differ from the defaults, e.g. "colorize hue 0.096 saturation 0.137".
std::string describeEffect(const Effect& effect)
{
    std::string text = effect.type;
    const std::map<std::string, double> defaults = defaultParams(effect.type);
    for (const auto& [name, value] : effect.params)
        if (auto d = defaults.find(name); d == defaults.end() || std::abs(d->second - value) > 1e-9)
            text += " " + name + " " + fmt(value);
    return text;
}

// One effect of a Resolume chain converted: the effect when Nocturne has one, the conversions that may not match
// Resolume exactly, and what could not be imported.
struct Converted {
    std::string label;
    std::optional<Effect> effect;
    bool identity = false;
    std::vector<std::string> approximated;
    std::vector<std::string> unsupported;
};

class Leftovers {
public:
    explicit Leftovers(std::vector<std::string>& out) : out_(out) {}

    // Every parameter of `params` not handled is reported when it is off its default, and every animation is, except
    // those whose animation the caller imports.
    void check(const XmlNode* params, const std::set<std::string>& handled, const std::string& prefix = {},
               const std::set<std::string>& animationHandled = {})
    {
        if (!params)
            return;
        for (const auto& c : params->children) {
            if (!isParam(*c))
                continue;
            const std::string name = c->attr("name");
            if (const std::string a = animation(*c); !a.empty() && !animationHandled.contains(name))
                out_.push_back(prefix + name + " is animated by " + a + "; imported at its saved value");
            if (!handled.contains(name) && !atDefault(*c))
                out_.push_back(prefix + "parameter " + describeParam(*c) + " not imported");
        }
    }

private:
    std::vector<std::string>& out_;
};

struct Size {
    double width = 1920.0;
    double height = 1080.0;
};

Converted transform(const XmlNode* params, Size size)
{
    Converted out{.label = "Transform"};
    Leftovers left(out.unsupported);
    left.check(params, {"Position X", "Position Y", "Scale", "Rotation Z"});
    Effect effect{.type = "transform", .params = defaultParams("transform")};
    const double x = paramValue(params, "Position X", 0.0);
    const double y = paramValue(params, "Position Y", 0.0);
    const double scale = paramValue(params, "Scale", 100.0);
    const double rotation = paramValue(params, "Rotation Z", 0.0);
    out.identity = x == 0.0 && y == 0.0 && scale == 100.0 && rotation == 0.0 && out.unsupported.empty();
    if (out.identity)
        return out;
    effect.params["positionX"] = clampParam("transform", "positionX", x / size.width);
    effect.params["positionY"] = clampParam("transform", "positionY", -y / size.height);
    effect.params["scale"] = clampParam("transform", "scale", scale / 100.0);
    effect.params["rotation"] = clampParam("transform", "rotation", rotation);
    out.effect = effect;
    out.approximated.push_back("Transform position in pixels over the composition size, Scale percent over 100; the "
                               "signs of Position Y and Rotation Z are unverified");
    return out;
}

void colorize(const XmlNode* params, Converted& out)
{
    Leftovers left(out.unsupported);
    left.check(params, {"Color"});
    const XmlNode* color = param(params, "Color");
    const XmlNode* channels = color ? color->child("Params", "Channels") : nullptr;
    left.check(channels, {"Red", "Green", "Blue", "Hue", "Saturation", "Brightness"}, "Color ");
    const double r = paramValue(channels, "Red", 1.0);
    const double g = paramValue(channels, "Green", 0.0);
    const double b = paramValue(channels, "Blue", 0.0);
    const double high = std::max({r, g, b});
    const double range = high - std::min({r, g, b});
    double hue = 0.0;
    if (range > 0.0) {
        if (high == r)
            hue = std::fmod((g - b) / range, 6.0);
        else if (high == g)
            hue = (b - r) / range + 2.0;
        else
            hue = (r - g) / range + 4.0;
        hue /= 6.0;
        if (hue < 0.0)
            hue += 1.0;
    }
    out.effect->params["hue"] = hue;
    out.effect->params["saturation"] = high > 0.0 ? range / high : 0.0;
    if (high < 0.999)
        out.approximated.push_back("Colorize colour value " + fmt(high) +
                                   " not kept: Nocturne's colorize tints at full value (hue and saturation kept)");
}

void edgeDetection(const XmlNode* params, Converted& out)
{
    Leftovers left(out.unsupported);
    left.check(params, {"Multiplier", "Color"});
    const double multiplier = paramValue(params, "Multiplier", 0.2);
    out.effect->params["multiplier"] = clampParam("edgeDetection", "multiplier", multiplier);
    if (const XmlNode* color = param(params, "Color"); color && color->attr("value") != "4294967295")
        out.unsupported.push_back("EdgeDetection colour " + color->attr("value") + " not imported (edges stay white)");
}

void stripper(const XmlNode* params, Converted& out)
{
    Leftovers left(out.unsupported);
    left.check(params, {"Number", "Speed", "GlobalWidth", "Alpha"});
    const double number = paramValue(params, "Number", 0.1);
    const double speed = paramValue(params, "Speed", 0.0);
    const double width = paramValue(params, "GlobalWidth", 1.0);
    const double alpha = paramValue(params, "Alpha", 1.0);
    Effect& e = *out.effect;
    e.params["number"] = clampParam("stripper", "number", 1.0 + 63.0 * number);
    e.params["speed"] = clampParam("stripper", "speed", 4.0 * speed);
    e.params["width"] = clampParam("stripper", "width", width);
    e.params["alpha"] = clampParam("stripper", "alpha", alpha);
    out.approximated.push_back("Stripper's 0..1 sliders mapped linearly: Number " + fmt(number) + " -> " +
                               fmt(e.params["number"]) + " strips, Speed " + fmt(speed) + " -> " +
                               fmt(e.params["speed"]) + " strips/s, GlobalWidth " + fmt(width) + " -> width, Alpha " +
                               fmt(alpha) + " -> fill alpha");
}

void withDefaults(const XmlNode* params, Converted& out, const std::string& name)
{
    Leftovers left(out.unsupported);
    left.check(params, {});
    out.approximated.push_back("Resolume " + name + " defaults taken as Nocturne's " + describeEffect(*out.effect) +
                               " defaults");
}

Converted convert(const XmlNode& pass, Size size)
{
    const std::string type = pass.attr("type");
    if (type == "TransformEffect")
        return transform(pass.child("Params"), size);
    Converted out{.label = pass.attr("name").empty() ? type : pass.attr("name")};
    const XmlNode* inner = type == "DryWetEffect" ? pass.child("RenderPass") : nullptr;
    if (!inner) {
        out.unsupported.push_back("render pass of type " + type + ": no Nocturne equivalent");
        return out;
    }
    const std::string effectType = inner->attr("type");
    out.label = effectType;
    const XmlNode* dryWet = pass.child("Params");
    Leftovers(out.unsupported).check(dryWet, {"Bypassed", "Opacity"});
    const bool bypassed = paramValue(dryWet, "Bypassed", 0.0) != 0.0;
    const double mix = paramValue(dryWet, "Opacity", 1.0);
    if (const XmlNode* mixer = mixerPass(pass.child("ChoosableMixer", "Mixer")); mixer && mixer->attr("type") != "Alpha")
        out.unsupported.push_back("effect mixed with blend " + mixer->attr("type") +
                                  " not imported; Nocturne mixes effects as normal (dry/wet)");

    static const std::map<std::string, std::string> types{{"Colorize", "colorize"},
                                                          {"EdgeDetection", "edgeDetection"},
                                                          {"Stripper", "stripper"},
                                                          {"Trails", "trails"}};
    auto known = types.find(effectType);
    if (known == types.end()) {
        out.unsupported.insert(out.unsupported.begin(), std::string("no Nocturne effect") +
                                                            (bypassed ? " (it was bypassed)" : ""));
        return out;
    }
    out.effect = Effect{.type = known->second, .params = defaultParams(known->second)};
    const XmlNode* params = inner->child("Params");
    if (effectType == "Colorize")
        colorize(params, out);
    else if (effectType == "EdgeDetection")
        edgeDetection(params, out);
    else if (effectType == "Stripper")
        stripper(params, out);
    else
        withDefaults(params, out, effectType);
    out.effect->params["mix"] = bypassed ? 0.0 : std::clamp(mix, 0.0, 1.0);
    if (bypassed)
        out.approximated.push_back(effectType + " was bypassed: imported with mix 0");
    return out;
}

std::string join(const std::vector<std::string>& parts, const std::string& separator)
{
    std::string text;
    for (const std::string& p : parts)
        text += (text.empty() ? "" : separator) + p;
    return text;
}

struct ClipNote {
    ImportNote note;
    size_t clips = 0;
};

class AvcImporter {
public:
    AvcImporter(const XmlNode& avc, const MediaFinder& media, ImportReport& report, const AvcOptions& options)
        : avc_(avc), media_(media), report_(report), options_(options)
    {
    }

    ImportedComposition run()
    {
        if (avc_.name != "Composition")
            throw std::runtime_error("not a Resolume composition (root element " + avc_.name + ")");
        Composition& c = out_.composition;
        c.decks.clear();
        c.quantise = 0;
        if (const XmlNode* info = avc_.child("CompositionInfo")) {
            size_.width = number(info->attr("width")).value_or(size_.width);
            size_.height = number(info->attr("height")).value_or(size_.height);
        }
        if (size_.width != 1920.0 || size_.height != 1080.0)
            approximated("composition", "size " + fmt(size_.width) + "x" + fmt(size_.height) +
                                            " not kept: Nocturne composes at its own size");
        compositionParams();
        tempo();
        compositionChain();
        for (const XmlNode* layer : avc_.all("Layer"))
            layers_.push_back(layer);
        std::sort(layers_.begin(), layers_.end(), [](const XmlNode* a, const XmlNode* b) {
            return number(a->attr("layerIndex")).value_or(0) < number(b->attr("layerIndex")).value_or(0);
        });
        for (const XmlNode* layer : layers_)
            importLayer(*layer);
        for (const XmlNode* deck : avc_.all("Deck"))
            importDeck(*deck);
        for (const ClipNote& n : clipNotes_)
            approximated(n.note.where, n.note.what + " (" + std::to_string(n.clips) + (n.clips == 1 ? " clip)" : " clips)"));
        if (options_.hoistClipChains)
            hoistClipChains();
        selectDeck();
        crossfader();
        other();
        summary();
        return std::move(out_);
    }

private:
    void approximated(const std::string& where, const std::string& what) { report_.approximated.push_back({where, what}); }
    void unsupported(const std::string& where, const std::string& what) { report_.unsupported.push_back({where, what}); }

    void unsupportedAll(const std::string& where, const std::vector<std::string>& what)
    {
        for (const std::string& w : what)
            unsupported(where, w);
    }

    void leftovers(const XmlNode* params, const std::set<std::string>& handled, const std::string& where,
                   const std::string& prefix = {}, const std::set<std::string>& animationHandled = {})
    {
        std::vector<std::string> left;
        Leftovers(left).check(params, handled, prefix, animationHandled);
        unsupportedAll(where, left);
    }

    void compositionParams()
    {
        const XmlNode* params = avc_.child("Params");
        name_ = paramText(params, "Name");
        leftovers(params, {"Name", "Beat Snap", "BeatSnap"}, "composition");
        const XmlNode* snap = param(params, "Beat Snap") ? param(params, "Beat Snap") : param(params, "BeatSnap");
        static const std::map<int, int> snaps{{1, 0}, {2, 32}, {3, 16}, {4, 8}, {5, 4}, {6, 2}, {7, 1}};
        const int value = snap ? int(number(snap->attr("value")).value_or(1)) : 1;
        auto it = snaps.find(value);
        out_.composition.quantise = it == snaps.end() ? 0 : it->second;
        if (it == snaps.end())
            unsupported("composition", "Beat Snap " + std::to_string(value) + " not imported; quantise off");
    }

    void tempo()
    {
        const XmlNode* tempo = avc_.child("TempoController");
        const XmlNode* params = tempo ? tempo->child("Params") : nullptr;
        const XmlNode* bpm = param(params, "Tempo") ? param(params, "Tempo") : param(params, "BPM");
        if (bpm)
            out_.composition.bpm = std::clamp(number(bpm->attr("value")).value_or(120.0), kMinBpm, kMaxBpm);
        leftovers(params, {"Tempo", "BPM"}, "tempo controller");
        tempoSaved_ = bpm != nullptr;
    }

    void compositionChain()
    {
        const XmlNode* track = avc_.child("VideoTrack");
        if (!track)
            return;
        sizeParams(track->child("Params"), "composition", {});
        importChain(chain(track->child("RenderPass", "RenderPassChain")), out_.composition.effects, "composition",
                    "the composition");
    }

    // Adds the converted effects to `effects` and reports what each lost on the way. A clip's conversions are counted
    // per layer and reported once (see clipNotes), where every clip of the layer converts alike.
    void importChain(std::vector<Converted> converted, std::vector<Effect>& effects, const std::string& where,
                     const std::string& identity, const std::string& clipLayer = {})
    {
        for (Converted& e : converted) {
            if (e.identity) {
                ++identityTransforms_[identity];
                continue;
            }
            const std::string at = where + " / effect " + e.label;
            for (const std::string& a : e.approximated)
                clipLayer.empty() ? approximated(at, a) : countClipNote(clipLayer + " / clip effect " + e.label, a);
            unsupportedAll(at, e.unsupported);
            if (e.effect) {
                e.effect->id = ++effectIds_;
                effects.push_back(*e.effect);
            }
        }
    }

    void hoistClipChains()
    {
        auto sameChain = [](const std::vector<Effect>& a, const std::vector<Effect>& b) {
            return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const Effect& x, const Effect& y) {
                return x.type == y.type && x.params == y.params && x.bypass == y.bypass;
            });
        };
        // The clips of `cells` all carry the same chain: it is taken off them and returned.
        auto takeShared = [&](const std::vector<ClipRef*>& cells) {
            std::vector<Effect> chain = cells.front()->effects;
            if (chain.empty() ||
                !std::all_of(cells.begin(), cells.end(), [&](ClipRef* c) { return sameChain(c->effects, chain); }))
                return std::vector<Effect>{};
            for (ClipRef* clip : cells)
                clip->effects.clear();
            return chain;
        };
        for (Layer& layer : out_.composition.layers) {
            std::map<uint64_t, std::vector<ClipRef*>> decks;
            std::vector<ClipRef*> all;
            for (auto& [column, clip] : layer.clips) {
                decks[columnDeck(out_.composition, column)].push_back(&clip);
                all.push_back(&clip);
            }
            if (all.empty())
                continue;
            if (const std::vector<Effect> chain = takeShared(all); !chain.empty()) {
                layer.effects.insert(layer.effects.begin(), chain.begin(), chain.end());
                report_.mapped.push_back("layer " + layer.name + ": clip effects " + describeChain(chain) +
                                         ", alike on every clip, run as the layer's first effects");
                continue;
            }
            for (const auto& [deck, cells] : decks) {
                std::vector<Effect> chain = takeShared(cells);
                if (chain.empty())
                    continue;
                const std::string described = describeChain(chain);
                for (Effect effect : layer.effects) {
                    effect.id = ++effectIds_;
                    chain.push_back(effect);
                }
                layer.decks[deck].effects = std::move(chain);
                report_.mapped.push_back("layer " + layer.name + " in deck " + deckName(deck) + ": clip effects " +
                                         described + ", alike on every clip of the deck, run as the deck's layer chain");
            }
        }
    }

    std::string deckName(uint64_t id) const
    {
        for (const Deck& deck : out_.composition.decks)
            if (deck.id == id)
                return deck.name;
        return std::to_string(id);
    }

    void countClipNote(const std::string& where, const std::string& what)
    {
        auto it = std::find_if(clipNotes_.begin(), clipNotes_.end(),
                               [&](const ClipNote& n) { return n.note == ImportNote{where, what}; });
        if (it == clipNotes_.end())
            clipNotes_.push_back({{where, what}, 1});
        else
            ++it->clips;
    }

    void sizeParams(const XmlNode* params, const std::string& where, const std::set<std::string>& owned)
    {
        std::set<std::string> handled{"Width", "Height"};
        handled.insert(owned.begin(), owned.end());
        leftovers(params, handled, where, {}, owned);
        const double w = paramValue(params, "Width", size_.width);
        const double h = paramValue(params, "Height", size_.height);
        if (w != size_.width || h != size_.height)
            unsupported(where, "size " + fmt(w) + "x" + fmt(h) + " not imported (the composition's size applies)");
    }

    std::vector<Converted> chain(const XmlNode* passes)
    {
        std::vector<Converted> effects;
        if (!passes)
            return effects;
        for (const XmlNode* pass : passes->all("RenderPass"))
            effects.push_back(convert(*pass, size_));
        return effects;
    }

    void importLayer(const XmlNode& node)
    {
        Layer layer{.id = out_.composition.layers.size() + 1};
        const XmlNode* params = node.child("Params");
        layer.name = paramText(params, "Name", "Layer " + std::to_string(layer.id));
        const std::string where = "layer " + layer.name;
        leftovers(params, {"Name"}, where);

        const XmlNode* track = node.child("VideoTrack");
        const XmlNode* trackParams = track ? track->child("Params") : nullptr;
        sizeParams(trackParams, where, {"Opacity"});
        opacity(param(trackParams, "Opacity"), layer, where);
        if (track) {
            importChain(chain(track->child("RenderPass", "RenderPassChain")), layer.effects, where, "layers");
            blend(track, layer, where);
            transition(node, track, layer, where);
        }
        audio(node.child("AudioTrack"), where);
        for (const auto& child : node.children)
            if (!std::set<std::string>{"Params", "ClipTransition", "LayerView", "AudioTrack", "VideoTrack"}.contains(
                    child->name))
                unsupported(where, "element " + child->name + " not imported");
        out_.composition.layers.push_back(std::move(layer));
    }

    void opacity(const XmlNode* p, Layer& layer, const std::string& where)
    {
        layer.opacity = p ? std::clamp(number(p->attr("value")).value_or(1.0), 0.0, 1.0) : 1.0;
        const XmlNode* fft = p ? p->child("PhaseSourceFFT") : nullptr;
        if (!fft) {
            if (p && !animation(*p).empty())
                unsupported(where, "opacity animated by " + animation(*p) + "; imported at its saved value " +
                                       fmt(layer.opacity));
            return;
        }
        const XmlNode* fftParams = fft->child("Params");
        const double range = paramValue(fftParams, "FrequencyRange", 0.5);
        const char* feature = range < 1.0 / 3.0 ? "bass" : range < 2.0 / 3.0 ? "mid" : "high";
        layer.opacity = 0.0;
        Modulator m{.id = out_.composition.modulators.size() + 1,
                    .name = layer.name + " opacity",
                    .source = ModulatorSource::Audio,
                    .feature = feature,
                    .target = {.param = {layer.id, 0, "opacity"}}};
        setEnvelope(m, EnvelopePreset::Meter);
        out_.composition.modulators.push_back(m);
        std::string source = fft->attr("linkId") == "/audioengine/externalfft" ? "the audio input" : fft->attr("linkId");
        approximated(where, "opacity follows " + source + " (FFT, FrequencyRange " + fmt(range) + ") -> audio modulator '" +
                                feature + "' adding 0..1 to opacity 0; the band is the third of the range the value "
                                "falls in");
        const double gain = paramValue(fftParams, "Gain", 0.0);
        const double fall = paramValue(fftParams, "Fallback", 1000.0);
        approximated(where, "FFT Gain " + fmt(gain) + " dB and Fallback " + fmt(fall) +
                                " ms not kept: Nocturne's auto gain and the Meter envelope apply");
        leftovers(fftParams, {"FrequencyRange", "Gain", "Fallback"}, where, "opacity FFT ");
    }

    void blend(const XmlNode* track, Layer& layer, const std::string& where)
    {
        const XmlNode* pass = mixerPass(track->child("ChoosableMixer", "Blend"));
        if (!pass)
            return;
        if (auto mode = blendFrom(pass->attr("type")))
            layer.blend = *mode;
        else
            unsupported(where, "blend mode " + pass->attr("type") + " not imported; normal");
    }

    void transition(const XmlNode& node, const XmlNode* track, Layer& layer, const std::string& where)
    {
        if (const XmlNode* pass = mixerPass(track->child("ChoosableMixer", "Transition"));
            pass && pass->attr("type") != "Alpha")
            unsupported(where, "clip transition " + pass->attr("type") + " not imported; alpha");
        const XmlNode* clipTransition = node.child("ClipTransition");
        if (clipTransition && !clipTransition->children.empty()) {
            std::vector<std::string> left;
            Leftovers(left).check(clipTransition->child("Params"), {}, "clip transition ");
            if (left.empty())
                left.push_back("clip transition settings not imported; transitions cut");
            unsupportedAll(where, left);
        }
    }

    void audio(const XmlNode* track, const std::string& where)
    {
        const XmlNode* chain = track ? track->child("AudioEffectChain") : nullptr;
        if (!chain)
            return;
        for (const auto& effect : chain->children)
            unsupported(where, "audio effect " + effect->attr("name") + " not imported: Nocturne plays no clip audio");
    }

    void importDeck(const XmlNode& node)
    {
        Composition& c = out_.composition;
        Deck deck{.id = c.decks.size() + 1};
        deck.name = paramText(node.child("Params"), "Name", "Deck " + std::to_string(deck.id));
        leftovers(node.child("Params"), {"Name"}, "deck " + deck.name);
        std::map<int, uint64_t> columns;
        for (const XmlNode* column : node.all("Column")) {
            const int index = int(number(column->attr("columnIndex")).value_or(double(columns.size())));
            const XmlNode* params = column->child("Params");
            columns[index] = ++columnIds_;
            deck.columns.push_back({columns[index], paramText(params, "Name", "Column " + std::to_string(index + 1))});
            leftovers(params, {"Name"}, "deck " + deck.name + " / column " + std::to_string(index + 1));
        }
        for (const XmlNode* clip : node.all("Clip"))
            importClip(*clip, deck, columns);
        c.decks.push_back(std::move(deck));
    }

    void importClip(const XmlNode& node, const Deck& deck, std::map<int, uint64_t>& columns)
    {
        const XmlNode* params = node.child("Params");
        if (!params)
            return;
        const size_t layerIndex = size_t(number(node.attr("layerIndex")).value_or(0));
        const int columnIndex = int(number(node.attr("columnIndex")).value_or(0));
        Composition& c = out_.composition;
        if (layerIndex >= c.layers.size() || !columns.contains(columnIndex)) {
            unsupported("deck " + deck.name, "clip at layer " + std::to_string(layerIndex + 1) + ", column " +
                                                 std::to_string(columnIndex + 1) + " has no such layer or column");
            return;
        }
        Layer& layer = c.layers[layerIndex];
        const uint64_t column = columns[columnIndex];
        const fs::path original = clipFile(node);
        ClipRef clip;
        clip.name = paramText(params, "Name", original.stem().string());
        const std::string where =
            "deck " + deck.name + " / column " + std::to_string(columnIndex + 1) + " / layer " + layer.name + " / clip " + clip.name;
        leftovers(params, {"Name", "TransportType", "LoadProgress"}, where);
        transport(node, clip, where);
        const XmlNode* track = node.child("VideoTrack");
        if (track)
            clipTrack(*track, layer, clip, where);
        if (const XmlNode* pass = mixerPass(node.child("ChoosableMixer", "Transition")))
            unsupported(where, "clip transition " + pass->attr("type") + " not imported");

        MediaFile file{.path = original};
        if (original.empty()) {
            unsupported(where, "no media file (only video file clips are imported)");
            return;
        }
        if (std::optional<MediaFile> found = media_ ? media_(original) : std::nullopt) {
            file = *found;
            clip.hash = found->hash;
            if (found->path.filename() != original.filename())
                transcoded_.insert(original.string());
        } else {
            missing_.insert(original.string());
        }
        mediaFiles_.insert(original.string());
        layer.clips[column] = clip;
        out_.media[{layer.id, column}] = file;
        ++clips_;
    }

    static fs::path clipFile(const XmlNode& node)
    {
        if (const XmlNode* file = node.at("PreloadData/VideoFile"))
            return file->attr("value");
        if (const XmlNode* source = node.at("VideoTrack/PrimarySource/VideoSource/VideoFormatReaderSource"))
            return source->attr("fileName");
        return {};
    }

    void transport(const XmlNode& node, ClipRef& clip, const std::string& where)
    {
        Transport& t = clip.transport;
        const int type = int(paramValue(node.child("Params"), "TransportType", 0.0));
        t.sync = type == 1 ? SyncMode::Bpm : SyncMode::Timeline;
        if (type > 1)
            unsupported(where, "transport type " + std::to_string(type) + " (timecode or DJ) not imported; timeline");
        const XmlNode* position = node.at("Transport/Params");
        position = position ? param(position, "Position") : nullptr;
        const XmlNode* timeline = position ? position->child("PhaseSourceTransportTimeline") : nullptr;
        if (!timeline) {
            unsupported(where, "transport without a timeline not imported; loops on the timeline");
            return;
        }
        if (const XmlNode* beats = timeline->child("Beats_d"))
            t.beats = std::clamp(int(number(beats->attr("numManualBeats")).value_or(0)), 0, 1024);
        else
            t.beats = std::clamp(int(number(timeline->attr("defaultBeatsDuration")).value_or(0)), 0, 1024);
        const XmlNode* params = timeline->child("Params");
        const XmlNode* modeParam = param(params, "Play Mode") ? param(params, "Play Mode") : param(params, "PlayMode");
        const int mode = modeParam ? int(number(modeParam->attr("value")).value_or(0)) : 0;
        static const std::map<int, PlayMode> modes{{0, PlayMode::Loop},
                                                   {1, PlayMode::Bounce},
                                                   {2, PlayMode::RandomJump},
                                                   {3, PlayMode::OnceClear},
                                                   {4, PlayMode::OnceHold}};
        t.playMode = modes.contains(mode) ? modes.at(mode) : PlayMode::Loop;
        const int direction = int(paramValue(params, "Direction", 2.0));
        if (direction == 0 && t.playMode == PlayMode::Loop)
            t.playMode = PlayMode::Reverse;
        else if (direction != 2)
            unsupported(where, std::string("direction ") + (direction == 1 ? "paused" : "backward") + " with play mode " +
                                   std::to_string(mode) + " not imported; forward");
        t.jumpDistance = std::clamp(paramValue(params, "Max Distance", t.beats > 0 ? double(t.beats) : 8.0), 0.0, 1024.0);
        t.jumpInterval = jumpInterval(param(params, "Interval"), where);
        if (t.playMode == PlayMode::RandomJump && t.sync == SyncMode::Timeline)
            approximated(where, "Random Jump on a timeline clip jumps on the beat grid, not by seconds");
        leftovers(params, {"Max Distance", "BPM", "Play Mode", "PlayMode", "Direction", "Interval"}, where);
    }

    double jumpInterval(const XmlNode* p, const std::string& where)
    {
        if (!p)
            return 1.0;
        const double v = number(p->attr("value")).value_or(1.0);
        for (double beats = 0.125; beats <= 8.0; beats *= 2.0)
            if (std::abs(v - beats) < 1e-9)
                return beats;
        if (v >= -3.0 && v <= 3.0 && v == std::floor(v)) {
            approximated(where, "Random Jump Interval " + fmt(v) + " read as 2^" + fmt(v) + " beats");
            return std::pow(2.0, v);
        }
        unsupported(where, "Random Jump Interval " + fmt(v) + " not imported; 1 beat");
        return 1.0;
    }

    void clipTrack(const XmlNode& track, const Layer& layer, ClipRef& clip, const std::string& where)
    {
        const XmlNode* params = track.child("Params");
        leftovers(params, {"Width", "Height", "Opacity"}, where);
        if (const XmlNode* opacity = param(params, "Opacity"); opacity && !atDefault(*opacity))
            unsupported(where, "clip opacity " + opacity->attr("value") + " not imported: Nocturne has layer opacity only");
        if (const XmlNode* pass = mixerPass(track.child("ChoosableMixer", "Blend")); pass && pass->attr("type") != "Alpha")
            unsupported(where, "clip blend " + pass->attr("type") + " not imported: Nocturne blends layers only");
        importChain(chain(track.child("RenderPass", "RenderPassChain")), clip.effects, where, "clips",
                    "layer " + layer.name);
        if (const XmlNode* source = track.at("PrimarySource/VideoSource/RenderPass"); source && !source->children.empty())
            unsupported(where, "source effects not imported");
    }

    void selectDeck()
    {
        Composition& c = out_.composition;
        if (c.decks.empty())
            c.decks.push_back({1, "Deck 1", {{++columnIds_, "Column 1"}}});
        const size_t current = size_t(number(avc_.attr("currentDeckIndex")).value_or(0));
        const Deck& deck = c.decks[std::min(current, c.decks.size() - 1)];
        c.deck = deck.id;
        if (options_.startColumn == 0 || options_.startColumn > deck.columns.size())
            return;
        c.column = deck.columns[options_.startColumn - 1].id;
        for (Layer& layer : c.layers)
            layer.playing = Play{c.column, std::nullopt};
        startLine_ = "plays deck " + deck.name + " column " + std::to_string(options_.startColumn) +
                     " on load (Resolume saved nothing playing)";
    }

    void crossfader()
    {
        const XmlNode* fader = avc_.child("CrossFader");
        if (!fader)
            return;
        const XmlNode* track = fader->child("VideoTrack");
        const XmlNode* chain = track ? track->child("RenderPass", "RenderPassChain") : nullptr;
        const XmlNode* pass = track ? mixerPass(track->child("ChoosableMixer", "Blend mode")) : nullptr;
        if ((chain && !chain->children.empty()) || (pass && pass->attr("type") != "Alpha"))
            unsupported("crossfader", "crossfader effects and blend not imported: Nocturne has no crossfader");
        else
            report_.ignored.push_back("crossfader: alpha, no effects, no layer assigned to a side");
    }

    void other()
    {
        static const std::set<std::string> known{"versionInfo", "CompositionInfo", "Params", "CompositionView",
                                                 "AudioTrack", "VideoTrack", "Layer", "CrossFader", "TempoController",
                                                 "Deck", "Notes"};
        for (const auto& child : avc_.children)
            if (!known.contains(child->name))
                unsupported("composition", "element " + child->name + " not imported");
        audio(avc_.child("AudioTrack"), "composition");
        if (const XmlNode* notes = avc_.child("Notes"))
            for (const XmlNode* note : notes->all("Note"))
                if (!note->attr("text").empty())
                    unsupported("composition", "note \"" + note->attr("text") + "\" not imported");
    }

    void summary()
    {
        const Composition& c = out_.composition;
        size_t columns = 0;
        for (const Deck& d : c.decks)
            columns += d.columns.size();
        report_.mapped.push_back("composition " + (name_.empty() ? std::string("(unnamed)") : name_) + ": " +
                                 std::to_string(c.layers.size()) + " layers, " + std::to_string(c.decks.size()) +
                                 " decks, " + std::to_string(columns) + " columns, " + std::to_string(clips_) +
                                 " clips");
        report_.mapped.push_back("BPM " + fmt(c.bpm) + (tempoSaved_ ? "" : " (Resolume's default; none saved)") +
                                 ", quantise " + (c.quantise ? std::to_string(c.quantise) + " beats" : "off (Beat Snap None)"));
        for (const Deck& d : c.decks)
            report_.mapped.push_back("deck " + d.name + ": " + std::to_string(d.columns.size()) + " columns");
        for (const Layer& layer : c.layers) {
            std::vector<std::string> effects;
            for (const Effect& e : layer.effects)
                effects.push_back(describeEffect(e));
            const auto modulated = std::find_if(c.modulators.begin(), c.modulators.end(),
                                                [&](const Modulator& m) { return m.target.param.layer == layer.id; });
            report_.mapped.push_back(
                "layer " + layer.name + ": " + std::to_string(layer.clips.size()) + " clips, blend " +
                blendModeName(layer.blend) + ", opacity " + fmt(layer.opacity) +
                (modulated != c.modulators.end() ? " + audio '" + modulated->feature + "'" : "") + ", effects: " +
                (effects.empty() ? "none" : join(effects, ", ")));
        }
        std::map<std::string, size_t> transports;
        for (const Layer& layer : c.layers)
            for (const auto& [column, clip] : layer.clips) {
                const Transport& t = clip.transport;
                ++transports[std::string(t.sync == SyncMode::Bpm ? "BPM sync" : "timeline") + ", play mode " +
                             nlohmann::json(clip)["transport"]["playMode"].get<std::string>() + ", Random Jump every " +
                             fmt(t.jumpInterval) + " beats by up to " + fmt(t.jumpDistance)];
            }
        for (const auto& [text, count] : transports)
            report_.mapped.push_back("transport on " + std::to_string(count) + " clips: " + text);
        clipChainLines();
        if (!c.effects.empty())
            report_.mapped.push_back("composition effects: " + describeChain(c.effects));
        if (!startLine_.empty())
            report_.mapped.push_back(startLine_);

        std::vector<std::string> identity;
        for (const auto& [where, count] : identityTransforms_)
            identity.push_back(std::to_string(count) + (where == "the composition" ? " on the composition" : " on " + where));
        if (!identity.empty())
            report_.ignored.push_back("identity Transform (every Resolume clip, layer and the composition has one): " +
                                      join(identity, ", "));
        report_.ignored.push_back("saved playhead positions, preload and load progress, views, fold states, "
                                  "uniqueIds and column/deck bookkeeping");
        report_.media.push_back(std::to_string(mediaFiles_.size()) + " media files referenced: " +
                                std::to_string(mediaFiles_.size() - missing_.size() - transcoded_.size()) +
                                " found by file name, " + std::to_string(transcoded_.size()) +
                                " as transcodes (STEM.mp4 or STEM.mov), " + std::to_string(missing_.size()) + " missing");
        for (const std::string& m : missing_)
            unsupported("media", m + " not found in the media folder; cells keep the original path");
    }

    static std::string describeChain(const std::vector<Effect>& effects)
    {
        std::vector<std::string> parts;
        for (const Effect& e : effects)
            parts.push_back(describeEffect(e));
        return join(parts, ", ");
    }

    // Each layer's clip chains, the most common first.
    void clipChainLines()
    {
        for (const Layer& layer : out_.composition.layers) {
            std::map<std::string, size_t> chains;
            for (const auto& [column, clip] : layer.clips)
                if (!clip.effects.empty())
                    ++chains[describeChain(clip.effects)];
            std::vector<std::pair<std::string, size_t>> sorted(chains.begin(), chains.end());
            std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            for (const auto& [chain, count] : sorted)
                report_.mapped.push_back("layer " + layer.name + ": clip effects " + chain + " on " +
                                         std::to_string(count) + " of " + std::to_string(layer.clips.size()) + " clips");
        }
    }

    const XmlNode& avc_;
    const MediaFinder& media_;
    ImportReport& report_;
    AvcOptions options_;
    ImportedComposition out_;
    Size size_;
    std::string name_;
    bool tempoSaved_ = false;
    std::vector<const XmlNode*> layers_;
    std::vector<ClipNote> clipNotes_;
    std::map<std::string, size_t> identityTransforms_;
    std::string startLine_;
    std::set<std::string> mediaFiles_;
    std::set<std::string> missing_;
    std::set<std::string> transcoded_;
    uint64_t effectIds_ = 0;
    uint64_t columnIds_ = 0;
    size_t clips_ = 0;
};

// Row-major 3x3, applied to (x, y, 1).
using Mat3 = std::array<double, 9>;

constexpr Mat3 kIdentity{1, 0, 0, 0, 1, 0, 0, 0, 1};

Mat3 multiply(const Mat3& a, const Mat3& b)
{
    Mat3 m{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 3; ++k)
                m[size_t(r * 3 + c)] += a[size_t(r * 3 + k)] * b[size_t(k * 3 + c)];
    return m;
}

// None when the point lands behind the projection.
std::optional<Point> project(const Mat3& m, const Point& p)
{
    const double w = m[6] * p[0] + m[7] * p[1] + m[8];
    if (w < 1e-9)
        return std::nullopt;
    return Point{(m[0] * p[0] + m[1] * p[1] + m[2]) / w, (m[3] * p[0] + m[4] * p[1] + m[5]) / w};
}

// The unit square onto the parallelogram at `origin` spanned by `x` and `y`.
Mat3 frame(const Point& origin, const Point& x, const Point& y)
{
    return {x[0], y[0], origin[0], x[1], y[1], origin[1], 0, 0, 1};
}

std::optional<Mat3> inverseAffine(const Mat3& m)
{
    const double det = m[0] * m[4] - m[1] * m[3];
    if (std::abs(det) < 1e-9)
        return std::nullopt;
    const double a = m[4] / det, b = -m[1] / det, c = -m[3] / det, d = m[0] / det;
    return Mat3{a, b, -(a * m[2] + b * m[5]), c, d, -(c * m[2] + d * m[5]), 0, 0, 1};
}

// The projective map taking each point of `src` onto the same one of `dst`; none when degenerate.
std::optional<Mat3> homography(const std::vector<Point>& src, const std::vector<Point>& dst)
{
    std::array<std::array<double, 9>, 8> a{};
    for (size_t i = 0; i < 4; ++i) {
        const auto [x, y] = src[i];
        const auto [u, v] = dst[i];
        a[2 * i] = {x, y, 1, 0, 0, 0, -u * x, -u * y, u};
        a[2 * i + 1] = {0, 0, 0, x, y, 1, -v * x, -v * y, v};
    }
    for (size_t col = 0; col < 8; ++col) {
        size_t pivot = col;
        for (size_t r = col + 1; r < 8; ++r)
            if (std::abs(a[r][col]) > std::abs(a[pivot][col]))
                pivot = r;
        if (std::abs(a[pivot][col]) < 1e-9)
            return std::nullopt;
        std::swap(a[col], a[pivot]);
        for (size_t r = 0; r < 8; ++r) {
            if (r == col)
                continue;
            const double f = a[r][col] / a[col][col];
            for (size_t c = col; c < 9; ++c)
                a[r][c] -= f * a[col][c];
        }
    }
    Mat3 h{};
    for (size_t i = 0; i < 8; ++i)
        h[i] = a[i][8] / a[i][i];
    h[8] = 1.0;
    return h;
}

std::vector<Point> points(const XmlNode* node)
{
    std::vector<Point> out;
    if (node)
        for (const XmlNode* v : node->all("v"))
            out.push_back({number(v->attr("x")).value_or(0.0), number(v->attr("y")).value_or(0.0)});
    return out;
}

bool near(const Point& a, const Point& b, double tolerance)
{
    return std::abs(a[0] - b[0]) < tolerance && std::abs(a[1] - b[1]) < tolerance;
}

bool near(const std::vector<Point>& a, const std::vector<Point>& b, double tolerance)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!near(a[i], b[i], tolerance))
            return false;
    return true;
}

std::array<Point, 2> bounds(const std::vector<Point>& points)
{
    Point lo{points[0]}, hi{points[0]};
    for (const Point& p : points)
        for (size_t k = 0; k < 2; ++k) {
            lo[k] = std::min(lo[k], p[k]);
            hi[k] = std::max(hi[k], p[k]);
        }
    return {lo, hi};
}

// TL, TR, BR, BL, as Resolume orders a rect's corners.
std::vector<Point> rectCorners(const Point& lo, const Point& hi)
{
    return {lo, {hi[0], lo[1]}, hi, {lo[0], hi[1]}};
}

// A Bezier control grid in Resolume's output pixels, row-major.
struct Grid {
    int width = 4;
    int height = 4;
    std::vector<Point> points;

    std::array<int, 2> patches() const { return {(width - 1) / 3, (height - 1) / 3}; }
};

// The flat one-patch grid over a quad.
Grid flatGrid(const std::vector<Point>& quad)
{
    Grid g;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            const double u = c / 3.0, v = r / 3.0;
            Point p{};
            for (size_t k = 0; k < 2; ++k)
                p[k] = (1 - u) * (1 - v) * quad[0][k] + u * (1 - v) * quad[1][k] + u * v * quad[2][k] +
                       (1 - u) * v * quad[3][k];
            g.points.push_back(p);
        }
    return g;
}

std::vector<Point> resolved(const Grid& g, bool bezier)
{
    return resolvedWarpGrid({g.patches(), bezier, g.points});
}

// The surface as an affine map of the unit square, when the grid is one.
std::optional<Mat3> affineSurface(const Grid& g, bool bezier)
{
    const std::vector<Point> p = resolved(g, bezier);
    const Point o = p.front();
    const Point x{p[size_t(g.width - 1)][0] - o[0], p[size_t(g.width - 1)][1] - o[1]};
    const Point y{p[size_t((g.height - 1) * g.width)][0] - o[0], p[size_t((g.height - 1) * g.width)][1] - o[1]};
    const Mat3 m = frame(o, x, y);
    for (int r = 0; r < g.height; ++r)
        for (int c = 0; c < g.width; ++c)
            if (!near(*project(m, {double(c) / (g.width - 1), double(r) / (g.height - 1)}),
                      p[size_t(r * g.width + c)], 0.01))
                return std::nullopt;
    return m;
}

// Whether an affine map takes the unit square onto itself: a turn by quarter turns, perhaps mirrored.
bool squareSymmetry(const Mat3& t)
{
    for (const Point& corner : rectCorners({0, 0}, {1, 1})) {
        const Point p = *project(t, corner);
        for (double v : p)
            if (std::abs(v) > 1e-4 && std::abs(v - 1) > 1e-4)
                return false;
    }
    return std::abs(t[0] * t[4] - t[1] * t[3]) > 0.5;
}

// The grid of the same surface over parameters (a, b), when Resolume's parameters are t(a, b), a square symmetry.
Grid reorder(const Grid& g, const Mat3& t)
{
    const bool swap = std::abs(t[1]) > 0.5;
    Grid out{swap ? g.height : g.width, swap ? g.width : g.height, {}};
    for (int j = 0; j < out.height; ++j)
        for (int i = 0; i < out.width; ++i) {
            const Point st = *project(t, {double(i) / (out.width - 1), double(j) / (out.height - 1)});
            const long si = std::lround(st[0] * (g.width - 1)), sj = std::lround(st[1] * (g.height - 1));
            out.points.push_back(g.points[size_t(sj * g.width + si)]);
        }
    return out;
}

// Resolume's Flip bits mirror a slice's picture: 1 horizontally, 2 vertically.
Mat3 flipMatrix(int flip)
{
    const double x = flip & 1 ? -1.0 : 1.0, y = flip & 2 ? -1.0 : 1.0;
    return {x, 0, x < 0 ? 1.0 : 0.0, 0, y, y < 0 ? 1.0 : 0.0, 0, 0, 1};
}

class OutputImporter {
public:
    OutputImporter(const XmlNode& preset, const std::vector<std::string>& displays, ImportReport& report)
        : preset_(preset), displays_(displays), report_(report)
    {
    }

    std::vector<Screen> run()
    {
        const XmlNode* setup = preset_.name == "ScreenSetup" ? &preset_ : preset_.child("ScreenSetup");
        if (!setup)
            throw std::runtime_error("not a Resolume Advanced Output file (no ScreenSetup)");
        if (const XmlNode* size = setup->child("CurrentCompositionTextureSize")) {
            input_[0] = number(size->attr("width")).value_or(input_[0]);
            input_[1] = number(size->attr("height")).value_or(input_[1]);
        }
        for (const auto& child : setup->children)
            if (child->name == "Params")
                check(child.get(), {}, "screen setup", "");
        unknown(*setup, {"Params:ScreenSetupParams", "Params:Params", "versionInfo", "CurrentCompositionTextureSize", "SoftEdging",
                         "screens"},
                "screen setup");
        std::vector<Screen> screens;
        if (const XmlNode* list = setup->child("screens"))
            for (const auto& child : list->children) {
                if (child->name == "Screen")
                    screens.push_back(screen(*child, screens.size()));
                else if (child->name == "DmxScreen")
                    dmxScreen(*child);
                else
                    unsupported("screen setup", "element " + label(*child) + " not imported");
            }
        softEdges(setup->child("SoftEdging"), screens);
        report_.mapped.push_back("test card off on every screen: Advanced Output files do not save one");
        return screens;
    }

private:
    void unsupported(const std::string& where, const std::string& what) { report_.unsupported.push_back({where, what}); }

    void check(const XmlNode* params, const std::set<std::string>& handled, const std::string& where,
               const std::string& prefix)
    {
        std::vector<std::string> left;
        Leftovers(left).check(params, handled, prefix);
        for (const std::string& l : left)
            unsupported(where, l);
    }

    static std::string label(const XmlNode& node)
    {
        const std::string name = paramText(node.child("Params", "Common"), "Name",
                                           paramText(node.child("Params", "Params"), "Name", node.attr("name")));
        return node.name + (name.empty() ? "" : " '" + name + "'");
    }

    // Reports every child element not listed; a Params element is listed as "Params:" and its name.
    void unknown(const XmlNode& node, const std::set<std::string>& known, const std::string& where)
    {
        for (const auto& c : node.children)
            if (!known.contains(c->name == "Params" ? "Params:" + c->attr("name") : c->name))
                unsupported(where, "element " + label(*c) + " not imported");
    }

    static ColorAdjust color(const XmlNode* params)
    {
        auto v = [&](const char* name) { return std::clamp(paramValue(params, name, 0.0), -1.0, 1.0); };
        return {v("Brightness"), v("Contrast"), v("Red"), v("Green"), v("Blue")};
    }

    Screen screen(const XmlNode& node, size_t index)
    {
        Screen s{.id = index + 1};
        const XmlNode* params = node.child("Params", "Params");
        s.name = paramText(params, "Name", node.attr("name"));
        const std::string where = "screen " + s.name;
        check(params, {"Name", "Enabled", "Hidden"}, where, "");
        if (paramValue(params, "Hidden", 0.0) != 0.0)
            report_.ignored.push_back(where + ": Hidden (an editor setting)");
        const XmlNode* output = node.child("Params", "Output");
        check(output, {"Brightness", "Contrast", "Red", "Green", "Blue"}, where, "screen output ");
        s.color = color(output);
        unknown(node, {"Params:Params", "Params:Output", "guides", "layers", "OutputDevice"}, where);

        const XmlNode* devices = node.child("OutputDevice");
        const XmlNode* device = devices && !devices->children.empty() ? devices->children.front().get() : nullptr;
        output_ = {1920.0, 1080.0};
        std::string deviceId, deviceLabel = "no output device";
        if (device) {
            output_[0] = number(device->attr("width")).value_or(output_[0]);
            output_[1] = number(device->attr("height")).value_or(output_[1]);
            deviceId = device->attr("deviceId");
            deviceLabel = device->name + " '" + device->attr("name") + "' " + fmt(output_[0]) + "x" + fmt(output_[1]) +
                          " (" + deviceId + ")";
            for (size_t i = 1; i < devices->children.size(); ++i)
                unsupported(where, "second output device " + label(*devices->children[i]) + " not imported");
        }
        s.display = index < displays_.size() ? displays_[index] : deviceId;
        if (paramValue(params, "Enabled", 1.0) == 0.0) {
            unsupported(where, "disabled in Resolume: imported without a display");
            s.display.clear();
        }
        if (const XmlNode* guides = node.child("guides"))
            for (const XmlNode* guide : guides->all("ScreenGuide"))
                if (const XmlNode* image = param(guide->child("Params"), "Image"); image && !image->children.empty())
                    unsupported(where, "guide image not imported (editor only)");
        if (const XmlNode* layers = node.child("layers"))
            for (const auto& child : layers->children) {
                if (child->name != "Slice") {
                    unsupported(where, label(*child) +
                                           " not imported: only slices are, as no saved file with masks or polygon "
                                           "slices has been seen to read their format from");
                    continue;
                }
                if (std::optional<Slice> slice = this->slice(*child, where))
                    s.slices.push_back(*slice);
            }
        report_.mapped.push_back(where + ": " + std::to_string(s.slices.size()) + " slice(s), output " + deviceLabel +
                                 " -> display " + (s.display.empty() ? "none" : s.display));
        return s;
    }

    void dmxScreen(const XmlNode& node)
    {
        const std::string where = "screen setup / DMX screen " + paramText(node.child("Params"), "Name", node.attr("name"));
        const XmlNode* layers = node.child("layers");
        const size_t slices = layers ? layers->children.size() : 0;
        unsupported(where, "not imported with its " + std::to_string(slices) + " fixture slice(s) (lumiverse " +
                               node.attr("LumiverseId") + "): Nocturne has no DMX output");
        if (layers)
            for (const auto& child : layers->children)
                unsupported(where + " / " + label(*child), "not imported");
    }

    std::optional<Slice> slice(const XmlNode& node, const std::string& screenWhere)
    {
        const XmlNode* common = node.child("Params", "Common");
        Slice s{.name = paramText(common, "Name", "Slice")};
        const std::string where = screenWhere + " / slice " + s.name;
        check(common, {"Name", "Enabled"}, where, "");
        if (paramValue(common, "Enabled", 1.0) == 0.0) {
            unsupported(where, "disabled in Resolume: not imported");
            return std::nullopt;
        }
        s.id = ++sliceIds_;
        unknown(node, {"Params:Common", "Params:Input", "Params:Output", "InputRect", "OutputRect", "Warper"}, where);
        const XmlNode* input = node.child("Params", "Input");
        check(input, {"Input Source", "SoftEdgeEnable"}, where, "");
        if (const XmlNode* source = param(input, "Input Source"); source && source->attr("value") != "0:1")
            unsupported(where, "input source " + source->attr("value") + " not imported; the composition");
        const XmlNode* output = node.child("Params", "Output");
        check(output, {"Flip", "Brightness", "Contrast", "Red", "Green", "Blue"}, where, "slice output ");
        s.color = color(output);
        const int flip = int(paramValue(output, "Flip", 0.0)) & 3;

        const XmlNode* inputRect = node.child("InputRect");
        std::vector<Point> in = points(inputRect);
        if (in.size() != 4 && inputRect)
            unsupported(where, "input rect with " + std::to_string(in.size()) + " points not imported; the composition");
        std::optional<Mat3> fromInput;
        if (in.size() == 4)
            fromInput = inverseAffine(frame(in[0], {in[1][0] - in[0][0], in[1][1] - in[0][1]},
                                            {in[3][0] - in[0][0], in[3][1] - in[0][1]}));
        if (!fromInput) {
            if (in.size() == 4)
                unsupported(where, "empty input rect not imported; the composition");
            in = rectCorners({0, 0}, input_);
            fromInput = inverseAffine(frame({0, 0}, {input_[0], 0}, {0, input_[1]}));
        }
        const auto [lo, hi] = bounds(in);
        s.inputRect = {lo[0] / input_[0], lo[1] / input_[1], (hi[0] - lo[0]) / input_[0], (hi[1] - lo[1]) / input_[1]};
        const double orientation = inputRect ? number(inputRect->attr("orientation")).value_or(0.0) : 0.0;
        const bool turned = std::abs(orientation) > 1e-9;
        const std::string turn = "input rect turned " + fmt(orientation * 180.0 / 3.14159265358979323846) + " degrees";

        Grid grid;
        Mat3 h = kIdentity;
        bool bezier = false;
        warper(node.child("Warper"), points(node.child("OutputRect")), where, grid, h, bezier);

        // Resolume shows input point inputFrame(flip(s, t)) at its surface point (s, t); Nocturne shows input point
        // box(a, b) at (a, b), so (s, t) = toSource(a, b).
        const Mat3 toSource =
            multiply(flipMatrix(flip), multiply(*fromInput, frame(lo, {hi[0] - lo[0], 0}, {0, hi[1] - lo[1]})));
        const std::optional<Mat3> surface = affineSurface(grid, bezier);
        if (squareSymmetry(toSource)) {
            place(s, reorder(grid, toSource), bezier, h, where);
            if (turned)
                report_.approximated.push_back(
                    {where, turn + ": Nocturne input rects cannot turn, so the warp grid is turned instead"});
        } else if (surface && placeTurned(s, multiply(h, multiply(*surface, toSource)), in, bezier)) {
            report_.approximated.push_back(
                {where, turn + ": Nocturne input rects cannot turn, so it is imported as its bounding box with an "
                               "inverted slice mask over the turned rect and the corner pin solved for it"});
        } else {
            unsupported(where, turn + " not imported, as its warp is curved or its corner pin cannot reach the "
                                      "bounding box: the bounding box, unturned");
            place(s, grid, bezier, h, where);
        }
        if (flip)
            report_.approximated.push_back(
                {where, "Flip " + std::to_string(flip) +
                            " read as 1 horizontal, 2 vertical (no saved sample to confirm) and imported by "
                            "mirroring the warp grid"});

        if (paramValue(input, "SoftEdgeEnable", 0.0) != 0.0) {
            if (turned)
                report_.approximated.push_back(
                    {where, "soft edge off: its input rect is turned (Resolume turns it off then too)"});
            else
                softEdged_.push_back({s.id, where});
        }
        report_.mapped.push_back(describe(s, where));
        return s;
    }

    void warper(const XmlNode* node, std::vector<Point> outputRect, const std::string& where, Grid& grid, Mat3& h,
                bool& bezier)
    {
        if (outputRect.size() != 4)
            outputRect = rectCorners({0, 0}, output_);
        grid = flatGrid(outputRect);
        if (!node)
            return;
        unknown(*node, {"Params:Warper", "BezierWarper", "Homography"}, where + " / warper");
        const XmlNode* params = node->child("Params", "Warper");
        check(params, {"Point Mode", "Flip"}, where, "warper ");
        const std::string mode = paramText(params, "Point Mode", "PM_LINEAR");
        bezier = mode == "PM_BEZIER";
        if (!bezier && mode != "PM_LINEAR")
            unsupported(where, "point mode " + mode + " not imported; linear, as Resolume falls back to");
        if (const double f = paramValue(params, "Flip", 0.0); f != 0.0)
            report_.ignored.push_back(where + ": warper Flip " + fmt(f) + " (a mirror already in its points)");

        if (const XmlNode* b = node->child("BezierWarper")) {
            const int w = int(number(b->attr("controlWidth")).value_or(0));
            const int ht = int(number(b->attr("controlHeight")).value_or(0));
            const std::vector<Point> v = points(b->child("vertices"));
            const bool shaped = w >= 4 && ht >= 4 && (w - 1) % 3 == 0 && (ht - 1) % 3 == 0 &&
                                (w - 1) / 3 <= kMaxWarpPatches && (ht - 1) / 3 <= kMaxWarpPatches &&
                                v.size() == size_t(w * ht);
            const Grid candidate{w, ht, v};
            if (shaped) {
                const auto [lo, hi] = bounds(resolved(candidate, bezier));
                if (hi[0] - lo[0] > 1e-6 && hi[1] - lo[1] > 1e-6)
                    grid = candidate;
                else
                    unsupported(where, "collapsed Bezier warper grid not imported; flat over its output rect");
            } else {
                unsupported(where, "Bezier warper grid " + std::to_string(w) + "x" + std::to_string(ht) + " with " +
                                       std::to_string(v.size()) + " points not imported; flat over its output rect");
            }
        }
        const std::vector<Point> src = points(node->at("Homography/src"));
        const std::vector<Point> dst = points(node->at("Homography/dst"));
        if (src.size() != 4 || dst.size() != 4 || near(src, dst, 1e-6))
            return;
        if (const std::optional<Mat3> m = homography(src, dst))
            h = *m;
        else
            unsupported(where, "degenerate homography not imported; no corner pin");
    }

    // The warp grid over the bounding box of its surface in output pixels, then that box through the homography as
    // the corner pin.
    void place(Slice& s, const Grid& grid, bool bezier, const Mat3& h, const std::string& where)
    {
        const std::vector<Point> surface = resolved(grid, bezier);
        const auto [lo, hi] = bounds(surface);
        const std::vector<Point> box = rectCorners(lo, hi);
        for (size_t i = 0; i < 4; ++i) {
            const std::optional<Point> p = project(h, box[i]);
            if (!p) {
                unsupported(where, "homography folding the slice behind the projector not imported; no corner pin");
                return place(s, grid, bezier, kIdentity, where);
            }
            s.corners[2 * i] = (*p)[0] / output_[0];
            s.corners[2 * i + 1] = (*p)[1] / output_[1];
        }
        s.warp = {grid.patches(), bezier, {}};
        for (const Point& p : surface)
            s.warp.points.push_back({(p[0] - lo[0]) / (hi[0] - lo[0]), (p[1] - lo[1]) / (hi[1] - lo[1])});
        if (near(s.warp.points, resolvedWarpGrid({s.warp.patches, bezier, {}}), 1e-9))
            s.warp.points.clear();
    }

    // A flat slice whose input rect is turned: its bounding box as the input, `surface` taking that box's unit square
    // to output pixels, and a mask showing only the turned rect.
    bool placeTurned(Slice& s, const Mat3& surface, const std::vector<Point>& inputFrame, bool bezier)
    {
        std::array<double, 8> corners{};
        const std::vector<Point> unit = rectCorners({0, 0}, {1, 1});
        for (size_t i = 0; i < 4; ++i) {
            const std::optional<Point> p = project(surface, unit[i]);
            if (!p)
                return false;
            corners[2 * i] = (*p)[0] / output_[0];
            corners[2 * i + 1] = (*p)[1] / output_[1];
        }
        s.corners = corners;
        s.warp = {{1, 1}, bezier, {}};
        Mask mask{.id = ++maskIds_, .name = "Turned input", .invert = true};
        for (const Point& p : inputFrame)
            mask.points.push_back({p[0] / input_[0], p[1] / input_[1]});
        s.masks.push_back(mask);
        return true;
    }

    static std::string describe(const Slice& s, const std::string& where)
    {
        std::string text = where + ": input rect " + fmt(s.inputRect[0]) + "," + fmt(s.inputRect[1]) + " " +
                           fmt(s.inputRect[2]) + "x" + fmt(s.inputRect[3]);
        if (!s.warp.points.empty())
            text += ", " + std::to_string(s.warp.patches[0]) + "x" + std::to_string(s.warp.patches[1]) + " " +
                    (s.warp.bezier ? "Bezier" : "linear") + " warp";
        if (s.corners != Slice{}.corners) {
            text += ", corners";
            for (double c : s.corners)
                text += " " + fmt(c);
        }
        if (s.color != ColorAdjust{})
            text += ", colour";
        if (!s.masks.empty())
            text += ", " + std::to_string(s.masks.size()) + " mask(s)";
        return text;
    }

    // Resolume fades soft-edged slices where their input rects overlap, with one curve for the whole setup.
    void softEdges(const XmlNode* node, std::vector<Screen>& screens)
    {
        const XmlNode* params = node ? node->child("Params", "Soft Edge") : nullptr;
        const std::set<std::string> handled{"Power", "Luminance", "Gamma", "Gamma Red", "Gamma Green", "Gamma Blue"};
        if (node)
            unknown(*node, {"Params:Soft Edge"}, "soft edging");
        check(params, handled, "soft edging", "");
        if (softEdged_.empty()) {
            if (params && !params->children.empty())
                report_.ignored.push_back("soft edging settings: no slice has soft edge on");
            return;
        }
        SoftEdge curve;
        auto clamped = [&](const char* name, double value, double min, double max) {
            const double c = std::clamp(value, min, max);
            if (c != value)
                report_.approximated.push_back(
                    {"soft edging", std::string(name) + " " + fmt(value) + " clamped to " + fmt(c)});
            return c;
        };
        curve.power = clamped("power", paramValue(params, "Power", 2.0), 0.1, 10.0);
        curve.luminance = clamped("luminance", paramValue(params, "Luminance", 0.5), 0.0, 1.0);
        const double r = paramValue(params, "Gamma Red", 2.0), g = paramValue(params, "Gamma Green", 2.0),
                     b = paramValue(params, "Gamma Blue", 2.0), master = paramValue(params, "Gamma", 1.0);
        if (r != g || g != b)
            report_.approximated.push_back({"soft edging", "gamma per channel " + fmt(r) + ", " + fmt(g) + ", " + fmt(b) +
                                                               " imported as their mean: Nocturne has one gamma"});
        curve.gamma = clamped("gamma", master * (r + g + b) / 3.0, 0.1, 10.0);

        std::map<uint64_t, std::string> where(softEdged_.begin(), softEdged_.end());
        Composition overlap;
        for (const Screen& screen : screens) {
            Screen copy{.id = screen.id};
            for (const Slice& slice : screen.slices)
                if (where.contains(slice.id)) {
                    copy.slices.push_back(slice);
                    copy.slices.back().softEdge = curve;
                }
            overlap.screens.push_back(copy);
        }
        for (const auto& [id, at] : softEdged_) {
            SetSoftEdges edit{.fromOverlap = id};
            try {
                apply(overlap, edit);
            } catch (const ProtocolError&) {
                report_.ignored.push_back(at + ": soft edge on, but no other soft-edged slice overlaps its input rect");
            }
        }
        for (size_t i = 0; i < screens.size(); ++i)
            for (Slice& slice : screens[i].slices)
                for (const Slice& o : overlap.screens[i].slices)
                    if (o.id == slice.id)
                        slice.softEdge = o.softEdge;
        report_.mapped.push_back("soft edge on " + std::to_string(softEdged_.size()) + " slice(s): power " +
                                 fmt(curve.power) + ", luminance " + fmt(curve.luminance) + ", gamma " +
                                 fmt(curve.gamma));
        report_.approximated.push_back({"soft edging", "fade widths come from where soft-edged slices' input rects "
                                                       "overlap, by Nocturne's rule"});
    }

    const XmlNode& preset_;
    const std::vector<std::string>& displays_;
    ImportReport& report_;
    Point input_{1920.0, 1080.0};
    Point output_{1920.0, 1080.0};
    std::vector<std::pair<uint64_t, std::string>> softEdged_;
    uint64_t sliceIds_ = 0;
    uint64_t maskIds_ = 0;
};


} // namespace

std::string formatReport(const ImportReport& report, const std::string& title)
{
    std::string text = "# " + title + "\n\n## Imported\n\n";
    for (const std::string& line : report.mapped)
        text += "- " + line + "\n";
    auto notes = [&](const char* heading, const char* intro, const std::vector<ImportNote>& list) {
        text += std::string("\n## ") + heading + " (" + std::to_string(list.size()) + ")\n\n" + intro + "\n\n";
        if (list.empty())
            text += "- none\n";
        for (const ImportNote& n : list)
            text += "- " + n.where + ": " + n.what + "\n";
    };
    notes("Approximated", "Imported with a conversion that may not look exactly as in Resolume.", report.approximated);
    notes("Not imported", "Left out of the Nocturne composition. Each entry names where it was.", report.unsupported);
    text += "\n## Ignored\n\nElements that do not change the picture.\n\n";
    for (const std::string& line : report.ignored)
        text += "- " + line + "\n";
    text += "\n## Media\n\n";
    for (const std::string& line : report.media)
        text += "- " + line + "\n";
    return text;
}

MediaFinder mediaIn(const fs::path& dir)
{
    auto hashes = std::make_shared<std::map<fs::path, std::string>>();
    return [dir, hashes](const fs::path& original) -> std::optional<MediaFile> {
        for (const fs::path& candidate : {dir / original.filename(), dir / (original.stem().string() + ".mp4"),
                                          dir / (original.stem().string() + ".mov")}) {
            std::error_code ec;
            if (!fs::is_regular_file(candidate, ec))
                continue;
            auto [it, fresh] = hashes->try_emplace(candidate);
            if (fresh)
                it->second = sha256File(candidate);
            return MediaFile{candidate, uint64_t(fs::file_size(candidate, ec)), it->second};
        }
        return std::nullopt;
    };
}

ImportedComposition importAvc(const XmlNode& avc, const MediaFinder& media, ImportReport& report,
                              const AvcOptions& options)
{
    return AvcImporter(avc, media, report, options).run();
}

std::vector<Screen> importAdvancedOutput(const XmlNode& preset, const std::vector<std::string>& displays,
                                         ImportReport& report)
{
    return OutputImporter(preset, displays, report).run();
}

nlohmann::json portableFile(const ImportedComposition& imported, const fs::path& mediaRoot)
{
    nlohmann::json j = toJson(CompositionFile{imported.composition});
    const fs::path root = mediaRoot.lexically_normal();
    j["mediaRoot"] = root.string();
    forEachCellJson(j["composition"], [&](uint64_t layer, uint64_t column, nlohmann::json& clip) {
        auto it = imported.media.find({layer, column});
        if (it == imported.media.end())
            return;
        const fs::path relative = it->second.path.lexically_normal().lexically_relative(root);
        const bool inside = !root.empty() && !relative.empty() && *relative.begin() != "..";
        clip["path"] = inside ? relative.generic_string() : it->second.path.string();
        clip["size"] = it->second.size;
    });
    return j;
}

} // namespace azl
