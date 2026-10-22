#pragma once

#include "protocol/document_server.hpp"
#include "protocol/messages.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace azl {

inline constexpr uint16_t kDefaultPort = 7400;

// The numeric addresses, IPv4 and IPv6, a host name or address resolves to. Throws, naming the host, when it does not.
std::vector<std::string> resolveHost(const std::string& host);
// Every address `host` resolves to is this machine's: loopback or one of its interfaces'. False when it does not
// resolve.
bool onThisMachine(const std::string& host);

// Who the server lets in. A client on another machine must present `token` in its first message, a hello; anything
// else is answered with an error and the connection closed. Without a token no client from another machine gets in.
// A handshake with an Origin header, which every web page's has, is refused whatever its address.
struct Pairing {
    std::string token;
    // Clients from loopback addresses need no token; tests turn this off to stand in for another machine.
    bool trustLoopback = true;
    // A client that has not paired by then is closed; more unpaired clients than this at once are turned away.
    std::chrono::milliseconds helloTimeout{10000};
    size_t maxUnpaired = 32;
};

// Serves a document over WebSocket, one protocol message per WebSocket message. Each client is answered in the
// encoding of the last message it sent: binary means MessagePack, text means JSON. Handlers see a paired client from
// another machine as not "loopback".
class WebSocketServer {
public:
    // Listens on every interface, so LAN clients can connect. Port 0 picks a free port. Throws when it cannot listen.
    WebSocketServer(DocumentServer& document, uint16_t port, Pairing pairing = {});
    ~WebSocketServer();

    uint16_t port() const { return port_; }

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
    uint16_t port_ = 0;
};

class WebSocketClient {
public:
    using OnMessage = std::function<void(const Message&)>;
    using OnClose = std::function<void()>;

    // `host` is a name (localhost, a Bonjour or Tailscale name) or an address. Returns once the WebSocket is open;
    // throws when the name does not resolve or it cannot connect, naming the addresses tried. Callbacks run on an
    // internal serial queue.
    WebSocketClient(const std::string& host, uint16_t port, Encoding encoding, OnMessage onMessage,
                    OnClose onClose = {});
    ~WebSocketClient();

    void send(const Message& message);

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

} // namespace azl
