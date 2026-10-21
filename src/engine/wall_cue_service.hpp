#pragma once

#include "document/wall_clock.hpp"
#include "engine/engine.hpp"
#include "protocol/document_server.hpp"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace azl {

// Fires the Cues set to a time of day (Cue::at) on the venue machine's own clock, so a lineup runs with no Director
// connected. When a Cue's time comes it shows the Cue's deck at once and triggers its column on the next bar, as live
// changes (cause "cue") outside the undo history. A Cue whose deck already shows does nothing, so a hand that switched
// early is never undone; it is logged and broadcast as {"type": "cue", "op": "skipped", "cue", "name", "deck",
// "reason"}. The Cues are document state, so they survive a restart; a time that passed in the kCatchUp seconds before
// the service started still fires, unless the Cue's deck already shows.
class WallCueService {
public:
    // Engine time, for the beat clock.
    using Now = std::function<double()>;
    // Unix time.
    using Wall = std::function<double()>;

    // With a period a thread polls every `period` seconds; without, the owner calls poll().
    WallCueService(DocumentServer& document, Engine& engine, Now now, Wall wall, UtcOffset zone,
                   std::optional<double> period = std::nullopt);
    ~WallCueService();

    // Fires every Cue whose time came since the last poll.
    void poll();
    // The next poll starts over, as after a restart.
    void restart();

    static constexpr double kCatchUp = 300.0;

private:
    void fire(const Composition& composition, const Cue& cue);
    void skip(const Cue& cue);
    void refresh();

    DocumentServer& document_;
    Engine& engine_;
    Now now_;
    Wall wall_;
    UtcOffset zone_;
    std::mutex mutex_;
    std::optional<double> last_;
    std::optional<uint64_t> change_;
    std::shared_ptr<const Composition> composition_;
    bool stop_ = false;
    std::condition_variable wake_;
    std::thread thread_;
};

} // namespace azl
