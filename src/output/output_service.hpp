#pragma once

#include "output/known_displays.hpp"
#include "output/outputs.hpp"
#include "output/screen_displays.hpp"
#include "protocol/document_server.hpp"

#include <functional>
#include <mutex>
#include <set>

namespace azl {

// Serves the machine's displays and outputs over the protocol as "outputs" messages. They are machine-local: which
// displays are attached and which output leads the render clock belong to the machine, not to the composition, which
// names a display by id on each screen as this machine places it.
//
//   {"type": "outputs", "op": "list"} -> {"type": "outputs", "op": "list",
//     "displays": [{"id", "name", "width", "height", "refreshHz"[, "mirrorOf"]}],
//     "outputs": [{"display", "screen", "attached", "main"}], "unmirror": true,
//     "roles": {"<screen role>": ["<display id>", ...]}} (this machine's display per role, screen_roles.hpp)
//   {"type": "outputs", "op": "main", "display": "<id>"} -> the same list, after making that display's output the
//     main output whenever it is attached
//   {"type": "outputs", "op": "guides", "show": true} -> the list, after showing or hiding the composition's guides on
//     every output
//   {"type": "outputs", "op": "target", "display": "<id>", "point": [x, y] | null} -> the list, after showing a corner
//     target through that point (in the output's 0..1 space) on that display's output, or hiding it
//   {"type": "outputs", "op": "identify"[, "display": "<id>"][, "seconds": 3]} -> the list, after every output (or that
//     display's) shows its screen's name and output large over the test card for that long; 0 seconds ends it
//   {"type": "outputs", "op": "unmirror", "on": false} -> the list, after turning off (or on) ending macOS mirroring
//     between displays that carry different screens; kept on this machine, on unless turned off
// The list also carries "guides" and, per output with one, its "target". A mirroring display ("mirrorOf": the display
// it shows) has no output of its own.
//
// Each is answered to its sender only. Every client that has sent one gets the list again whenever the displays
// change, and is told of keeping screens on their displays as {"type": "outputs", "op": "notice", "kind":
// "unmirrored" | "rebound", "text"} (a rebind also carries "screen", "from", "to"):
// - displays mirrored together that carry two or more screens are unmirrored, while the setting is on;
// - a screen whose display is gone takes a display newly attached that is the same projector under a new id.
class OutputService {
public:
    // Runs `work` on the thread that owns the displays.
    using Post = std::function<void(std::function<void()> work)>;

    OutputService(DocumentServer& document, Displays& displays, Outputs& outputs, KnownDisplays& known,
                  const ScreenDisplays& screens, Post post);
    ~OutputService();

    // Checks the displays against the screens now, as after a display change; on the thread that owns the displays.
    void keep();
    // The same, when the screens' displays differ from the last composition seen; on the thread that owns the displays.
    void compositionChanged(const Composition& composition);
    // Ends macOS mirroring between displays that carry different screens now, even while the setting is off; the
    // number of mirrored sets ended. On the thread that owns the displays.
    int unmirrorNow();

private:
    void handle(const Message& message, const DocumentServer::Sink& reply);
    Message list() const;
    std::string displayName(const std::string& id) const;
    void setUnmirror(bool on);
    void rebindReturned();
    int endMirroring(bool always);
    void notice(Message notice);
    void tell(const Message& message);

    DocumentServer& document_;
    Displays& displays_;
    Outputs& outputs_;
    KnownDisplays& known_;
    const ScreenDisplays& screens_;
    Post post_;
    bool keeping_ = false;
    std::set<std::string> seen_;
    std::vector<std::string> screenDisplays_;
    std::mutex mutex_;
    std::set<ClientId> listeners_;
};

} // namespace azl
