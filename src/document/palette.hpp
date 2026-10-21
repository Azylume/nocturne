#pragma once

#include "document/composition.hpp"
#include "effects/colour.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace azl {

// None when the colour is not in the composition.
std::optional<Hsva> colourOf(const Composition& composition, const ColourRef& ref);
bool hasAlpha(const Composition& composition, const ColourRef& ref);
// "Color 1", "Text", "Outline", a palette colour's name.
std::string colourLabel(const Composition& composition, const ColourRef& ref);
// The edit that gives the colour this value; none when it is not in the composition.
std::optional<Edit> colourEdit(const Composition& composition, const ColourRef& ref, const Hsva& colour);
// The parameters macros and modulators drive to move the colour, channels then alpha; empty when none can (a text
// outline, a palette colour).
std::vector<ParamRef> colourParams(const Composition& composition, const ColourRef& ref);
// Every colour group of the effect.
std::vector<ColourRef> effectColours(const Composition& composition, uint64_t effect);

Rgba rgbaOf(const std::array<double, 4>& colour);
std::array<double, 4> arrayOf(const Rgba& colour);
const ColourPalette* paletteById(const Composition& composition, uint64_t id);
// The palette the pickers offer and colours link to: the selected one, else the first, or null when the show has none.
const ColourPalette* activePalette(const Composition& composition);
size_t activePaletteIndex(const Composition& composition);
// Adds the colour to the active palette; with no palette yet it creates one holding it.
SetPalette addToPalette(const Composition& composition, PaletteColour colour);
// "Colour N" for the next colour of the palette.
std::string nextColourName(const ColourPalette* palette);

// The palette colour id the colour follows, if it is linked.
std::optional<uint64_t> linkOf(const Composition& composition, const ColourRef& ref);
// Every linked colour with the palette colour id it follows.
std::vector<std::pair<ColourRef, uint64_t>> linkedColours(const Composition& composition);
// The colour in the active palette at the slot of the palette colour with this id, and that slot; null when there is
// none.
const PaletteColour* paletteColour(const Composition& composition, uint64_t id, size_t* slot = nullptr);
// The slot of the palette colour with this id in its palette.
std::optional<size_t> linkSlot(const Composition& composition, uint64_t id);

// A palette edit recolours every colour linked to a slot of the active palette it changes, and unlinks those whose
// colour leaves every palette; undo brings both back.
Edit apply(Composition& composition, SetPalette& edit);
Edit apply(Composition& composition, RemovePalette& edit);
Edit apply(Composition& composition, SetPaletteLink& edit);
Edit apply(Composition& composition, SelectPalette& edit);
Edit apply(Composition& composition, SetPaletteAutopilot& edit);
Edit apply(Composition& composition, SetDeckPalette& edit);
// Selects the deck's palette, if it has one, as SelectPalette does at `beat` (none: at once).
void selectDeckPalette(Composition& composition, uint64_t deck, std::optional<double> beat);
nlohmann::json toJson(const SetPalette& e);
nlohmann::json toJson(const RemovePalette& e);
nlohmann::json toJson(const SetPaletteLink& e);
nlohmann::json toJson(const SelectPalette& e);
nlohmann::json toJson(const SetPaletteAutopilot& e);
nlohmann::json toJson(const SetDeckPalette& e);
std::optional<Edit> paletteEditFromJson(const std::string& op, const nlohmann::json& j);

void to_json(nlohmann::json& j, const ColourPalette& palette);
void from_json(const nlohmann::json& j, ColourPalette& palette);
void to_json(nlohmann::json& j, const ColourRef& ref);
void from_json(const nlohmann::json& j, ColourRef& ref);

} // namespace azl
