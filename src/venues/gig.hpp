#pragma once

#include "document/composition.hpp"
#include "document/wall_clock.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace azl {

// Who plays from `start` (minutes from the venue's local midnight), on which deck of the venue's composition.
struct GigSlot {
    std::string dj;
    int start = 0;
    uint64_t deck = 0;

    bool operator==(const GigSlot&) const = default;
};

// One night at a venue, kept on the Director as one JSON file, a shape a later admin backend can own:
//
//   {"schema": 1, "venue": "mini", "date": "2026-10-16", "window": {"start": "22:00", "end": "05:00"},
//    "lineup": [{"dj": "Headliner", "start": "03:00", "deck": 2}]}
//
// Times are the venue's local time, HH:MM; a window end or a slot start not after the window's start is on the next
// day. `deck` is a deck id of the venue's composition.
struct Gig {
    std::string venue;
    std::string date;
    int start = 22 * 60;
    int end = 5 * 60;
    std::vector<GigSlot> lineup;

    bool operator==(const Gig&) const = default;
};

inline constexpr int kGigSchema = 1;

void to_json(nlohmann::json& j, const Gig& gig);
// Throws ProtocolError when it is not a valid Gig.
void from_json(const nlohmann::json& j, Gig& gig);
// Throws ProtocolError naming what is wrong.
void checkGig(const Gig& gig);

// ~/Library/Application Support/Nocturne/Gigs
std::filesystem::path defaultGigsFolder();
// <folder>/<venue>/<date>.json
std::filesystem::path gigPath(const std::filesystem::path& folder, const std::string& venue, const std::string& date);
Gig loadGig(const std::filesystem::path& path);
// Replaces the file whole, creating its folder.
void saveGig(const std::filesystem::path& path, const Gig& gig);
// The venue's readable Gigs in date order.
std::vector<Gig> venueGigs(const std::filesystem::path& folder, const std::string& venue);
// The first Gig that has not ended by `now` (Unix seconds).
std::optional<Gig> currentGig(const std::vector<Gig>& gigs, double now, const UtcOffset& zone = systemUtcOffset);

// The date of the night's slot starting at `minute`: the Gig's date, or the next day after midnight.
std::string slotDate(const Gig& gig, int minute);
GigWindow gigWindow(const Gig& gig);
// A deck switch per slot at its start, firing the deck's first column, marked with the Gig's date.
std::vector<Cue> lineupCues(const Gig& gig);
// Saving the Gig to a venue showing `composition`, as one undo step: its window, and its lineup Cues in place of the
// ones this Gig made before. Hand-made Cues and other Gigs' stay.
Edit gigEdit(const Gig& gig, const Composition& composition);
// Moves slot `slot` (0 is the first) by `minutes`, and with `following` every slot after it too.
void shiftSlot(Gig& gig, size_t slot, int minutes, bool following = false);

// nocturne-ctl's commands for these, whole, parsed here so they are tested:
//   cue at HH:MM NAME [date:YYYY-MM-DD] [deck:DECK] [column:COLUMN | column:#N]
//   gig show [DATE] | gig apply [DATE] | gig set DATE HH:MM-HH:MM [DJ@HH:MM=DECK ...]
//   gig shift SLOT MINUTES [following] [DATE]        (SLOT is 1-based)
// Each throws std::runtime_error on words it does not understand.
Cue cueAtWords(const std::vector<std::string>& words);
struct GigCommand {
    std::string op;
    std::optional<std::string> date;
    // For set: the Gig without its venue.
    Gig gig;
    size_t slot = 0;
    int minutes = 0;
    bool following = false;

    bool operator==(const GigCommand&) const = default;
};
GigCommand gigCommandWords(const std::vector<std::string>& words);

} // namespace azl
