#pragma once

#include "ui/client/engine_client.hpp"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace azl {
class WebSocketClient;
}

namespace azl::ui {

// Keeps a WebSocket open to one engine and feeds the client, reconnecting every second while the engine is away (a
// supervised engine comes back on its own after a crash). The UI never blocks on it.
class Connection {
public:
    // `surfaces` false makes even a local engine send JPEG previews, as a remote one does. `token` is the engine
    // machine's pairing token, for an engine on another machine.
    Connection(EngineClient& client, std::string host, uint16_t port, bool surfaces = true, std::string token = {});
    ~Connection();

    const std::string& host() const { return host_; }
    uint16_t port() const { return port_; }
    // Why the last attempt failed (the name does not resolve, or the addresses it resolved to refuse); empty once
    // connected.
    std::string lastError() const;

private:
    void run();

    EngineClient& client_;
    const std::string host_;
    const uint16_t port_;
    const bool surfaces_;
    const std::string token_;
    mutable std::mutex mutex_;
    std::string lastError_;
    std::condition_variable wake_;
    bool stopping_ = false;
    bool closed_ = false;
    std::thread thread_;
};

} // namespace azl::ui
