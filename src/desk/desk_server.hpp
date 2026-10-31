#pragma once

#include "net/socket_compat.hpp"
#include "protocol/messages.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace azl {

class WebSocketClient;

// The page the desk serves.
extern const char* const kDeskPage;

// The venue desk's HTTP server: a protocol client of the engine on this machine (its "desk" status) and a small
// HTTP/1.1 server for phones on the venue LAN.
//
//   GET /       the page (plain HTML, CSS and JavaScript, nothing from elsewhere)
//   GET /state  the engine's desk status with "connected": true; 503 {"connected": false} while the engine is away
//
// It listens on the LAN addresses only (lanAddresses, checked again every few seconds) and refuses a request whose
// source or Host header deskRefusal turns down. With `loopbackOnly` (tests) it listens on 127.0.0.1 alone and lets
// loopback requests through. Each connection answers one request.
class DeskServer {
public:
    struct Options {
        uint16_t port = 7480;
        uint16_t enginePort = 7400;
        bool loopbackOnly = false;
        // For the harness: the addresses and name it takes as its own.
        std::function<std::vector<std::string>()> addresses;
        std::string name;
    };

    // Throws when it cannot listen on any address.
    explicit DeskServer(Options options);
    ~DeskServer();

    uint16_t port() const { return port_; }

    struct Response {
        int status = 200;
        std::string type;
        std::string body;
    };
    // Answers one request as seen from `source` (a numeric address) with its Host header.
    Response respond(const std::string& method, const std::string& path, const std::string& source,
                     const std::string& host);

private:
    std::optional<Message> engineStatus();
    void listen();
    void bindAll();
    void serve(Socket fd, std::string source);
    void closeAll();

    Options options_;
    uint16_t port_ = 0;
    std::map<std::string, Socket> sockets_;
    Wake wake_;
    std::atomic<bool> stop_{false};
    std::atomic<int> serving_{0};
    std::thread thread_;

    struct Link;
    std::mutex engineMutex_;
    std::unique_ptr<Link> link_;
};

} // namespace azl
