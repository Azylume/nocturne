#include "document/edit_support.hpp"

#include "document/cue_edits.hpp"
#include "document/mapping_edits.hpp"
#include "document/modulation_edits.hpp"
#include "document/palette.hpp"
#include "document/text_edits.hpp"

#include <algorithm>
#include <cmath>

// Edits of screens and slices.

namespace azl {

using namespace edits;

namespace {

IdPool screenIds(const Composition& composition)
{
    IdPool ids;
    for (const Screen& screen : composition.screens)
        ids.reserve(screen.id);
    return ids;
}

IdPool sliceIds(const Composition& composition)
{
    IdPool ids;
    for (const Screen& screen : composition.screens)
        for (const Slice& slice : screen.slices)
            ids.reserve(slice.id);
    return ids;
}

Screen& findScreen(Composition& composition, uint64_t id)
{
    for (Screen& screen : composition.screens)
        if (screen.id == id)
            return screen;
    throw ProtocolError("no screen " + std::to_string(id));
}

struct SlicePlace {
    Screen* screen = nullptr;
    size_t index = 0;
};

SlicePlace findSlice(Composition& composition, uint64_t id)
{
    for (Screen& screen : composition.screens)
        for (size_t i = 0; i < screen.slices.size(); ++i)
            if (screen.slices[i].id == id)
                return {&screen, i};
    throw ProtocolError("no slice " + std::to_string(id));
}

// One screen per display: two would cover the same display.
void checkDisplayFree(const Composition& composition, const std::string& display, uint64_t screen)
{
    if (display.empty())
        return;
    for (const Screen& other : composition.screens)
        if (other.display == display && other.id != screen)
            throw ProtocolError("display " + display + " already has screen " + std::to_string(other.id));
}

void checkInput(Composition& composition, uint64_t input)
{
    if (input != 0)
        findLayer(composition, input);
}

void checkInputRect(const std::array<double, 4>& r)
{
    constexpr double kSlack = 1e-9;
    if (!(r[0] >= 0.0 && r[1] >= 0.0 && r[2] > 0.0 && r[3] > 0.0 && r[0] + r[2] <= 1.0 + kSlack &&
          r[1] + r[3] <= 1.0 + kSlack))
        throw ProtocolError("input rect must lie within 0..1 with a positive size");
}

// Corners may reach past the screen's edges, e.g. to overscan a projector.
void checkCorners(const std::array<double, 8>& corners)
{
    for (double c : corners)
        if (!(c >= -1.0 && c <= 2.0))
            throw ProtocolError("slice corners must lie within -1..2");
}

void completeSlice(Composition& composition, Slice& slice, IdPool& ids)
{
    checkInput(composition, slice.input);
    checkInputRect(slice.inputRect);
    checkCorners(slice.corners);
    checkSliceMapping(slice);
    slice.id = ids.take(slice.id, "slice");
    if (slice.name.empty())
        slice.name = "Slice " + std::to_string(slice.id);
}

std::vector<Mask*> pointers(std::vector<Mask>& masks)
{
    std::vector<Mask*> out;
    for (Mask& mask : masks)
        out.push_back(&mask);
    return out;
}

void checkSliceRoom(const Screen& screen, size_t adding)
{
    if (screen.slices.size() + adding > kMaxSlicesPerScreen)
        throw ProtocolError("a screen holds at most " + std::to_string(kMaxSlicesPerScreen) + " slices");
}

} // namespace

Edit apply(Composition& composition, AddScreen& edit)
{
    const size_t index = edit.index.value_or(composition.screens.size());
    if (index > composition.screens.size())
        throw ProtocolError("screen index out of range");
    Screen screen = edit.screen;
    checkDisplayFree(composition, screen.display, 0);
    checkSliceRoom(screen, 0);
    IdPool screens = screenIds(composition);
    IdPool slices = sliceIds(composition);
    checkColor(screen.color);
    checkMaskRoom(screen.masks);
    std::vector<Mask*> masks = pointers(screen.masks);
    for (Slice& slice : screen.slices) {
        completeSlice(composition, slice, slices);
        checkMaskRoom(slice.masks);
        for (Mask* mask : pointers(slice.masks))
            masks.push_back(mask);
    }
    completeMasks(composition, masks);
    screen.id = screens.take(screen.id, "screen");
    if (screen.name.empty())
        screen.name = "Screen " + std::to_string(screen.id);
    composition.screens.insert(composition.screens.begin() + std::ptrdiff_t(index), screen);
    edit.index = index;
    edit.screen = std::move(screen);
    return RemoveScreen{edit.screen.id};
}

Edit apply(Composition& composition, AssignDisplay& edit)
{
    const Layer& group = findLayer(composition, edit.group);
    if (!group.group)
        throw ProtocolError("layer " + std::to_string(edit.group) + " is not a group");
    if (edit.display.empty())
        throw ProtocolError("missing display");
    AddScreen add{std::nullopt, edit.screen.value_or(Screen{.name = group.name, .display = edit.display,
                                                            .slices = {Slice{.input = group.id}}})};
    Edit inverse = apply(composition, add);
    edit.screen = add.screen;
    return inverse;
}

Edit apply(Composition& composition, RemoveScreen& edit)
{
    auto& screens = composition.screens;
    auto it = std::find_if(screens.begin(), screens.end(), [&](const Screen& s) { return s.id == edit.screen; });
    if (it == screens.end())
        throw ProtocolError("no screen " + std::to_string(edit.screen));
    AddScreen inverse{size_t(it - screens.begin()), std::move(*it)};
    screens.erase(it);
    return inverse;
}

Edit apply(Composition& composition, SetScreenDisplay& edit)
{
    Screen& screen = findScreen(composition, edit.screen);
    checkDisplayFree(composition, edit.display, screen.id);
    const SetScreenDisplay inverse{screen.id, screen.display};
    screen.display = edit.display;
    return inverse;
}

Edit apply(Composition& composition, SetScreenTestCard& edit)
{
    if (edit.screen == 0) {
        const SetScreenTestCard inverse{0, composition.testCard};
        composition.testCard = edit.testCard;
        return inverse;
    }
    Screen& screen = findScreen(composition, edit.screen);
    const SetScreenTestCard inverse{screen.id, screen.testCard};
    screen.testCard = edit.testCard;
    return inverse;
}

Edit apply(Composition& composition, SetScreenPosition& edit)
{
    Screen& screen = findScreen(composition, edit.screen);
    if (edit.position && !(std::isfinite((*edit.position)[0]) && std::isfinite((*edit.position)[1])))
        throw ProtocolError("screen position is not a number");
    const SetScreenPosition inverse{screen.id, screen.mapPosition};
    screen.mapPosition = edit.position;
    return inverse;
}

Edit apply(Composition& composition, SetScreenHidden& edit)
{
    Screen& screen = findScreen(composition, edit.screen);
    const SetScreenHidden inverse{screen.id, screen.hidden};
    screen.hidden = edit.hidden;
    return inverse;
}

Edit apply(Composition& composition, MoveScreen& edit)
{
    auto& screens = composition.screens;
    auto it = std::find_if(screens.begin(), screens.end(), [&](const Screen& s) { return s.id == edit.screen; });
    if (it == screens.end())
        throw ProtocolError("no screen " + std::to_string(edit.screen));
    if (edit.index >= screens.size())
        throw ProtocolError("screen index out of range");
    const MoveScreen inverse{edit.screen, size_t(it - screens.begin())};
    Screen screen = std::move(*it);
    screens.erase(it);
    screens.insert(screens.begin() + std::ptrdiff_t(edit.index), std::move(screen));
    return inverse;
}

Edit apply(Composition& composition, AddSlice& edit)
{
    Screen& screen = findScreen(composition, edit.screen);
    checkSliceRoom(screen, 1);
    const size_t index = edit.index.value_or(screen.slices.size());
    if (index > screen.slices.size())
        throw ProtocolError("slice index out of range");
    IdPool ids = sliceIds(composition);
    Slice slice = edit.slice;
    completeSlice(composition, slice, ids);
    checkMaskRoom(slice.masks);
    completeMasks(composition, pointers(slice.masks));
    screen.slices.insert(screen.slices.begin() + std::ptrdiff_t(index), slice);
    edit.index = index;
    edit.slice = std::move(slice);
    return RemoveSlice{edit.slice.id};
}

Edit apply(Composition& composition, RemoveSlice& edit)
{
    const SlicePlace p = findSlice(composition, edit.slice);
    auto& slices = p.screen->slices;
    AddSlice inverse{p.screen->id, p.index, std::move(slices[p.index])};
    slices.erase(slices.begin() + std::ptrdiff_t(p.index));
    return inverse;
}

Edit apply(Composition& composition, SetSliceCorners& edit)
{
    checkCorners(edit.corners);
    const SlicePlace p = findSlice(composition, edit.slice);
    Slice& slice = p.screen->slices[p.index];
    const SetSliceCorners inverse{slice.id, slice.corners};
    slice.corners = edit.corners;
    return inverse;
}

Edit apply(Composition& composition, SetSliceInput& edit)
{
    if (edit.input)
        checkInput(composition, *edit.input);
    if (edit.inputRect)
        checkInputRect(*edit.inputRect);
    const SlicePlace p = findSlice(composition, edit.slice);
    Slice& slice = p.screen->slices[p.index];
    SetSliceInput inverse{slice.id, std::nullopt, std::nullopt};
    if (edit.input) {
        inverse.input = slice.input;
        slice.input = *edit.input;
    }
    if (edit.inputRect) {
        inverse.inputRect = slice.inputRect;
        slice.inputRect = *edit.inputRect;
    }
    return inverse;
}

} // namespace azl
