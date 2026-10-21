#include "document/edit_support.hpp"

#include "document/cue_edits.hpp"
#include "document/mapping_edits.hpp"
#include "document/modulation_edits.hpp"
#include "document/palette.hpp"
#include "document/text_edits.hpp"

#include <algorithm>
#include <cmath>

// Edits of the clip grid and composition settings: columns, decks, quantise, pace, column Autopilot, frame guard
// and names.

namespace azl {

using namespace edits;

namespace {

std::string defaultName(const std::string& name, const char* kind, uint64_t id)
{
    return name.empty() ? kind + std::to_string(id) : name;
}

void checkCell(Composition& composition, const Cell& cell)
{
    checkClip(findLayer(composition, cell.layer), cell.clip);
}

void fillCells(Composition& composition, const std::vector<Cell>& cells)
{
    for (const Cell& cell : cells)
        findLayer(composition, cell.layer).clips[cell.column] = cell.clip;
}

std::vector<Cell> takeCells(Composition& composition, uint64_t column)
{
    std::vector<Cell> cells;
    forEachLayer(composition.layers, [&](Layer& layer) {
        auto it = layer.clips.find(column);
        if (it == layer.clips.end())
            return;
        cells.push_back({layer.id, column, it->second});
        layer.clips.erase(it);
    });
    return cells;
}

} // namespace

Edit apply(Composition& composition, AddColumn& edit)
{
    Deck& deck = findDeck(composition, edit.deck);
    const size_t index = edit.index.value_or(deck.columns.size());
    if (index > deck.columns.size())
        throw ProtocolError("column index out of range");
    IdPool ids = columnIds(composition);
    const uint64_t id = ids.take(edit.column.id, "column");
    const Column column{id, defaultName(edit.column.name, "Column ", id)};
    for (Cell& cell : edit.cells) {
        cell.column = id;
        checkCell(composition, cell);
    }
    completeCells(composition, edit.cells);
    deck.columns.insert(deck.columns.begin() + std::ptrdiff_t(index), column);
    fillCells(composition, edit.cells);
    edit.index = index;
    edit.column = column;
    return RemoveColumn{id};
}

Edit apply(Composition& composition, RemoveColumn& edit)
{
    const ColumnPlace p = placeColumn(composition, edit.column);
    AddColumn inverse{p.deck->id, p.index, p.deck->columns[p.index], takeCells(composition, edit.column)};
    p.deck->columns.erase(p.deck->columns.begin() + std::ptrdiff_t(p.index));
    return inverse;
}

Edit apply(Composition& composition, AddDeck& edit)
{
    const size_t index = edit.index.value_or(composition.decks.size());
    if (index > composition.decks.size())
        throw ProtocolError("deck index out of range");
    IdPool decks = deckIds(composition);
    IdPool columns = columnIds(composition);
    const uint64_t id = decks.take(edit.deck.id, "deck");
    Deck deck{id, defaultName(edit.deck.name, "Deck ", id), {}};
    for (const Column& given : edit.deck.columns) {
        const uint64_t column = columns.take(given.id, "column");
        deck.columns.push_back({column, defaultName(given.name, "Column ", column)});
    }
    for (const Cell& cell : edit.cells) {
        auto named = [&](const Column& column) { return column.id == cell.column; };
        if (std::none_of(deck.columns.begin(), deck.columns.end(), named))
            throw ProtocolError("a cell names column " + std::to_string(cell.column) + ", not one of the deck's");
        checkCell(composition, cell);
    }
    completeCells(composition, edit.cells);
    IdPool effects = effectIds(composition);
    for (const Cell& cell : edit.cells)
        for (const Effect& effect : cell.clip.effects)
            effects.reserve(effect.id);
    for (auto& [layer, settings] : edit.layers)
        completeDeckLayer(findLayer(composition, layer), settings, effects);
    composition.decks.insert(composition.decks.begin() + std::ptrdiff_t(index), deck);
    fillCells(composition, edit.cells);
    for (const auto& [layer, settings] : edit.layers)
        findLayer(composition, layer).decks[id] = settings;
    edit.index = index;
    edit.deck = std::move(deck);
    return RemoveDeck{id};
}

Edit apply(Composition& composition, RemoveDeck& edit)
{
    auto it = std::find_if(composition.decks.begin(), composition.decks.end(),
                           [&](const Deck& deck) { return deck.id == edit.deck; });
    if (it == composition.decks.end())
        throw ProtocolError("no deck " + std::to_string(edit.deck));
    if (composition.decks.size() == 1)
        throw ProtocolError("the last deck stays");
    AddDeck inverse{size_t(it - composition.decks.begin()), *it, {}};
    for (const Column& column : it->columns)
        for (Cell& cell : takeCells(composition, column.id))
            inverse.cells.push_back(std::move(cell));
    forEachLayer(composition.layers, [&](Layer& layer) {
        if (auto own = layer.decks.find(edit.deck); own != layer.decks.end()) {
            inverse.layers[layer.id] = std::move(own->second);
            layer.decks.erase(own);
        }
    });
    composition.decks.erase(it);
    if (composition.deck == edit.deck)
        composition.deck = composition.decks.front().id;
    return inverse;
}

Edit apply(Composition& composition, MoveDeck& edit)
{
    auto it = std::find_if(composition.decks.begin(), composition.decks.end(),
                           [&](const Deck& deck) { return deck.id == edit.deck; });
    if (it == composition.decks.end())
        throw ProtocolError("no deck " + std::to_string(edit.deck));
    if (edit.index >= composition.decks.size())
        throw ProtocolError("deck index out of range");
    const MoveDeck inverse{edit.deck, size_t(it - composition.decks.begin())};
    Deck deck = std::move(*it);
    composition.decks.erase(it);
    composition.decks.insert(composition.decks.begin() + std::ptrdiff_t(edit.index), std::move(deck));
    return inverse;
}

Edit apply(Composition& composition, SetQuantise& edit)
{
    const int beats = edit.beats;
    checkQuantise(beats);
    const SetQuantise inverse{composition.quantise};
    composition.quantise = beats;
    return inverse;
}

Edit apply(Composition& composition, SetAutopilotPace& edit)
{
    const double paces[] = {0.5, 1.0, 2.0, 4.0, 8.0};
    if (std::find(std::begin(paces), std::end(paces), edit.pace) == std::end(paces))
        throw ProtocolError("autopilot pace must be 0.5, 1, 2, 4 or 8");
    const SetAutopilotPace inverse{composition.pace};
    composition.pace = edit.pace;
    return inverse;
}

Edit apply(Composition& composition, SetColumnAutopilot& edit)
{
    checkAutopilotBars(edit.bars, edit.hold);
    Autopilot& autopilot = composition.columnAutopilot;
    const SetColumnAutopilot inverse{autopilot.mode, autopilot.bars, autopilot.hold};
    autopilot.mode = edit.mode.value_or(autopilot.mode);
    autopilot.bars = edit.bars.value_or(autopilot.bars);
    autopilot.hold = edit.hold.value_or(autopilot.hold);
    return inverse;
}

Edit apply(Composition& composition, SetFrameGuard& edit)
{
    const SetFrameGuard inverse{composition.frameGuard};
    composition.frameGuard = edit.on;
    return inverse;
}

Edit apply(Composition& composition, Rename& edit)
{
    std::string* name = nullptr;
    if (edit.kind == "layer") {
        name = &findLayer(composition, edit.id).name;
    } else if (edit.kind == "deck" || edit.kind == "column") {
        for (Deck& deck : composition.decks) {
            if (edit.kind == "deck" && deck.id == edit.id)
                name = &deck.name;
            for (Column& column : deck.columns)
                if (edit.kind == "column" && column.id == edit.id)
                    name = &column.name;
        }
    } else if (edit.kind == "macro") {
        for (Macro& macro : composition.macros)
            if (macro.id == edit.id)
                name = &macro.name;
    } else if (edit.kind == "screen" || edit.kind == "slice" || edit.kind == "mask") {
        for (Screen& screen : composition.screens) {
            if (edit.kind == "screen" && screen.id == edit.id)
                name = &screen.name;
            for (Mask& mask : screen.masks)
                if (edit.kind == "mask" && mask.id == edit.id)
                    name = &mask.name;
            for (Slice& slice : screen.slices) {
                if (edit.kind == "slice" && slice.id == edit.id)
                    name = &slice.name;
                for (Mask& mask : slice.masks)
                    if (edit.kind == "mask" && mask.id == edit.id)
                        name = &mask.name;
            }
        }
    } else {
        throw ProtocolError("rename takes a layer, deck, column, macro, screen, slice or mask, not " + edit.kind);
    }
    if (!name)
        throw ProtocolError("no " + edit.kind + " " + std::to_string(edit.id));
    if (edit.name.empty() || edit.name.size() > 256)
        throw ProtocolError("a name needs 1..256 bytes");
    Rename inverse{edit.kind, edit.id, *name};
    *name = edit.name;
    return inverse;
}

} // namespace azl
