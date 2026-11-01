#pragma once

#include "document/composition.hpp"

#include <map>
#include <string>
#include <vector>

namespace azl {

// The displays a machine gives each screen role (ADR 0011: the machine owns its displays, the show names roles). A
// screen's role is its name; the n-th screen with a role takes the role's n-th display, an empty one meaning no output.
using RoleDisplays = std::map<std::string, std::vector<std::string>>;

std::string screenRole(const Screen& screen);

// Puts every screen whose role `roles` maps on its display. A screen left unmapped keeps its own display unless a
// mapped screen took it, and so does a mapped one whose display an earlier screen took; those lose it, with a warning.
void placeByRole(Composition& composition, const RoleDisplays& roles, std::vector<std::string>& warnings);

// Records where the composition's screens are into `roles`, keeping the other roles and the displays of a role past
// its screens here. A role not mapped yet enters only with a display. True when `roles` changed.
bool recordRoles(RoleDisplays& roles, const Composition& composition);

// Keeps the screens off `uiDisplay`, the display of a UI on this machine: a screen there moves to the first `attached`
// display that is not the UI's and that no screen has, else to no output. An empty `uiDisplay` changes nothing.
void keepOffUiDisplay(Composition& composition, const std::vector<std::string>& attached, const std::string& uiDisplay);

} // namespace azl
