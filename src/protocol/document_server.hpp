#pragma once

#include "document/composition.hpp"
#include "document/composition_file.hpp"
#include "protocol/messages.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace azl {

using ClientId = uint64_t;

// Both set the same value, so the later one alone does what both do.
bool sameEdit(const Edit& a, const Edit& b);

// Owns the composition document on the main engine. Clients talk to it only through protocol messages, whether they
// are in-process (the harness) or behind a transport. Every applied command becomes one numbered change, broadcast in
// order to every client that has said hello. Thread-safe.
//
// A command may carry "gesture": a nonzero number the client picks per mouse drag or compound action. Consecutive
// commands of one client with the same gesture make one undo step; repeated edits of the same value within it keep
// only the first inverse and the last edit. setScreenDisplay is no undo step: where a screen shows belongs to the
// machine, not the show. Every change and snapshot carries "history": {"undo", "redo"} (the steps on each stack) and
// "undoOp", "redoOp" (the op of the step undo or redo would revert or repeat, empty when none). A snapshot also
// carries "steps": {"undo": [op, ...], "redo": [op, ...]}, every step's op, oldest first, and "file": {"path",
// "unsaved"} once the document belongs to a show file.
//
// A Prepare change (prepare.hpp) arrives as {"type": "prepare", "op": "send", "at": "now" | "switch", "edits": [...]}.
// "now" applies the edits that still fit as one undo step and answers {"op": "sent", "applied", "skipped"}; "switch"
// adds a Cue holding them (one undo step) and answers {"op": "scheduled", "cue"}. At the next deck switch that Cue
// applies the same way, gone in the same step, and every client gets the "sent" message with its "cue".
//
// A change whose edit removed a layer or an effect that macro targets, modulators or Cue steps pointed at carries
// "notice": a sentence naming each of them, gone in the same step (applyEdit), for a UI to show.
class DocumentServer {
public:
    // Called with every new document state, on whichever thread applied the edit.
    using Publish = std::function<void(std::shared_ptr<const Composition>)>;
    // Must not block or call back into the server: it runs under the server's lock so changes stay ordered.
    using Sink = std::function<void(const Message&)>;
    // Called with file() after every change and every change of the file; the same rules as a Sink.
    using FileWatch = std::function<void(const std::optional<ShowFile>&)>;
    // Handles a machine-local message type that is not part of the document (e.g. the Media Library). It runs outside
    // the server's lock and may call `reply` later, from any thread, while the server lives. The message carries the
    // sender's id as "client" and whether it connected from this machine as "loopback".
    using Handler = std::function<void(const Message& message, Sink reply)>;

    DocumentServer(Composition initial, Publish publish);

    // The client gets nothing until it sends hello, which is answered with a snapshot; a hello with "watch": false
    // gets no snapshot and no broadcasts, only replies (a venue health probe). `loopback`: it connected from this
    // machine (in-process, or a transport from a loopback address).
    ClientId connect(Sink sink, bool loopback = true);
    void disconnect(ClientId client);
    void receive(ClientId client, const Message& message);
    // A null handler unregisters the type.
    void handle(const std::string& type, Handler handler);
    // Sends to one client if it is still connected; false if it is not.
    bool send(ClientId client, const Message& message);
    // Sends to every client that has said hello.
    void broadcast(const Message& message);
    // Applies an edit made by a live performance action (e.g. tap tempo, a trigger): a numbered change from `client`
    // (0: the engine) with `cause`, but no undo step. Throws ProtocolError when it does not apply.
    void perform(Edit edit, const char* cause, ClientId client = 0);
    // The document as it is now.
    Composition document();
    // The number of the last change.
    uint64_t change();
    // The show file the document was last loaded from or saved to; unsaved while an undo step taken or undone since
    // stands between the two.
    std::optional<ShowFile> file();
    // The document as it is now is what `path` holds.
    void markSaved(const std::string& path);
    // Loads `composition` as `client`'s undoable edit and marks it saved to `path`, so no client sees one without the
    // other.
    void loadFile(ClientId client, Composition composition, const std::string& path);
    // Takes over file() from before a restart: unsaved stays unsaved until the next save.
    void resumeFile(const ShowFile& file);
    void watchFile(FileWatch watch);
    // Called with each client that disconnects, outside the server's lock; null stops it.
    void watchDisconnects(std::function<void(ClientId)> watch);
    // The folders a client on another machine may name in a machine-local message (remote_files.hpp), asked outside
    // the server's lock for each such message; a path outside them is refused before any handler sees it. Unset, it
    // may name none.
    void allowRemoteFiles(std::function<std::vector<std::filesystem::path>()> roots);
    // While set, clients cannot edit: commands and every machine-local type outside `open` are refused with `reason`
    // (a node follows its main engine). perform still applies.
    void setReadOnly(std::optional<std::string> reason, std::set<std::string> open = {});

private:
    struct Client {
        Sink sink;
        bool helloed = false;
        // Gets the snapshot and every broadcast; a hello with "watch": false only asks machine-local questions.
        bool watching = false;
        bool loopback = true;
    };
    struct Step {
        std::vector<Edit> redo;
        std::vector<Edit> undo;
        ClientId client = 0;
        uint64_t gesture = 0;
        // Changes whenever the step does, so a saved step is told from an edited one.
        uint64_t serial = 0;
    };

    Handler route(ClientId id, const Message& message, bool& loopback);
    void sendSnapshot(Client& client, ClientId id);
    void command(ClientId client, const Message& message);
    void prepare(ClientId client, const Message& message);
    Message applyPrepared(ClientId client, const Message& edits, std::optional<uint64_t> cue, const char* cause);
    void followDeck();
    Edit apply(ClientId client, Edit& edit, const char* cause);
    // `dropped` names what the edit took with it (applyEdit), carried as the change's "notice".
    void announce(ClientId client, const Edit& edit, const char* cause, const std::vector<std::string>& dropped = {});
    void history(ClientId client, std::vector<Step>& from, std::vector<Step>& to, bool undo);
    void record(ClientId client, uint64_t gesture, Edit edit, Edit inverse);
    Message historyJson() const;
    Message stepsJson() const;
    uint64_t lastStep() const;
    std::optional<ShowFile> fileLocked() const;
    void fileChanged();
    void markSavedLocked(const std::string& path);

    std::mutex mutex_;
    Composition document_;
    Publish publish_;
    std::map<ClientId, Client> clients_;
    std::map<std::string, Handler> handlers_;
    std::optional<std::string> readOnly_;
    std::set<std::string> open_;
    ClientId nextClient_ = 1;
    uint64_t change_ = 0;
    uint64_t deck_ = 0;
    std::vector<Step> undo_;
    std::vector<Step> redo_;
    uint64_t serial_ = 0;
    std::optional<std::string> file_;
    uint64_t savedStep_ = 0;
    FileWatch watchFile_;
    std::function<void(ClientId)> watchDisconnects_;
    std::function<std::vector<std::filesystem::path>()> remoteRoots_;
};

} // namespace azl
