#include "document/edit_support.hpp"

#include "document/cue_edits.hpp"
#include "document/mapping_edits.hpp"
#include "document/modulation_edits.hpp"
#include "document/palette.hpp"
#include "document/text_edits.hpp"

#include <algorithm>
#include <cmath>

// Edits of layers and groups: opacity, blend, bypass, solo, deck settings, adding, removing and moving.

namespace azl {

using namespace edits;

namespace {

void checkPlacement(const Layer& layer, bool inGroup)
{
    if (layer.group && inGroup)
        throw ProtocolError("groups do not nest");
    if (!layer.group && !layer.layers.empty())
        throw ProtocolError("only a group holds layers");
}

// Checks a layer about to join the composition and completes its ids, names and effect params.
void completeLayer(const Composition& composition, Layer& layer, bool inGroup, IdPool& layers, IdPool& effects)
{
    checkPlacement(layer, inGroup);
    for (const auto& [column, clip] : layer.clips) {
        requireColumn(composition, column);
        checkClip(layer, clip);
    }
    if (!(layer.opacity >= 0.0 && layer.opacity <= 1.0))
        throw ProtocolError("opacity must be within 0..1");
    layer.id = layers.take(layer.id, "layer");
    if (layer.name.empty())
        layer.name = (layer.group ? "Group " : "Layer ") + std::to_string(layer.id);
    completeEffects(layer.effects, effects);
    for (auto& [deck, settings] : layer.decks) {
        requireDeck(composition, deck);
        completeDeckLayer(layer, settings, effects);
    }
    for (auto& [column, clip] : layer.clips)
        completeEffects(clip.effects, effects);
    for (Layer& child : layer.layers)
        completeLayer(composition, child, true, layers, effects);
}

} // namespace

Edit apply(Composition& composition, SetLayerOpacity& edit)
{
    if (!(edit.opacity >= 0.0 && edit.opacity <= 1.0))
        throw ProtocolError("opacity must be within 0..1");
    if (edit.layer == 0) {
        const SetLayerOpacity inverse{0, composition.opacity};
        composition.opacity = edit.opacity;
        return inverse;
    }
    Layer& layer = findLayer(composition, edit.layer);
    const SetLayerOpacity inverse{layer.id, layer.opacity};
    layer.opacity = edit.opacity;
    return inverse;
}

Edit apply(Composition& composition, SetLayerBlend& edit)
{
    Layer& layer = findLayer(composition, edit.layer);
    const SetLayerBlend inverse{layer.id, layer.blend};
    layer.blend = edit.blend;
    return inverse;
}

Edit apply(Composition& composition, SetLayerBypass& edit)
{
    Layer& layer = findLayer(composition, edit.layer);
    const SetLayerBypass inverse{layer.id, layer.bypass};
    layer.bypass = edit.bypass;
    return inverse;
}

Edit apply(Composition& composition, SetLayerSolo& edit)
{
    Layer& layer = findLayer(composition, edit.layer);
    const SetLayerSolo inverse{layer.id, layer.solo};
    layer.solo = edit.solo;
    return inverse;
}

Edit apply(Composition& composition, SetLayerFullResolution& edit)
{
    Layer& layer = findLayer(composition, edit.layer);
    const SetLayerFullResolution inverse{layer.id, layer.fullResolution};
    layer.fullResolution = edit.fullResolution;
    return inverse;
}

Edit apply(Composition& composition, SetDeckLayer& edit)
{
    requireDeck(composition, edit.deck);
    Layer& layer = findLayer(composition, edit.layer);
    auto own = layer.decks.find(edit.deck);
    const DeckLayer old = own == layer.decks.end() ? DeckLayer{} : own->second;
    IdPool ids = effectIds(composition, old.effects ? &*own->second.effects : nullptr);
    completeDeckLayer(layer, edit.settings, ids);
    if (edit.settings == DeckLayer{})
        layer.decks.erase(edit.deck);
    else
        layer.decks[edit.deck] = edit.settings;
    return SetDeckLayer{layer.id, edit.deck, old};
}

Edit apply(Composition& composition, AddLayer& edit)
{
    std::vector<Layer>& siblings = children(composition, edit.parent);
    const size_t index = edit.index.value_or(siblings.size());
    if (index > siblings.size())
        throw ProtocolError("layer index out of range");

    Layer layer = edit.layer;
    IdPool layers = layerIds(composition);
    IdPool effects = effectIds(composition);
    completeLayer(composition, layer, edit.parent != 0, layers, effects);
    siblings.insert(siblings.begin() + std::ptrdiff_t(index), layer);
    edit.index = index;
    edit.layer = std::move(layer);
    return RemoveLayer{edit.layer.id};
}

Edit apply(Composition& composition, RemoveLayer& edit)
{
    const Place p = place(composition, edit.layer);
    AddLayer inverse{p.parent, p.index, std::move((*p.siblings)[p.index])};
    p.siblings->erase(p.siblings->begin() + std::ptrdiff_t(p.index));
    return inverse;
}

Edit apply(Composition& composition, MoveLayer& edit)
{
    const Place from = place(composition, edit.layer);
    const std::vector<Layer>& to = children(composition, edit.parent);
    checkPlacement((*from.siblings)[from.index], edit.parent != 0);
    const size_t room = to.size() - (&to == from.siblings ? 1 : 0);
    if (edit.index > room)
        throw ProtocolError("layer index out of range");

    const MoveLayer inverse{edit.layer, from.parent, from.index};
    Layer layer = std::move((*from.siblings)[from.index]);
    from.siblings->erase(from.siblings->begin() + std::ptrdiff_t(from.index));
    // Erasing may have moved the target group, so it is looked up again.
    std::vector<Layer>& target = children(composition, edit.parent);
    target.insert(target.begin() + std::ptrdiff_t(edit.index), std::move(layer));
    return inverse;
}

Edit apply(Composition& composition, SetGroupOutputOnly& edit)
{
    Layer& layer = findLayer(composition, edit.layer);
    if (!layer.group)
        throw ProtocolError("layer " + std::to_string(edit.layer) + " is not a group");
    const SetGroupOutputOnly inverse{layer.id, layer.outputOnly};
    layer.outputOnly = edit.outputOnly;
    return inverse;
}

} // namespace azl
