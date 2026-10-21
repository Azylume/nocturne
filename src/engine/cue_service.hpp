#pragma once

#include "engine/engine.hpp"
#include "protocol/document_server.hpp"

#include <functional>

namespace azl {

// Serves Sections and Cues as "cue" messages. Cues themselves are document state, edited with undoable commands
// (addCue, removeCue, setCue, setCueEnabled, setCueRepeat). Marking Sections and taking back firings are playback, like
// triggers: live setTimeline changes (cause "cue") outside the undo history, so every client and every engine sees the
// same timeline and, on the same beat clock, holds the same values on the same beat.
//
//   {"op": "mark", "section": "drop"[, "bar": 33]} -> change setTimeline {timeline}: the Section starts on the next bar,
//                       or on bar 33 of the beat clock (bar 1 starts on beat 0); a mark on that beat is replaced
//   {"op": "cancel"[, "bar": 33 | "cue": 3]} -> change setTimeline: drops every scheduled mark, or the one on bar 33,
//                       or skips the Cue's next firing
//   {"op": "clear"}  -> change setTimeline: an empty timeline, for a new track
//   {"op": "state"}  -> {"op": "state", "beat", "section", "start", "end", "sections": [{"section", "beat"}],
//                       "active": [{"cue", "beat", "until"}], "upcoming": [...], "autopilot": [{"layer", "mode",
//                       "bars"}], "macros": [{"macro", "value"}]} at the beat now; a start or end that is none is null
//
// Every message has "type": "cue"; state replies go to the sender, errors as {"type": "error"}. A resync moves the
// timeline onto the new beats (see ClockService).
class CueService {
public:
    using Now = std::function<double()>;

    CueService(DocumentServer& document, Engine& engine, Now now);
    ~CueService();

private:
    void handle(const Message& message, const DocumentServer::Sink& reply);

    DocumentServer& document_;
    Engine& engine_;
    Now now_;
};

// The beat at `now` on the engine's clock, from its last frame's beat and tempo.
double beatAt(const Engine& engine, double now);

} // namespace azl
