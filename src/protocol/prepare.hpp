#pragma once

#include "document/composition.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace azl {

// Prepare: a Director edits a local copy of the venue's composition and sends it later as one undo step. The copy is
// the venue as it is now with Prepare's own edits on top, so playback and other clients' edits show through it.
//
// Each edit keeps what it touched in the composition's JSON (playback left out) and what that held on the venue when
// the edit was made. On a Send, an edit whose ground the venue has changed since is left out, as is one that no longer
// applies, so the venue's own changes win.

// Object keys from the composition's root; "#ID" is the element with that id of an array of objects with ids, "@order"
// the order of such an array's ids.
using DocPath = std::vector<std::string>;

struct Touch {
    DocPath path;
    // None when the path did not exist.
    std::optional<nlohmann::json> base;
};

struct PreparedEdit {
    Edit edit;
    std::vector<Touch> touched;
};

struct Rebased {
    Composition composition;
    std::vector<Edit> applied;
    // One line per edit left out: its op, where, and why.
    std::vector<std::string> skipped;
};

// The composition's JSON without playback (what plays, the deck and column shown, the timeline, the selected palette)
// and without the Cues that hold a Prepare change.
nlohmann::json documentJson(const Composition& composition);
// The edits that apply on top of `venue`, and the composition they make. With `keepDeck` an edit that would change the
// deck shown is left out, so a change sent at a deck switch never switches the deck itself.
Rebased rebase(const Composition& venue, const std::vector<PreparedEdit>& edits, bool keepDeck = false);

// [{edit fields..., "touched": [{"path": [...], "base": value}, ...]}, ...]; "base" is missing for a path that did not
// exist.
nlohmann::json preparedToJson(const std::vector<PreparedEdit>& edits);
// Throws ProtocolError when malformed.
std::vector<PreparedEdit> preparedFromJson(const nlohmann::json& j);

// The local copy and its own undo history.
class PreparedCopy {
public:
    explicit PreparedCopy(const Composition& venue);

    // The venue changed: the copy is Prepare's edits on top of it again.
    void follow(const Composition& venue);
    // Applies `edit` to the copy; throws ProtocolError and changes nothing when it does not apply. Edits of one nonzero
    // gesture make one undo step, and a repeated edit of the same value keeps only the last.
    void edit(Edit edit, uint64_t gesture = 0);
    bool undo();
    bool redo();

    const Composition& composition() const { return copy_.composition; }
    // The edits the venue's own changes leave out now.
    const std::vector<std::string>& skipped() const { return copy_.skipped; }
    // Every edit of the undo history, oldest first.
    std::vector<PreparedEdit> edits() const;
    bool empty() const { return undo_.empty(); }
    size_t undoSteps() const { return undo_.size(); }
    size_t redoSteps() const { return redo_.size(); }
    // The op of each step's first edit, oldest first.
    std::vector<std::string> ops(bool undo) const;

    // {"undo": [[edit, ...], ...], "redo": [...]}, each edit as preparedToJson writes it.
    nlohmann::json toJson() const;
    static PreparedCopy fromJson(const nlohmann::json& j, const Composition& venue);

private:
    struct Step {
        std::vector<PreparedEdit> edits;
        uint64_t gesture = 0;
    };

    void refresh();

    Composition venue_;
    nlohmann::json venueJson_;
    Rebased copy_;
    nlohmann::json copyJson_;
    std::vector<Step> undo_;
    std::vector<Step> redo_;
};

} // namespace azl
