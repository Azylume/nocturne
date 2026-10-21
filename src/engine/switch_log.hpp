#pragma once

#include "document/wall_clock.hpp"
#include "output/recorder.hpp"
#include "protocol/document_server.hpp"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace azl {

// Appends one JSON line per switch of the deck shown, whatever caused it, to the night's switch log
// `<recording folder>/Sets/<night date>/switches.jsonl` (nightDate):
//
//   {"utc", "local", "deck", "deckName", "dj", "slot", "cause", "recording": {"file", "mediaTime"} | null}
//
// `dj` and `slot` (from 1) name the lineup slot the deck belongs to (slotFor), null outside the night's lineup;
// `cause` is the change's cause; `recording` is where the switch lands in the recording running. A thread of its own
// writes and flushes each line, so the switch never waits for it; a line that cannot be written is logged once.
//
// Served as "switches" messages:
//   {"op": "list", "date"?, "watch"?} -> {"op": "list", "date", "switches": [line, ...]}, the night now by default
// and with "watch": true every line written from then on comes to that client as {"op": "added", "date", "switch": line}.
class SwitchLog {
public:
    using Clock = std::function<double()>;
    struct Options {
        // Engine time, for the recording, and Unix time.
        Clock now;
        Clock wall;
        UtcOffset zone = systemUtcOffset;
        // The recording folder.
        std::function<std::filesystem::path()> folder;
        Recorder* recorder = nullptr;
    };

    SwitchLog(DocumentServer& document, Options options);
    ~SwitchLog();

    // For the harness: returns once every switch so far is written or given up.
    void waitIdle();

    static std::filesystem::path file(const std::filesystem::path& folder, const std::string& date);
    // The lines of a switch log; one that does not parse is left out.
    static std::vector<Message> read(const std::filesystem::path& file);

private:
    struct Switch {
        double wall = 0.0;
        double time = 0.0;
        uint64_t deck = 0;
        std::string cause;
    };

    void changed(const Message& message);
    void run();
    void write(const Switch& s);
    Message recording(double time);
    void handle(const Message& message, const DocumentServer::Sink& reply);

    DocumentServer& document_;
    Options options_;
    uint64_t deck_ = 0;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::deque<Switch> queue_;
    bool busy_ = false;
    bool stop_ = false;
    bool failing_ = false;
    std::thread thread_;
    ClientId client_ = 0;
    std::mutex watchersMutex_;
    std::set<ClientId> watchers_;
};

} // namespace azl
