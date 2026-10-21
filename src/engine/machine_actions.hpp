#pragma once

#include "protocol/document_server.hpp"

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace azl {

// The machine actions that take the show down, served by the engine and by safe mode alike, and the venue's Show
// lock that refuses them:
//
//   {"op": "restart"} -> {"op": "restart"}, then the engine quits for the supervisor to start it again; from safe mode
//     it starts in normal mode again
//   {"op": "safeMode"} -> {"op": "safeMode"}, then the same into safe mode
//   {"op": "lock", "on": bool} -> {"op": "lock", "on"}
//
// While the lock is on, Restart engine, Safe mode and Update are refused for every client with an error naming the
// lock. The lock is kept in `lockFile` (none: in memory only), so it holds across engine restarts; a lock file that
// cannot be read counts as on. Every message has "type": "machine"; errors are {"type": "error"}.
class MachineActions {
public:
    // Asks the supervisor for a restart, into safe mode when `safeMode`. Null: the engine runs without a supervisor.
    using Restart = std::function<void(bool safeMode)>;

    MachineActions(std::filesystem::path lockFile, Restart restart);

    // Handles a machine message with one of the ops above; false for any other op.
    bool handle(const Message& message, const DocumentServer::Sink& reply);
    bool locked() const;
    // The error for a guarded action ("Restart engine", "Safe mode", "Update") while the lock is on.
    std::optional<std::string> refusal(const std::string& action) const;

private:
    void restart(ClientId client, bool safeMode, const char* op, const char* action, const DocumentServer::Sink& reply);
    void lock(const Message& message, const DocumentServer::Sink& reply);

    std::filesystem::path lockFile_;
    Restart restart_;
    mutable std::mutex mutex_;
    bool locked_ = false;
};

} // namespace azl
