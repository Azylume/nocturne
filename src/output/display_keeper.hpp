#pragma once

#include "document/composition.hpp"
#include "output/known_displays.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace azl {

// What keeps screens on their own displays through a replug: macOS turning mirroring back on, and a display behind a
// splitter coming back under a new id (its port changed). Pure decisions; OutputService acts on them.

// Each set of displays mirrored together whose members carry two or more of the screens: every member's id.
std::vector<std::vector<std::string>> mirrorsAcrossScreens(const Composition& composition,
                                                           const std::vector<DisplayInfo>& attached);

struct DisplayRebind {
    uint64_t screen = 0;
    std::string from;
    std::string to;
};

// Screens whose display is gone while a display attached since `before`, bound to no screen, is that same projector
// under another id: same vendor and model (else name), and serial where both report one. A projector two missing
// displays or two new ones could be is told apart by port, then by position, or left alone.
std::vector<DisplayRebind> returnedDisplays(const Composition& composition, const std::vector<DisplayInfo>& attached,
                                            const std::set<std::string>& before, const KnownDisplays& known);

} // namespace azl
