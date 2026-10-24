#include "document/composition.hpp"

#include "document/cue_edits.hpp"
#include "document/edit_support.hpp"
#include "document/mapping_edits.hpp"
#include "document/modulation_edits.hpp"
#include "document/palette.hpp"
#include "document/text_edits.hpp"

#include "effects/effect_registry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <set>
#include <type_traits>
#include <utility>

// applyEdit: it dispatches each edit to its apply (edit_support.hpp) and takes the macro targets, modulators and
// Cue steps of what an edit removes with it; and the two edits that hold others or the whole document.

namespace azl {

namespace {

struct Owners {
    std::set<uint64_t> layers;
    std::set<uint64_t> effects;
};

Owners ownersIn(const Composition& composition)
{
    Owners owners;
    forEachLayer(composition.layers, [&](const Layer& layer) { owners.layers.insert(layer.id); });
    forEachChain(composition, [&](const std::vector<Effect>& effects, uint64_t, std::optional<uint64_t>) {
        for (const Effect& effect : effects)
            owners.effects.insert(effect.id);
    });
    return owners;
}

std::set<uint64_t> gone(const std::set<uint64_t>& before, const std::set<uint64_t>& after)
{
    std::set<uint64_t> result;
    std::set_difference(before.begin(), before.end(), after.begin(), after.end(),
                        std::inserter(result, result.end()));
    return result;
}

// The edits that can take a layer or an effect out of the document, directly or with its clip, column or deck.
bool mayRemoveOwners(const Edit& edit)
{
    return std::holds_alternative<RemoveLayer>(edit) || std::holds_alternative<RemoveEffect>(edit) ||
           std::holds_alternative<SetSlot>(edit) || std::holds_alternative<SetLayerClip>(edit) ||
           std::holds_alternative<RemoveColumn>(edit) || std::holds_alternative<RemoveDeck>(edit) ||
           std::holds_alternative<SetDeckLayer>(edit);
}

} // namespace

void checkAutopilotBars(std::optional<int> bars, std::optional<int> hold)
{
    if (bars && (*bars < 1 || *bars > kMaxStepBars))
        throw ProtocolError("autopilot bars must be within 1..64");
    if (hold && (*hold < 0 || *hold > kMaxStepBars))
        throw ProtocolError("autopilot hold must be within 0..64");
}

Edit apply(Composition& composition, LoadComposition& edit)
{
    if (edit.composition.modulators.size() > kMaxModulators)
        throw ProtocolError("a composition holds at most " + std::to_string(kMaxModulators) + " modulators");
    IdPool ids;
    forEachChain(edit.composition, [&](const std::vector<Effect>& effects, uint64_t, std::optional<uint64_t>) {
        for (const Effect& effect : effects)
            ids.take(effect.id, "effect");
    });
    LoadComposition inverse{composition};
    composition = edit.composition;
    return inverse;
}

Edit apply(Composition& composition, Batch& edit, std::vector<std::string>* dropped = nullptr);

// The macro targets, modulators and Cue steps on a layer or effect the edit removes go with it, in the same undo step:
// the inverse puts the effect back first, so the restored targets find it.
Edit applyEdit(Composition& composition, Edit& edit, std::vector<std::string>* dropped)
{
    auto applyOne = [&] {
        return std::visit(
            [&](auto& e) {
                if constexpr (std::is_same_v<std::decay_t<decltype(e)>, Batch>)
                    return apply(composition, e, dropped);
                else
                    return apply(composition, e);
            },
            edit);
    };
    if (!mayRemoveOwners(edit))
        return applyOne();
    const Owners before = ownersIn(composition);
    Edit inverse = applyOne();
    const Owners after = ownersIn(composition);
    const std::set<uint64_t> effects = gone(before.effects, after.effects);
    std::vector<std::string> names;
    std::optional<SetMacros> restore = dropTargets(composition, gone(before.layers, after.layers), effects, names);
    std::vector<SetCue> cues = dropCueSteps(composition, effects, names);
    if (dropped)
        dropped->insert(dropped->end(), names.begin(), names.end());
    if (!restore && cues.empty())
        return inverse;
    Batch undo{{std::move(inverse)}};
    if (restore)
        undo.edits.push_back(std::move(*restore));
    for (SetCue& cue : cues)
        undo.edits.push_back(std::move(cue));
    return undo;
}

std::string droppedNotice(const std::vector<std::string>& dropped)
{
    std::string notice = "Gone with what the edit removed: ";
    for (size_t i = 0; i < dropped.size(); ++i)
        notice += (i ? "; " : "") + dropped[i];
    return notice;
}

// The edits applied so far are reverted when one throws.
Edit apply(Composition& composition, Batch& edit, std::vector<std::string>* dropped)
{
    Batch inverse;
    try {
        for (Edit& e : edit.edits) {
            if (isLive(e))
                throw ProtocolError("a batch cannot hold the live action " + editToJson(e).value("op", std::string()));
            inverse.edits.insert(inverse.edits.begin(), applyEdit(composition, e, dropped));
        }
    } catch (...) {
        for (Edit& undo : inverse.edits)
            applyEdit(composition, undo);
        throw;
    }
    return inverse;
}

} // namespace azl
