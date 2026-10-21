#pragma once

#include "document/composition.hpp"
#include "document/id_pool.hpp"

#include "effects/effect_registry.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// What the edit units share: finding what an edit names, checking and completing what enters the document, and the
// apply of each edit, which applyEdit dispatches to. Each family of edits lives in its own unit (clip_edits.cpp,
// effect_edits.cpp, layer_edits.cpp, screen_edits.cpp, grid_edits.cpp), so a change to one recompiles only that one.

namespace azl {

namespace edits {

// Where a layer sits: its siblings, its position among them and the group holding it (0 for the composition).
struct Place {
    std::vector<Layer>* siblings = nullptr;
    size_t index = 0;
    uint64_t parent = 0;
};

std::optional<Place> locate(std::vector<Layer>& layers, uint64_t id, uint64_t parent);
Place place(Composition& composition, uint64_t id);
Layer& findLayer(Composition& composition, uint64_t id);

// The layers of `parent`, which is 0 for the composition or a group.
std::vector<Layer>& children(Composition& composition, uint64_t parent);

bool isContentHash(const std::string& s);
void checkBeats(int beats);
void checkJumpInterval(double interval);
void checkJumpDistance(double distance);
void checkSpeedRange(const Transport& t);
void checkQuantise(int beats);
void checkStep(int bars);
void checkTransitionBeats(double beats);
// The beat moved onto the next multiple of `quantise` beats; 0 leaves it.
double quantised(double beat, int quantise);
// Steps name parameters of the effect's type, within their ranges, and there are at most kMaxEffectSteps.
void checkSteps(const EffectDef& def, const std::vector<EffectStep>& steps);

// A generator only starts the chain of a clip with no media and no text, and such a clip always starts with one.
void checkSources(const std::vector<Effect>& effects, bool generatorClip);

void checkClip(const Layer& layer, const std::optional<ClipRef>& clip);
Deck& findDeck(Composition& composition, uint64_t id);

struct ColumnPlace {
    Deck* deck = nullptr;
    size_t index = 0;
};

ColumnPlace placeColumn(Composition& composition, uint64_t id);
bool hasColumn(const Composition& composition, uint64_t id);
void requireColumn(const Composition& composition, uint64_t id);
const EffectDef& effectDef(const std::string& type);
const ParamDef& paramDef(const EffectDef& def, const std::string& name);
void checkParam(const EffectDef& def, const std::string& name, double value);
std::map<std::string, double> completeParams(const EffectDef& def, const std::map<std::string, double>& given);

// Every effect id in use, except those of the chain `replaced` that an edit is about to replace.
IdPool effectIds(const Composition& composition, const std::vector<Effect>* replaced = nullptr);

IdPool layerIds(const Composition& composition);
IdPool columnIds(const Composition& composition);
IdPool deckIds(const Composition& composition);
Effect completeEffect(const Effect& given, IdPool& ids);
void checkChainRoom(size_t size);
void completeEffects(std::vector<Effect>& effects, IdPool& ids);
void completeClip(const Composition& composition, ClipRef& clip, const ClipRef* replaced);
void completeCells(const Composition& composition, std::vector<Cell>& cells);
void requireDeck(const Composition& composition, uint64_t id);

// Checks a layer's settings in a deck and completes their chain.
void completeDeckLayer(const Layer& layer, DeckLayer& settings, IdPool& effects);

} // namespace edits

Edit apply(Composition& composition, SetLayerOpacity& edit);
Edit apply(Composition& composition, SetLayerClip& edit);
Edit apply(Composition& composition, SetSlot& edit);
Edit apply(Composition& composition, SetTempo& edit);
Edit apply(Composition& composition, SetClipTransport& edit);
Edit apply(Composition& composition, SetClipTrigger& edit);
Edit apply(Composition& composition, SetLayerAutopilot& edit);
Edit apply(Composition& composition, SetClipStep& edit);
Edit apply(Composition& composition, SetLayerTransition& edit);
Edit apply(Composition& composition, SetClipTransition& edit);
Edit apply(Composition& composition, SetTransitionParam& edit);
Edit apply(Composition& composition, AddEffect& edit);
Edit apply(Composition& composition, RemoveEffect& edit);
Edit apply(Composition& composition, MoveEffect& edit);
Edit apply(Composition& composition, SetEffectParam& edit);
Edit apply(Composition& composition, SetEffectBypass& edit);
Edit apply(Composition& composition, SetSensitivity& edit);
Edit apply(Composition& composition, SetBandEdges& edit);
Edit apply(Composition& composition, SetEffectParams& edit);
Edit apply(Composition& composition, SetEffectSteps& edit);
Edit apply(Composition& composition, SetEffectAutopilot& edit);
Edit apply(Composition& composition, TriggerStep& edit);
Edit apply(Composition& composition, SetLayerBlend& edit);
Edit apply(Composition& composition, SetLayerBypass& edit);
Edit apply(Composition& composition, SetLayerSolo& edit);
Edit apply(Composition& composition, SetLayerFullResolution& edit);
Edit apply(Composition& composition, SetDeckLayer& edit);
Edit apply(Composition& composition, AddLayer& edit);
Edit apply(Composition& composition, RemoveLayer& edit);
Edit apply(Composition& composition, MoveLayer& edit);
Edit apply(Composition& composition, SetGroupOutputOnly& edit);
Edit apply(Composition& composition, AddScreen& edit);
Edit apply(Composition& composition, AssignDisplay& edit);
Edit apply(Composition& composition, RemoveScreen& edit);
Edit apply(Composition& composition, SetScreenDisplay& edit);
Edit apply(Composition& composition, SetScreenTestCard& edit);
Edit apply(Composition& composition, SetScreenPosition& edit);
Edit apply(Composition& composition, SetScreenHidden& edit);
Edit apply(Composition& composition, MoveScreen& edit);
Edit apply(Composition& composition, AddSlice& edit);
Edit apply(Composition& composition, RemoveSlice& edit);
Edit apply(Composition& composition, SetSliceCorners& edit);
Edit apply(Composition& composition, SetSliceInput& edit);
Edit apply(Composition& composition, AddColumn& edit);
Edit apply(Composition& composition, RemoveColumn& edit);
Edit apply(Composition& composition, AddDeck& edit);
Edit apply(Composition& composition, RemoveDeck& edit);
Edit apply(Composition& composition, MoveDeck& edit);
Edit apply(Composition& composition, SetQuantise& edit);
Edit apply(Composition& composition, SetAutopilotPace& edit);
Edit apply(Composition& composition, SetColumnAutopilot& edit);
Edit apply(Composition& composition, SetFrameGuard& edit);
Edit apply(Composition& composition, Rename& edit);
Edit apply(Composition& composition, TriggerClip& edit);
Edit apply(Composition& composition, StopLayer& edit);
Edit apply(Composition& composition, TriggerColumn& edit);
Edit apply(Composition& composition, SelectDeck& edit);
Edit apply(Composition& composition, ArmClip& edit);

} // namespace azl
