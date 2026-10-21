#pragma once

#include "engine/beat_clock.hpp"
#include "engine/engine.hpp"
#include "protocol/document_server.hpp"

#include <functional>
#include <mutex>

namespace azl {

// Serves the live beat clock and transport actions over the protocol as "clock" messages. The tempo itself is document state
// (setTempo, undoable like any edit), but tap, nudge, resync and scrub are performance actions on time: they act at the moment
// they arrive, cannot be meaningfully undone once the music has moved on, and stay out of the undo history. Each is
// broadcast to every client:
//
//   {"type": "clock", "op": "tap"}                  -> {"type": "clock", "op": "tap", "bpm": 128.0 | null}
//     from the second tap of a run, the tempo also changes as a document change with cause "tap"
//   {"type": "clock", "op": "nudge", "percent": 5}  -> broadcast as sent; held until a nudge of 0 percent, or until
//     the client that set it disconnects (a UI quit with the button held), which broadcasts a nudge of 0
//   {"type": "clock", "op": "resync"}               -> broadcast as sent; the downbeat (beat 0) is now
//     the Section playing then restarts on beat 0 and scheduled ones are dropped, as a setTimeline change (cause
//     "resync") before it
//   {"type": "clock", "op": "scrub", "layer": 1, "position": 0.5}
//                                                   -> broadcast as sent; the layer's clip moves to that point (0..1)
//     at the next frame and plays on from there
//   {"type": "clock", "op": "state"}                -> to the sender only: {"type": "clock", "op": "state", "time",
//     "beat", "bpm", "tempo", "nudge", "clips": [{"layer", "column", "frame", "beats", "visible", "next", "held"}],
//     "starting", "startedAt"} as of the last frame; a text or generator clip has "frame" and "beats" 0; "next" is
//     the column Autopilot plays next, 0 when it is off; "held" the bars left of a trigger's hold on Autopilot, 0
//     when not held; "starting" is true while the start-up hold keeps the outputs black; "startedAt" is the time of
//     the first frame drawn after it; "columns":
//     {"column", "next", "held"} is the column Autopilot's last column, next column and bars held, all 0 when it is off
//
// Actions are stamped with `now`, the engine's clock, when they arrive.
class ClockService {
public:
    using Now = std::function<double()>;

    static constexpr double kMaxNudge = 50.0;

    ClockService(DocumentServer& document, Engine& engine, Now now);
    ~ClockService();

private:
    void handle(const Message& message, const DocumentServer::Sink& reply);
    void tap();
    void rebaseTimeline(double now);
    void disconnected(ClientId client);

    DocumentServer& document_;
    Engine& engine_;
    Now now_;
    std::mutex tapMutex_;
    TapTempo taps_;
    std::mutex nudgeMutex_;
    // The client whose nudge is on, 0 when none is.
    ClientId nudger_ = 0;
};

} // namespace azl
