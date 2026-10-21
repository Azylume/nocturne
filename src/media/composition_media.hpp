#pragma once

#include "document/composition_file.hpp"
#include "document/screen_roles.hpp"
#include "media/media_library.hpp"

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace azl {

// A composition saved to move between machines: the composition file format (composition_file.hpp) where every cell
// also carries its media's "path", relative to a media root when the file lies inside it, and "size"; "mediaRoot"
// records the saving machine's root, for copying the media (nocturne-ctl showmedia). The file keeps no beats of the
// engine that saved it: layers start their cells when loaded.
//
// Loading resolves each cell's media by its path under the media root, then by its name anywhere under the root, then
// by its content hash (the Media Library, then files of its size under the root), and adds what it finds to the
// library. A cell whose media is not found loads empty, an effect of an unknown type is skipped with the macro targets
// and modulators on it. A screen goes to this machine's display for its role (screen_roles.hpp) when the machine has
// one, else to its own; one whose display is not attached moves to the first free attached display, unless this
// machine has seen that display before (a projector switched off): then it stays parked until the display attaches.
// Each is a warning, never an error.
struct LoadedComposition {
    Composition composition;
    std::vector<std::string> warnings;
    // What dropRemovedEffects took out, also among the warnings.
    std::vector<std::string> removed{};
};

// The engine machine's displays as a load places screens on them. `known`: every id ever attached here; `remap`
// moves a screen whose display is not attached even when it is known; `roles`: this machine's display per role.
struct DisplayPlacement {
    std::vector<std::string> attached;
    std::set<std::string> known;
    bool remap = false;
    RoleDisplays roles{};
};

// An empty `mediaRoot` is the file's own folder.
std::filesystem::path mediaRootOf(const std::filesystem::path& file, const std::filesystem::path& mediaRoot);

// Throws std::runtime_error when the file cannot be written; media the library cannot find are warnings.
std::vector<std::string> saveComposition(const std::filesystem::path& file, const Composition& composition,
                                         MediaLibrary& library, const std::filesystem::path& mediaRoot = {});

// Throws when the file is missing, unreadable or not a composition file.
LoadedComposition loadComposition(const std::filesystem::path& file, MediaLibrary& library,
                                  const DisplayPlacement& displays, const std::filesystem::path& mediaRoot = {});

// Unknown effects and parameters, and macro targets and modulators that drive nothing, are removed; out-of-range
// parameters are wrapped round (angles and hues) or clamped with a warning, as are ranges wider than their parameter's.
void dropUnknownEffects(Composition& composition, std::vector<std::string>& warnings);
// Takes out every instance of an effect type that no longer exists (Bloom, Classic Bloom), and the macro targets,
// modulators and Cue steps on them; a Cue left with nothing to do goes too. One line per thing dropped.
std::vector<std::string> dropRemovedEffects(Composition& composition);
// The notice a load shows for what dropRemovedEffects took out.
std::string removedEffectsNotice(const std::vector<std::string>& dropped);
void moveToAttachedDisplays(Composition& composition, const DisplayPlacement& displays,
                            std::vector<std::string>& warnings);

} // namespace azl
