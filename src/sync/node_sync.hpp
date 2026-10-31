#pragma once

#include "engine/engine.hpp"
#include "media/media_library.hpp"
#include "platform/work_queue.hpp"
#include "protocol/document_server.hpp"
#include "sync/udp_clock.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

namespace azl {

// Node Sync between engines (ADR 0006): a node replicates the main engine's whole composition over the protocol,
// copies the media it lacks by hash, and follows the main engine's beat clock. Messages, all of type "node" except
// the beat:
//
//   node -> main  {"op": "attach"}  -> {"op": "attached", "name", "clock": UDP port of the clock responder}, then the
//                 beat timeline {"type": "beat", "time", "beat", "bpm", "nudge", "resyncs"} on the main engine's clock,
//                 whenever it changes and at least twice a second
//   node -> main  {"op": "fetch", "hash", "offset", "size"}
//                 -> {"op": "chunk", "hash", "offset", "total", "data": <binary>} or {"op": "chunk", "hash", "error"}
//   client -> any {"op": "status"} -> {"op": "status", "role": "main" | "node", ...}
//   client -> node {"op": "promote"}: the node becomes the main engine and takes edits, from the state it holds
//   client -> main {"op": "demote"}: the main engine becomes a node and follows the main engine it finds
//   client -> node {"op": "adopt"}: a node holding its show against an empty main engine takes the main's
//
// The attach answer also carries the main engine's Random Jump and Autopilot seed, its Autopilot pilots (so a node
// joining mid-show steps alike) and "fresh": the main engine runs the default composition it started with and nothing
// changed it. A node with clips of its own then keeps its show and its clock instead of adopting an empty one, until
// `adopt` or until the main engine loads a composition.
//
// A node keeps playing its last composition on its own beat clock whenever the main engine is away, and picks up
// again when it returns without a visible jump: its clock bends towards the main engine's instead of jumping.

struct Endpoint {
    std::string name;
    std::string host;
    uint16_t port = 0;
    // The main engine machine's pairing token, for a main on another machine.
    std::string token;
};

// The media hashes the composition's clips use, with a display name each.
std::map<std::string, std::string> mediaOf(const Composition& composition);
// Some layer has a clip.
bool hasShow(const Composition& composition);

// On the main engine: answers nodes and sends them its beat timeline.
class MainSync {
public:
    static constexpr uint64_t kMaxChunk = uint64_t(1) << 20;

    // `fresh`: the document is the default composition the engine started with.
    MainSync(DocumentServer& document, Engine& engine, MediaLibrary& library, std::string name, uint16_t clockPort,
             bool fresh = false);
    // Waits for the read in progress.
    ~MainSync();

    // "attach" and "fetch"; false for other ops.
    bool handle(const Message& message, const DocumentServer::Sink& reply);
    size_t nodes() const;

private:
    void run();
    void fetch(const Message& message, const DocumentServer::Sink& reply);

    DocumentServer& document_;
    Engine& engine_;
    MediaLibrary& library_;
    std::string name_;
    uint16_t clockPort_;
    bool fresh_;
    WorkQueue files_;
    mutable std::mutex mutex_;
    std::condition_variable stop_;
    bool stopped_ = false;
    std::set<ClientId> nodes_;
    std::thread thread_;
};

struct NodeStatus {
    bool connected = false;
    std::string main;
    bool synced = false;
    // Keeping its own show against a main engine that lost its.
    bool held = false;
    // Media still to copy, the file being copied, and what was copied since start.
    size_t missing = 0;
    std::string fetching;
    uint64_t bytes = 0;
    uint64_t files = 0;
    std::optional<double> offset;
    double roundTrip = 0.0;
};

void to_json(nlohmann::json& j, const NodeStatus& status);

// On a node: finds the main engine, mirrors its composition into this engine's document, copies missing media into
// this machine's library and makes the beat clock follow the main engine's.
class NodeSync {
public:
    static constexpr int kWindow = 4;
    // Silence from the main engine this long ends the connection (the main engine or the network is gone).
    static constexpr std::chrono::seconds kTimeout{3};

    using Locate = std::function<std::optional<Endpoint>()>;
    using ConnectClock = std::function<std::unique_ptr<ClockLink>(const Endpoint& main, uint16_t clockPort)>;

    // Without `fetch` missing media is only counted, never copied (a UI's preview follower).
    NodeSync(DocumentServer& document, Engine& engine, MediaLibrary& library, Clock now, Locate locate,
             ConnectClock clock, bool fetch = true);
    // Disconnects; the engine keeps playing what it has.
    ~NodeSync();

    NodeStatus status() const;
    // Takes the main engine's composition after all while held.
    void adopt();

    struct Session;

private:
    void run();
    void session(const Endpoint& main);
    void receive(const Message& message);
    void mirror(const Message& message);
    void adoptMirror();
    void attached(const Message& message);
    void release();
    void follow();
    void wantMissing();
    void fetchNext();
    void requestChunks();
    void chunk(const Message& message);
    void abortFetch();
    void verify(std::string hash, std::filesystem::path part, std::filesystem::path file);
    void send(const Message& message);

    DocumentServer& document_;
    Engine& engine_;
    MediaLibrary& library_;
    Clock now_;
    Locate locate_;
    ConnectClock connectClock_;
    bool fetch_;
    WorkQueue files_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stopped_ = false;
    std::unique_ptr<Session> session_;
    NodeStatus status_;
    std::set<std::string> have_;
    std::set<std::string> verifying_;
    std::thread thread_;
};

enum class Role { Main, Node };

// Which side of Node Sync this engine is on, and the "node" messages. Promoting a node makes it the main engine with
// the composition, media and beat clock it already has (ADR 0006); demoting a main engine makes it a node.
class SyncRole {
public:
    struct Options {
        Role role = Role::Main;
        std::string name;
        uint16_t clockPort = 0;
        // The main engine started on the default composition (no state, no file).
        bool fresh = false;
        NodeSync::Locate locate;
        NodeSync::ConnectClock clock;
        // Called after every change of role, e.g. to update the Bonjour TXT record.
        std::function<void(Role)> changed;
    };

    SyncRole(DocumentServer& document, Engine& engine, MediaLibrary& library, Clock now, Options options);
    ~SyncRole();

    Role role() const;
    void promote();
    void demote();
    void adopt();
    Message status() const;

private:
    void handle(const Message& message, const DocumentServer::Sink& reply);
    void become(Role role, bool fresh);

    DocumentServer& document_;
    Engine& engine_;
    MediaLibrary& library_;
    Clock now_;
    Options options_;
    mutable std::mutex mutex_;
    Role role_;
    std::unique_ptr<MainSync> main_;
    std::unique_ptr<NodeSync> node_;
};

} // namespace azl
