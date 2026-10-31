#include "desk/desk_server.hpp"

#include "net/lan.hpp"
#include "net/socket_compat.hpp"
#include "net/websocket.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <stdexcept>

namespace azl {

namespace {

constexpr int kMaxServing = 32;
constexpr size_t kMaxRequest = 8192;
constexpr int kRescanMs = 5000;
constexpr auto kEngineWait = std::chrono::milliseconds(1500);

const char* reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    default: return "Service Unavailable";
    }
}

std::string lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

std::string trimmed(const std::string& text)
{
    const size_t first = text.find_first_not_of(" \t");
    const size_t last = text.find_last_not_of(" \t\r");
    return first == std::string::npos ? "" : text.substr(first, last - first + 1);
}

} // namespace

struct DeskServer::Link {
    std::mutex mutex;
    std::condition_variable arrived;
    std::deque<Message> inbox;
    bool closed = false;
    std::unique_ptr<WebSocketClient> client;
};

DeskServer::DeskServer(Options options) : options_(std::move(options)), port_(options_.port)
{
    if (!options_.addresses)
        options_.addresses = lanAddresses;
    if (options_.name.empty())
        options_.name = bonjourName();
    startSockets();
    bindAll();
    if (sockets_.empty() && (options_.loopbackOnly || port_ == 0)) {
        closeAll();
        throw std::runtime_error("cannot listen on port " + std::to_string(options_.port));
    }
    thread_ = std::thread([this] { listen(); });
}

DeskServer::~DeskServer()
{
    stop_ = true;
    wake_.signal();
    thread_.join();
    while (serving_ > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    closeAll();
}

void DeskServer::closeAll()
{
    for (auto& [address, fd] : sockets_)
        closeSocket(fd);
    sockets_.clear();
}

// Listens on every address it should and no other; the port stays the one first bound.
void DeskServer::bindAll()
{
    const std::vector<std::string> wanted =
        options_.loopbackOnly ? std::vector<std::string>{"127.0.0.1"} : options_.addresses();
    for (auto it = sockets_.begin(); it != sockets_.end();) {
        if (std::find(wanted.begin(), wanted.end(), it->first) != wanted.end()) {
            ++it;
            continue;
        }
        std::printf("desk: no longer listening on %s\n", it->first.c_str());
        closeSocket(it->second);
        it = sockets_.erase(it);
    }
    for (const std::string& address : wanted) {
        if (sockets_.contains(address))
            continue;
        sockaddr_in at = ipv4Endpoint({}, port_);
        if (inet_pton(AF_INET, address.c_str(), &at.sin_addr) != 1)
            continue;
        const Socket fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd == kNoSocket)
            continue;
        const int on = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof on);
        noInherit(fd);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&at), sizeof at) != 0 || ::listen(fd, 16) != 0) {
            std::printf("desk: cannot listen on %s:%u: %s\n", address.c_str(), unsigned(port_),
                        socketErrorText(socketError()).c_str());
            closeSocket(fd);
            continue;
        }
        if (port_ == 0) {
            socklen_t size = sizeof at;
            ::getsockname(fd, reinterpret_cast<sockaddr*>(&at), &size);
            port_ = ntohs(at.sin_port);
        }
        sockets_[address] = fd;
        std::printf("desk: listening on %s:%u\n", address.c_str(), unsigned(port_));
        std::fflush(stdout);
    }
}

void DeskServer::listen()
{
    auto rescan = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRescanMs);
    while (!stop_) {
        std::vector<pollfd> fds{{wake_.socket(), POLLIN, 0}};
        for (const auto& [address, fd] : sockets_)
            fds.push_back({fd, POLLIN, 0});
        pollSockets(fds.data(), fds.size(), kRescanMs);
        if (stop_)
            break;
        for (size_t i = 1; i < fds.size(); ++i) {
            if (!(fds[i].revents & POLLIN))
                continue;
            sockaddr_storage from{};
            socklen_t size = sizeof from;
            const Socket fd = ::accept(fds[i].fd, reinterpret_cast<sockaddr*>(&from), &size);
            if (fd == kNoSocket)
                continue;
            char text[INET6_ADDRSTRLEN] = {};
            if (from.ss_family == AF_INET)
                inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in&>(from).sin_addr, text, sizeof text);
            else if (from.ss_family == AF_INET6)
                inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6&>(from).sin6_addr, text, sizeof text);
            if (serving_ >= kMaxServing) {
                closeSocket(fd);
                continue;
            }
            ++serving_;
            std::thread([this, fd, source = std::string(text)] {
                serve(fd, source);
                --serving_;
            }).detach();
        }
        if (!options_.loopbackOnly && std::chrono::steady_clock::now() >= rescan) {
            bindAll();
            rescan = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRescanMs);
        }
    }
}

void DeskServer::serve(Socket fd, std::string source)
{
    setTimeouts(fd, 5.0);
    noSigPipe(fd);
    std::string request;
    char buffer[2048];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < kMaxRequest) {
        const long n = receiveSome(fd, buffer, sizeof buffer);
        if (n <= 0)
            break;
        request.append(buffer, size_t(n));
    }
    const size_t end = request.find("\r\n\r\n");
    if (end == std::string::npos) {
        closeSocket(fd);
        return;
    }
    std::string method, path, host;
    size_t at = 0;
    for (size_t line = 0; at < end; ++line) {
        const size_t next = std::min(request.find("\r\n", at), end);
        const std::string text = request.substr(at, next - at);
        at = next + 2;
        if (line == 0) {
            const size_t a = text.find(' ');
            const size_t b = text.find(' ', a + 1);
            method = text.substr(0, a);
            path = a == std::string::npos ? "" : text.substr(a + 1, b - a - 1);
            continue;
        }
        const size_t colon = text.find(':');
        if (colon != std::string::npos && lower(text.substr(0, colon)) == "host")
            host = trimmed(text.substr(colon + 1));
    }
    const Response r = respond(method, path, source, host);
    std::string head = "HTTP/1.1 " + std::to_string(r.status) + " " + reason(r.status) + "\r\n";
    head += "Content-Type: " + r.type + "\r\nContent-Length: " + std::to_string(r.body.size()) + "\r\n";
    head += "Cache-Control: no-store\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n";
    head += "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
            "connect-src 'self'; img-src 'self'\r\n\r\n";
    const std::string reply = head + r.body;
    sendAll(fd, reply.data(), reply.size());
    closeSocket(fd);
}

DeskServer::Response DeskServer::respond(const std::string& method, const std::string& path, const std::string& source,
                                         const std::string& host)
{
    const std::vector<std::string> own = options_.loopbackOnly ? std::vector<std::string>{} : options_.addresses();
    if (const std::optional<std::string> refused = deskRefusal(source, host, own, options_.name, options_.loopbackOnly))
        return {403, "text/plain; charset=utf-8", *refused + "\n"};
    if (method != "GET")
        return {405, "text/plain; charset=utf-8", "only GET\n"};
    const std::string route = path.substr(0, path.find('?'));
    if (route == "/" || route == "/index.html")
        return {200, "text/html; charset=utf-8", kDeskPage};
    if (route != "/state")
        return {404, "text/plain; charset=utf-8", "not found\n"};
    std::optional<Message> status = engineStatus();
    if (!status)
        return {503, "application/json", Message{{"connected", false}}.dump()};
    (*status)["connected"] = true;
    status->erase("type");
    status->erase("op");
    return {200, "application/json", status->dump(-1, ' ', false, Message::error_handler_t::replace)};
}

std::optional<Message> DeskServer::engineStatus()
{
    std::lock_guard lock(engineMutex_);
    if (link_) {
        bool closed = false;
        {
            std::lock_guard inbox(link_->mutex);
            closed = link_->closed;
        }
        if (closed)
            link_.reset();
    }
    if (!link_) {
        auto link = std::make_unique<Link>();
        Link* raw = link.get();
        try {
            link->client = std::make_unique<WebSocketClient>(
                "127.0.0.1", options_.enginePort, Encoding::MessagePack,
                [raw](const Message& m) {
                    std::lock_guard l(raw->mutex);
                    raw->inbox.push_back(m);
                    raw->arrived.notify_all();
                },
                [raw] {
                    std::lock_guard l(raw->mutex);
                    raw->closed = true;
                    raw->arrived.notify_all();
                });
        } catch (const std::exception&) {
            return std::nullopt;
        }
        link->client->send({{"type", "hello"}, {"watch", false}});
        link_ = std::move(link);
    }
    {
        std::lock_guard l(link_->mutex);
        link_->inbox.clear();
    }
    link_->client->send({{"type", "desk"}, {"op", "status"}});
    std::unique_lock l(link_->mutex);
    const auto until = std::chrono::steady_clock::now() + kEngineWait;
    for (;;) {
        if (!link_->arrived.wait_until(l, until, [&] { return link_->closed || !link_->inbox.empty(); }))
            return std::nullopt;
        if (link_->closed)
            return std::nullopt;
        Message m = std::move(link_->inbox.front());
        link_->inbox.pop_front();
        if (m.value("type", "") == "desk" && m.value("op", "") == "status")
            return m;
        if (m.value("type", "") == "error")
            return std::nullopt;
    }
}

} // namespace azl
