#include "document/composition.hpp"

#include "document/json_fields.hpp"
#include "effects/effect_registry.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <mutex>

namespace azl {

namespace {

constexpr std::array<const char*, kBlendModeCount> kBlendModeNames = {
    "normal",  "add",     "screen",  "multiply",  "difference", "subtract", "exclusion",
    "lighten", "darken",  "overlay", "hardLight", "softLight",  "dodge",    "burn"};

} // namespace

bool isGeneratorClip(const ClipRef& clip)
{
    return clip.hash.empty() && !clip.text && !clip.effects.empty() && isGenerator(clip.effects.front().type);
}

const char* blendModeName(BlendMode mode)
{
    return kBlendModeNames[size_t(mode)];
}

BlendMode blendModeFromName(const std::string& name)
{
    auto it = std::find(kBlendModeNames.begin(), kBlendModeNames.end(), name);
    if (it == kBlendModeNames.end())
        throw ProtocolError("unknown blend mode " + name);
    return BlendMode(it - kBlendModeNames.begin());
}

Composition defaultComposition()
{
    return {.layers = {Layer{.id = 1, .name = "Layer 1"}}};
}

std::vector<const Effect*> effectsOn(const Composition& composition, uint64_t layer)
{
    std::vector<const Effect*> effects;
    auto add = [&](const std::vector<Effect>& chain) {
        for (const Effect& effect : chain)
            effects.push_back(&effect);
    };
    if (layer == 0)
        add(composition.effects);
    forEachLayer(composition.layers, [&](const Layer& l) {
        if (l.id != layer)
            return;
        add(l.effects);
        for (const auto& [deck, settings] : l.decks)
            if (settings.effects)
                add(*settings.effects);
        for (const auto& [column, clip] : l.clips)
            add(clip.effects);
    });
    return effects;
}

const Effect* effectById(const Composition& composition, uint64_t effect, uint64_t* layer)
{
    const Effect* found = nullptr;
    forEachChain(composition, [&](const std::vector<Effect>& effects, uint64_t holder, std::optional<uint64_t>) {
        for (const Effect& e : effects)
            if (!found && e.id == effect) {
                found = &e;
                if (layer)
                    *layer = holder;
            }
    });
    return found;
}

uint64_t columnDeck(const Composition& composition, uint64_t column)
{
    for (const Deck& deck : composition.decks)
        for (const Column& c : deck.columns)
            if (c.id == column)
                return deck.id;
    return 0;
}

namespace {

const DeckLayer* deckLayer(const Layer& layer, uint64_t deck)
{
    auto it = layer.decks.find(deck);
    return it == layer.decks.end() ? nullptr : &it->second;
}

} // namespace

const std::vector<Effect>& deckEffects(const Layer& layer, uint64_t deck)
{
    const DeckLayer* own = deckLayer(layer, deck);
    return own && own->effects ? *own->effects : layer.effects;
}

BlendMode deckBlend(const Layer& layer, uint64_t deck)
{
    const DeckLayer* own = deckLayer(layer, deck);
    return own ? own->blend.value_or(layer.blend) : layer.blend;
}

namespace {

struct UserTransitions {
    std::mutex mutex;
    std::vector<std::string> types;
};

UserTransitions& userTransitions()
{
    static UserTransitions table;
    return table;
}

bool userTransitionType(const std::string& name)
{
    return name.size() > 4 && name.compare(0, 4, "user") == 0 &&
           !std::islower(static_cast<unsigned char>(name[4])) &&
           std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c); });
}

} // namespace

TransitionMode userTransitionMode(const std::string& type)
{
    if (!userTransitionType(type))
        throw ProtocolError("unknown transition " + type);
    UserTransitions& table = userTransitions();
    std::lock_guard lock(table.mutex);
    auto it = std::find(table.types.begin(), table.types.end(), type);
    if (it == table.types.end())
        it = table.types.insert(table.types.end(), type);
    return TransitionMode(kFirstUserTransition + uint32_t(it - table.types.begin()));
}

std::string transitionName(TransitionMode mode)
{
    if (uint32_t(mode) < kFirstUserTransition)
        return json_fields::nameOf(json_fields::kTransitionNames, mode);
    UserTransitions& table = userTransitions();
    std::lock_guard lock(table.mutex);
    const size_t index = uint32_t(mode) - kFirstUserTransition;
    return index < table.types.size() ? table.types[index] : std::string();
}

TransitionMode transitionFromName(const std::string& name)
{
    for (const auto& [mode, n] : json_fields::kTransitionNames)
        if (name == n)
            return mode;
    return userTransitionMode(name);
}

Transition deckTransition(const Layer& layer, uint64_t deck)
{
    const DeckLayer* own = deckLayer(layer, deck);
    if (!own)
        return layer.transition;
    return {own->transitionMode.value_or(layer.transition.mode), own->transitionBeats.value_or(layer.transition.beats)};
}

Autopilot deckAutopilot(const Layer& layer, uint64_t deck)
{
    const DeckLayer* own = deckLayer(layer, deck);
    if (!own)
        return layer.autopilot;
    return {own->autopilotMode.value_or(layer.autopilot.mode), own->autopilotBars.value_or(layer.autopilot.bars),
            layer.autopilot.hold};
}

const ClipRef* playingClip(const Layer& layer)
{
    if (!layer.playing)
        return nullptr;
    auto it = layer.clips.find(layer.playing->column);
    return it == layer.clips.end() ? nullptr : &it->second;
}

bool isLive(const Edit& edit)
{
    return std::holds_alternative<TriggerClip>(edit) || std::holds_alternative<TriggerColumn>(edit) ||
           std::holds_alternative<SelectDeck>(edit) || std::holds_alternative<ArmClip>(edit) ||
           std::holds_alternative<SetTimeline>(edit) || std::holds_alternative<CancelTrigger>(edit) ||
           std::holds_alternative<StopLayer>(edit) || std::holds_alternative<TriggerStep>(edit) ||
           std::holds_alternative<SelectPalette>(edit);
}

} // namespace azl
