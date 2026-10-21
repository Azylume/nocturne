#include "document/palette.hpp"

#include "document/edit_support.hpp"
#include "document/json_fields.hpp"
#include "document/modulation_edits.hpp"

#include <algorithm>
#include <cmath>

namespace azl {

namespace {

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

struct EffectColour {
    const Effect* effect = nullptr;
    const EffectDef* def = nullptr;
    ColourGroup group;
};

std::optional<EffectColour> effectColour(const Composition& composition, const ColourRef& ref)
{
    const Effect* effect = ref.effect ? effectById(composition, ref.effect) : nullptr;
    const EffectDef* def = effect ? findEffect(effect->type) : nullptr;
    if (!def)
        return std::nullopt;
    auto group = colourGroup(*def, ref.group);
    if (!group)
        return std::nullopt;
    return EffectColour{effect, def, *group};
}

const TextClip* textOf(const Composition& composition, const ColourRef& ref)
{
    if (ref.effect || (ref.group != kTextColour && ref.group != kTextOutline))
        return nullptr;
    const Layer* layer = layerById(composition, ref.layer);
    if (!layer)
        return nullptr;
    auto clip = layer->clips.find(ref.column);
    return clip == layer->clips.end() || !clip->second.text ? nullptr : &*clip->second.text;
}

const PaletteColour* slotOf(const Composition& composition, const ColourRef& ref)
{
    if (ref.effect || ref.group != kPaletteSlot)
        return nullptr;
    const ColourPalette* palette = paletteById(composition, ref.palette);
    return palette && ref.slot < palette->colours.size() ? &palette->colours[ref.slot] : nullptr;
}

void checkPalette(const ColourPalette& palette)
{
    if (palette.colours.size() > kMaxPaletteColours)
        throw ProtocolError("a palette holds at most " + std::to_string(kMaxPaletteColours) + " colours");
    for (const PaletteColour& c : palette.colours)
        for (const double v : c.colour)
            if (!(v >= 0.0 && v <= 1.0))
                throw ProtocolError("palette colour channels are 0..1");
}

std::vector<ColourPalette>::iterator findPalette(Composition& composition, uint64_t id)
{
    auto it = std::find_if(composition.palettes.begin(), composition.palettes.end(),
                           [&](const ColourPalette& p) { return p.id == id; });
    if (it == composition.palettes.end())
        throw ProtocolError("no palette " + std::to_string(id));
    return it;
}

// Palette colour ids are unique across the composition, so a link never lands on another palette's colour.
void giveColourIds(const Composition& composition, ColourPalette& palette)
{
    uint64_t fresh = 1;
    for (const ColourPalette& p : composition.palettes)
        for (const PaletteColour& c : p.colours)
            fresh = std::max(fresh, c.id + 1);
    for (const PaletteColour& c : palette.colours)
        fresh = std::max(fresh, c.id + 1);
    for (PaletteColour& c : palette.colours)
        if (!c.id)
            c.id = fresh++;
}

// Every palette colour id with the colour in its slot of the active palette; none where the active palette is
// shorter.
std::map<uint64_t, std::optional<Rgba>> activeColours(const Composition& composition)
{
    std::map<uint64_t, std::optional<Rgba>> colours;
    const ColourPalette* active = activePalette(composition);
    for (const ColourPalette& palette : composition.palettes)
        for (size_t i = 0; i < palette.colours.size(); ++i)
            colours[palette.colours[i].id] =
                active && i < active->colours.size() ? std::optional(rgbaOf(active->colours[i].colour)) : std::nullopt;
    return colours;
}

Effect* effectIn(Composition& composition, uint64_t id)
{
    Effect* found = nullptr;
    forEachChain(composition, [&](std::vector<Effect>& chain, uint64_t, std::optional<uint64_t>) {
        for (Effect& e : chain)
            if (e.id == id)
                found = &e;
    });
    return found;
}

TextClip* textIn(Composition& composition, const ColourRef& ref)
{
    TextClip* found = nullptr;
    forEachLayer(composition.layers, [&](Layer& layer) {
        auto clip = layer.clips.find(ref.column);
        if (layer.id == ref.layer && clip != layer.clips.end() && clip->second.text)
            found = &*clip->second.text;
    });
    return found;
}

// The links map holding the colour's link: its effect's or its text clip's.
std::map<std::string, uint64_t>* linksOf(Composition& composition, const ColourRef& ref)
{
    if (ref.effect) {
        Effect* effect = effectIn(composition, ref.effect);
        return effect ? &effect->links : nullptr;
    }
    TextClip* text = textIn(composition, ref);
    return text ? &text->links : nullptr;
}

// Gives the colour this value in place; an effect colour without alpha keeps none.
void paint(Composition& composition, const ColourRef& ref, const Rgba& colour)
{
    if (auto e = effectColour(composition, ref)) {
        Effect* effect = effectIn(composition, ref.effect);
        for (const auto& [name, value] : groupParams(e->group, toHsva(colour)))
            effect->params[name] = value;
    } else if (TextClip* text = textIn(composition, ref)) {
        (ref.group == kTextColour ? text->color : text->outlineColor) = arrayOf(colour);
    }
}

// After a palette edit: recolours the colours whose slot of the active palette it changed, unlinks those whose
// colour left every palette, and adds what relinks them to `undo`.
Edit followPalette(Composition& composition, const std::map<uint64_t, std::optional<Rgba>>& before, Batch undo)
{
    const std::map<uint64_t, std::optional<Rgba>> after = activeColours(composition);
    for (const auto& [ref, id] : linkedColours(composition)) {
        auto now = after.find(id);
        if (now == after.end()) {
            linksOf(composition, ref)->erase(ref.group);
            undo.edits.push_back(SetPaletteLink{ref, id});
            continue;
        }
        auto was = before.find(id);
        if (now->second && (was == before.end() || was->second != now->second))
            paint(composition, ref, *now->second);
    }
    if (undo.edits.size() == 1)
        return std::move(undo.edits.front());
    return undo;
}

} // namespace

Rgba rgbaOf(const std::array<double, 4>& c)
{
    return {c[0], c[1], c[2], c[3]};
}

std::array<double, 4> arrayOf(const Rgba& c)
{
    return {c.r, c.g, c.b, c.a};
}

const ColourPalette* paletteById(const Composition& composition, uint64_t id)
{
    auto it = std::find_if(composition.palettes.begin(), composition.palettes.end(),
                           [&](const ColourPalette& p) { return p.id == id; });
    return it == composition.palettes.end() ? nullptr : &*it;
}

const ColourPalette* activePalette(const Composition& composition)
{
    if (const ColourPalette* selected = paletteById(composition, composition.palette))
        return selected;
    return composition.palettes.empty() ? nullptr : &composition.palettes.front();
}

size_t activePaletteIndex(const Composition& composition)
{
    const ColourPalette* active = activePalette(composition);
    return active ? size_t(active - composition.palettes.data()) : 0;
}

SetPalette addToPalette(const Composition& composition, PaletteColour colour)
{
    const ColourPalette* active = activePalette(composition);
    ColourPalette palette = active ? *active : ColourPalette{0, "Palette 1", {}};
    palette.colours.push_back(std::move(colour));
    return {palette};
}

std::string nextColourName(const ColourPalette* palette)
{
    return "Colour " + std::to_string((palette ? palette->colours.size() : 0) + 1);
}

std::optional<Hsva> colourOf(const Composition& composition, const ColourRef& ref)
{
    if (auto e = effectColour(composition, ref))
        return groupColour(*e->def, e->group, e->effect->params);
    if (const TextClip* text = textOf(composition, ref))
        return toHsva(rgbaOf(ref.group == kTextColour ? text->color : text->outlineColor));
    if (const PaletteColour* slot = slotOf(composition, ref))
        return toHsva(rgbaOf(slot->colour));
    return std::nullopt;
}

bool hasAlpha(const Composition& composition, const ColourRef& ref)
{
    if (auto e = effectColour(composition, ref))
        return !e->group.alpha.empty();
    return textOf(composition, ref) || slotOf(composition, ref);
}

std::string colourLabel(const Composition& composition, const ColourRef& ref)
{
    if (auto e = effectColour(composition, ref))
        return e->group.label;
    if (const PaletteColour* slot = slotOf(composition, ref))
        return slot->name;
    return ref.group == kTextColour ? "Text" : "Outline";
}

std::optional<Edit> colourEdit(const Composition& composition, const ColourRef& ref, const Hsva& colour)
{
    if (auto e = effectColour(composition, ref))
        return SetEffectParams{ref.effect, groupParams(e->group, colour)};
    if (textOf(composition, ref)) {
        SetClipText edit{ref.layer, ref.column};
        (ref.group == kTextColour ? edit.color : edit.outlineColor) = arrayOf(toRgba(colour));
        return edit;
    }
    if (!slotOf(composition, ref))
        return std::nullopt;
    ColourPalette palette = *paletteById(composition, ref.palette);
    palette.colours[ref.slot].colour = arrayOf(toRgba(colour));
    return SetPalette{palette};
}

std::vector<ParamRef> colourParams(const Composition& composition, const ColourRef& ref)
{
    std::vector<ParamRef> params;
    if (auto e = effectColour(composition, ref)) {
        for (const std::string& name : groupParamNames(e->group))
            params.push_back({0, ref.effect, name});
    } else if (textOf(composition, ref) && ref.group == kTextColour) {
        for (const char* name : {"textRed", "textGreen", "textBlue", "textAlpha"})
            params.push_back({ref.layer, 0, name});
    }
    return params;
}

std::vector<ColourRef> effectColours(const Composition& composition, uint64_t effect)
{
    const Effect* e = effectById(composition, effect);
    const EffectDef* def = e ? findEffect(e->type) : nullptr;
    std::vector<ColourRef> refs;
    if (def)
        for (const ColourGroup& group : colourGroups(*def))
            refs.push_back({effect, group.key});
    return refs;
}

std::optional<uint64_t> linkOf(const Composition& composition, const ColourRef& ref)
{
    const std::map<std::string, uint64_t>* links = nullptr;
    if (auto e = effectColour(composition, ref))
        links = &e->effect->links;
    else if (const TextClip* text = textOf(composition, ref))
        links = &text->links;
    auto it = links ? links->find(ref.group) : std::map<std::string, uint64_t>::const_iterator();
    if (!links || it == links->end())
        return std::nullopt;
    return it->second;
}

std::vector<std::pair<ColourRef, uint64_t>> linkedColours(const Composition& composition)
{
    std::vector<std::pair<ColourRef, uint64_t>> linked;
    forEachChain(composition, [&](const std::vector<Effect>& chain, uint64_t, std::optional<uint64_t>) {
        for (const Effect& effect : chain)
            for (const auto& [group, id] : effect.links)
                linked.push_back({ColourRef{effect.id, group}, id});
    });
    forEachLayer(composition.layers, [&](const Layer& layer) {
        for (const auto& [column, clip] : layer.clips)
            if (clip.text)
                for (const auto& [group, id] : clip.text->links)
                    linked.push_back({ColourRef{0, group, layer.id, column}, id});
    });
    return linked;
}

const PaletteColour* paletteColour(const Composition& composition, uint64_t id, size_t* slot)
{
    const ColourPalette* active = activePalette(composition);
    const auto at = linkSlot(composition, id);
    if (!active || !at || *at >= active->colours.size())
        return nullptr;
    if (slot)
        *slot = *at;
    return &active->colours[*at];
}

std::optional<size_t> linkSlot(const Composition& composition, uint64_t id)
{
    for (const ColourPalette& palette : composition.palettes)
        for (size_t i = 0; i < palette.colours.size(); ++i)
            if (palette.colours[i].id == id)
                return i;
    return std::nullopt;
}

Edit apply(Composition& composition, SetPalette& edit)
{
    checkPalette(edit.palette);
    giveColourIds(composition, edit.palette);
    const auto before = activeColours(composition);
    if (edit.palette.id) {
        auto it = std::find_if(composition.palettes.begin(), composition.palettes.end(),
                               [&](const ColourPalette& p) { return p.id == edit.palette.id; });
        if (it != composition.palettes.end()) {
            SetPalette inverse{*it};
            *it = edit.palette;
            return followPalette(composition, before, Batch{{inverse}});
        }
    }
    const size_t index = edit.index.value_or(composition.palettes.size());
    if (index > composition.palettes.size())
        throw ProtocolError("palette index out of range");
    if (composition.palettes.size() >= kMaxPalettes)
        throw ProtocolError("a composition holds at most " + std::to_string(kMaxPalettes) + " palettes");
    if (!edit.palette.id) {
        uint64_t fresh = 1;
        for (const ColourPalette& p : composition.palettes)
            fresh = std::max(fresh, p.id + 1);
        edit.palette.id = fresh;
    }
    edit.index = index;
    composition.palettes.insert(composition.palettes.begin() + ptrdiff_t(index), edit.palette);
    return followPalette(composition, before, Batch{{RemovePalette{edit.palette.id}}});
}

// Colours linked to the removed palette's colours follow the same slot of the palette active after it.
Edit apply(Composition& composition, RemovePalette& edit)
{
    const auto before = activeColours(composition);
    auto it = findPalette(composition, edit.palette);
    Batch undo{{SetPalette{*it, size_t(it - composition.palettes.begin())}}};
    const ColourPalette removed = *it;
    composition.palettes.erase(it);
    const ColourPalette* active = activePalette(composition);
    for (const auto& [ref, id] : linkedColours(composition))
        for (size_t i = 0; active && i < removed.colours.size() && i < active->colours.size(); ++i)
            if (removed.colours[i].id == id) {
                (*linksOf(composition, ref))[ref.group] = active->colours[i].id;
                undo.edits.push_back(SetPaletteLink{ref, id});
            }
    return followPalette(composition, before, std::move(undo));
}

Edit apply(Composition& composition, SelectPalette& edit)
{
    auto it = findPalette(composition, edit.palette);
    const auto before = activeColours(composition);
    edit.beat = edits::quantised(edit.beat, composition.quantise);
    composition.palette = edit.palette;
    composition.palettePlaying = StepPlay{size_t(it - composition.palettes.begin()), edit.beat};
    followPalette(composition, before, Batch{});
    return edit;
}

Edit apply(Composition& composition, SetPaletteAutopilot& edit)
{
    checkAutopilotBars(edit.bars, edit.hold);
    if (edit.transition)
        edits::checkTransitionBeats(*edit.transition);
    Autopilot& autopilot = composition.palettePilot;
    const SetPaletteAutopilot inverse{autopilot.mode, autopilot.bars, autopilot.hold, composition.paletteTransition};
    autopilot.mode = edit.mode.value_or(autopilot.mode);
    autopilot.bars = edit.bars.value_or(autopilot.bars);
    autopilot.hold = edit.hold.value_or(autopilot.hold);
    composition.paletteTransition = edit.transition.value_or(composition.paletteTransition);
    return inverse;
}

Edit apply(Composition& composition, SetDeckPalette& edit)
{
    auto deck = std::find_if(composition.decks.begin(), composition.decks.end(),
                             [&](const Deck& d) { return d.id == edit.deck; });
    if (deck == composition.decks.end())
        throw ProtocolError("no deck " + std::to_string(edit.deck));
    if (edit.palette)
        findPalette(composition, edit.palette);
    SetDeckPalette inverse{edit.deck, deck->palette};
    deck->palette = edit.palette;
    return inverse;
}

void selectDeckPalette(Composition& composition, uint64_t deck, std::optional<double> beat)
{
    auto it = std::find_if(composition.decks.begin(), composition.decks.end(),
                           [&](const Deck& d) { return d.id == deck; });
    if (it == composition.decks.end() || !paletteById(composition, it->palette))
        return;
    SelectPalette select{it->palette, beat.value_or(0.0)};
    apply(composition, select);
    if (!beat)
        composition.palettePlaying->beat.reset();
}

Edit apply(Composition& composition, SetPaletteLink& edit)
{
    const auto colour = colourOf(composition, edit.colour);
    if (!colour || edit.colour.group == kPaletteSlot)
        throw ProtocolError("no colour to link there");
    const PaletteColour* target = edit.link ? paletteColour(composition, *edit.link) : nullptr;
    if (edit.link && !target)
        throw ProtocolError("the active palette has no colour " + std::to_string(*edit.link));
    const Edit restore = *colourEdit(composition, edit.colour, *colour);
    SetPaletteLink inverse{edit.colour, linkOf(composition, edit.colour)};
    std::map<std::string, uint64_t>& links = *linksOf(composition, edit.colour);
    if (!edit.link) {
        links.erase(edit.colour.group);
        return inverse;
    }
    links[edit.colour.group] = *edit.link;
    paint(composition, edit.colour, rgbaOf(target->colour));
    return Batch{{inverse, restore}};
}

nlohmann::json toJson(const SetPalette& e)
{
    nlohmann::json j{{"op", "setPalette"}, {"palette", e.palette}};
    if (e.index)
        j["index"] = *e.index;
    return j;
}

nlohmann::json toJson(const RemovePalette& e)
{
    return {{"op", "removePalette"}, {"palette", e.palette}};
}

nlohmann::json toJson(const SetPaletteLink& e)
{
    return {{"op", "setPaletteLink"}, {"colour", e.colour}, {"link", e.link ? nlohmann::json(*e.link) : nullptr}};
}

nlohmann::json toJson(const SelectPalette& e)
{
    return {{"op", "selectPalette"}, {"palette", e.palette}, {"beat", e.beat}};
}

nlohmann::json toJson(const SetPaletteAutopilot& e)
{
    nlohmann::json j{{"op", "setPaletteAutopilot"}};
    if (e.mode)
        j["mode"] = json_fields::nameOf(json_fields::kAutopilotNames, *e.mode);
    if (e.bars)
        j["bars"] = *e.bars;
    if (e.hold)
        j["hold"] = *e.hold;
    if (e.transition)
        j["transition"] = *e.transition;
    return j;
}

nlohmann::json toJson(const SetDeckPalette& e)
{
    return {{"op", "setDeckPalette"}, {"deck", e.deck}, {"palette", e.palette}};
}

std::optional<Edit> paletteEditFromJson(const std::string& op, const nlohmann::json& j)
{
    if (op == "setPalette") {
        SetPalette edit{field<ColourPalette>(j, "palette")};
        if (j.contains("index") && !j["index"].is_null())
            edit.index = field<size_t>(j, "index");
        return edit;
    }
    if (op == "removePalette")
        return RemovePalette{field<uint64_t>(j, "palette")};
    if (op == "selectPalette")
        return SelectPalette{field<uint64_t>(j, "palette"), j.value("beat", 0.0)};
    if (op == "setPaletteAutopilot")
        return SetPaletteAutopilot{json_fields::autopilotModeField(j), json_fields::optionalField<int>(j, "bars"),
                                   json_fields::optionalField<int>(j, "hold"),
                                   json_fields::optionalField<double>(j, "transition")};
    if (op == "setDeckPalette")
        return SetDeckPalette{field<uint64_t>(j, "deck"), field<uint64_t>(j, "palette")};
    if (op == "setPaletteLink") {
        SetPaletteLink edit{field<ColourRef>(j, "colour"), std::nullopt};
        if (j.contains("link") && !j["link"].is_null())
            edit.link = field<uint64_t>(j, "link");
        return edit;
    }
    return std::nullopt;
}

void to_json(nlohmann::json& j, const ColourPalette& palette)
{
    nlohmann::json colours = nlohmann::json::array();
    for (const PaletteColour& c : palette.colours)
        colours.push_back({{"id", c.id}, {"name", c.name}, {"colour", c.colour}});
    j = {{"id", palette.id}, {"name", palette.name}, {"colours", colours}};
}

void from_json(const nlohmann::json& j, ColourPalette& palette)
{
    palette.id = j.value("id", uint64_t(0));
    palette.name = j.value("name", std::string());
    palette.colours.clear();
    for (const nlohmann::json& c : j.value("colours", nlohmann::json::array()))
        palette.colours.push_back({c.value("name", std::string()),
                                   c.value("colour", std::array<double, 4>{1.0, 1.0, 1.0, 1.0}),
                                   c.value("id", uint64_t(0))});
}

void to_json(nlohmann::json& j, const ColourRef& ref)
{
    j = {{"effect", ref.effect}, {"group", ref.group}};
    if (ref.layer || ref.column)
        j.update({{"layer", ref.layer}, {"column", ref.column}});
    if (ref.group == kPaletteSlot)
        j.update({{"palette", ref.palette}, {"slot", ref.slot}});
}

void from_json(const nlohmann::json& j, ColourRef& ref)
{
    ref.effect = j.value("effect", uint64_t(0));
    ref.group = j.at("group").get<std::string>();
    ref.layer = j.value("layer", uint64_t(0));
    ref.column = j.value("column", uint64_t(0));
    ref.palette = j.value("palette", uint64_t(0));
    ref.slot = j.value("slot", size_t(0));
}

} // namespace azl
