#pragma once

#include "engine/engine.hpp"
#include "protocol/document_server.hpp"

#include <functional>

namespace azl {

// Serves clip, column and deck triggers as "trigger" messages. Triggering is a performance action, like tap or resync:
// it lands on a musical moment that has passed once the music moves on, so it stays out of the undo history. Each
// trigger is still a numbered document change (cause "trigger", from the sender), so every client and every engine
// sees which cell plays and from which beat.
//
//   {"type": "trigger", "op": "clip", "layer": 1, "column": 2}    -> change triggerClip {layer, column, beat}
//   {"type": "trigger", "op": "column", "column": 2}             -> change triggerColumn {column, beat}
//   {"type": "trigger", "op": "deck", "deck": 2}                 -> change selectDeck {deck}
//   {"type": "trigger", "op": "arm", "layer": 1, "column": 2, "armed": true}
//                                                                -> change armClip {layer, column, armed}
//   {"type": "trigger", "op": "stop", "layer": 1}                -> change stopLayer {layer}: plays nothing, at once
//   {"type": "trigger", "op": "cancel"[, "layer": 1 | "column": 2]}
//                                                                -> change cancelTrigger {beat, layers}
//
// A trigger may name its hand, "by": "ui" or "ctl"; the change then carries that cause instead of "trigger", so the
// switch log (switch_log.hpp) tells them apart.
//
// Cancel takes back the triggers still waiting for their beat (all, a layer's, or a column's): each layer goes back
// to the cell it shows, as a second click on a queued cell or Esc does. With none waiting it is an error.
//
// A trigger's beat is the beat clock at `now` when it arrives, moved onto the composition's quantisation: the next bar
// by default. The switch happens on the first frame at or after that beat. A clip that cannot be played is reported to
// every client as an error naming its layer and hash; the layer keeps its picture.
class TriggerService {
public:
    using Now = std::function<double()>;

    TriggerService(DocumentServer& document, Engine& engine, Now now);
    ~TriggerService();

private:
    void handle(const Message& message, const DocumentServer::Sink& reply);
    Edit edit(const Message& message) const;

    DocumentServer& document_;
    Engine& engine_;
    Now now_;
};

} // namespace azl
