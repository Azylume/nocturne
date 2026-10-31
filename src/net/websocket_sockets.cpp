// The protocol's WebSocket server and client on plain sockets (Winsock on Windows, POSIX elsewhere), for platforms
// without Network.framework. Same limits, pairing and Origin rules as websocket.mm (both use websocket_rules.hpp).

#include "net/websocket.hpp"

#include "net/lan.hpp"
#include "net/socket_compat.hpp"
#include "net/websocket_frames.hpp"
#include "net/websocket_rules.hpp"
#include "output/return_video.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <thread>

namespace azl {

namespace {

using namespace ws;
using Steady = std::chrono::steady_clock;

constexpr auto kSetupTimeout = std::chrono::seconds(5);
// After its close frame is out a connection is read and dropped until the other side closes, so an error sent just
// before is not lost to a reset; this long at most.
constexpr auto kLinger = std::chrono::seconds(2);
// The client takes what its engine sends: snapshots and previews, larger than anything a client sends.
constexpr size_t kMaxReceivedByClient = size_t(256) << 20;
constexpr int kMaxPollMs = 1000;

std::array<uint8_t, 4> randomMask()
{
    thread_local std::mt19937 random{std::random_device{}()};
    const uint32_t bits = random();
    return {uint8_t(bits), uint8_t(bits >> 8), uint8_t(bits >> 16), uint8_t(bits >> 24)};
}

std::string randomKey()
{
    std::random_device random;
    uint8_t bytes[16];
    for (uint8_t& b : bytes)
        b = uint8_t(random());
    return base64(bytes, sizeof bytes);
}

// The numeric address, IPv4-mapped IPv6 as plain IPv4.
std::string numericAddress(const sockaddr_storage& at)
{
    char text[INET6_ADDRSTRLEN] = {};
    if (at.ss_family == AF_INET) {
        inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in&>(at).sin_addr, text, sizeof text);
        return text;
    }
    if (at.ss_family != AF_INET6)
        return {};
    const in6_addr& address = reinterpret_cast<const sockaddr_in6&>(at).sin6_addr;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&address);
    static constexpr uint8_t kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (std::memcmp(b, kMapped, sizeof kMapped) == 0)
        inet_ntop(AF_INET, b + 12, text, sizeof text);
    else
        inet_ntop(AF_INET6, &address, text, sizeof text);
    return text;
}

int millisecondsUntil(Steady::time_point when)
{
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(when - Steady::now()).count();
    return int(std::clamp<int64_t>(left, 0, kMaxPollMs));
}

// Messages go out as soon as they are queued, not held back to fill a segment.
void prepareConnection(Socket socket)
{
    noInherit(socket);
    noSigPipe(socket);
    const int on = 1;
    ::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
}

void shutdownSending(Socket socket)
{
#ifdef _WIN32
    ::shutdown(socket, SD_SEND);
#else
    ::shutdown(socket, SHUT_WR);
#endif
}

// One TCP connection carrying a WebSocket, on either side. Its socket is read and written on its owner's I/O thread
// only; any thread may queue a message on it.
struct Peer {
    enum class State { Handshake, Open, Closing };

    Peer(Socket s, Encoding e, bool client, std::shared_ptr<Wake> w, size_t limit)
        : socket(s), encoding(e), isClient(client), wake(std::move(w)), reader(!client, limit)
    {
    }

    const Socket socket;
    std::atomic<Encoding> encoding;
    std::atomic<int> pending{0};
    return_video::Gate video;
    const bool isClient;
    const std::shared_ptr<Wake> wake;

    struct Outgoing {
        std::vector<uint8_t> bytes;
        // A protocol message, counted in `pending` until it is out.
        bool counted = false;
        // The connection closes once this is out.
        bool last = false;
    };
    std::mutex mutex;
    std::deque<Outgoing> queue;
    bool sealed = false;

    // On the I/O thread only.
    State state = State::Handshake;
    std::string head;
    MessageReader reader;
    std::optional<Outgoing> writing;
    size_t written = 0;
    bool lastOut = false;
    Steady::time_point lingerUntil{};
    // Server side: as in websocket.mm.
    ClientId id = 0;
    bool paired = true;
    bool refused = false;
    std::string address;
    std::optional<Steady::time_point> helloBy;

    std::optional<std::array<uint8_t, 4>> mask() const { return isClient ? std::optional(randomMask()) : std::nullopt; }

    // Queues `bytes`; nothing after the last one goes out. The handshake's answer goes ahead of what was queued
    // while the handshake was under way.
    void queueBytes(std::vector<uint8_t> bytes, bool counted, bool last, bool first = false)
    {
        {
            std::lock_guard lock(mutex);
            if (sealed)
                return;
            sealed = last;
            if (counted)
                ++pending;
            queue.insert(first ? queue.begin() : queue.end(), {std::move(bytes), counted, last});
        }
        wake->signal();
    }

    // A connection closed before its handshake was answered gets no messages, only the refusal when it had one.
    void endHandshake()
    {
        std::lock_guard lock(mutex);
        if (sealed)
            return;
        sealed = true;
        queue.clear();
        queue.push_back({{}, false, true});
    }

    bool wantsToWrite()
    {
        std::lock_guard lock(mutex);
        return state != State::Handshake && !lastOut && (writing || !queue.empty());
    }

    // Writes what the socket takes now; nothing before the handshake is answered. False when the connection failed.
    bool flush()
    {
        if (state == State::Handshake || lastOut)
            return true;
        for (;;) {
            if (!writing) {
                std::lock_guard lock(mutex);
                if (queue.empty())
                    return true;
                writing = std::move(queue.front());
                queue.pop_front();
                written = 0;
            }
            const std::vector<uint8_t>& bytes = writing->bytes;
            while (written < bytes.size()) {
                const long n = sendSome(socket, bytes.data() + written, bytes.size() - written);
                if (n < 0)
                    return wouldBlock(socketError());
                written += size_t(n);
            }
            if (writing->counted)
                --pending;
            const bool last = writing->last;
            writing.reset();
            if (last) {
                lastOut = true;
                return true;
            }
        }
    }
};

// By value, as in websocket.mm: a queued message keeps its peer. `close` closes the connection once it is out.
void send(std::shared_ptr<Peer> peer, const Message& message, bool close = false)
{
    const Encoding encoding = peer->encoding;
    const std::vector<uint8_t> bytes = encode(message, encoding);
    const Opcode opcode = encoding == Encoding::Json ? Opcode::Text : Opcode::Binary;
    peer->queueBytes(frame(opcode, bytes.data(), bytes.size(), peer->mask()), true, false);
    if (close)
        peer->queueBytes(closeFrame(kClosePolicy, peer->mask()), false, true);
}

void closeWith(Peer& peer, uint16_t code)
{
    peer.queueBytes(closeFrame(code, peer.mask()), false, true);
}

// Hooks one side gives the I/O loop. All run on its thread.
struct Handlers {
    // A data message arrived; `text` tells JSON from MessagePack.
    std::function<void(const std::shared_ptr<Peer>&, std::vector<uint8_t>&&, bool text)> onMessage;
    // The WebSocket closed, or the connection failed. Called once per peer.
    std::function<void(const std::shared_ptr<Peer>&)> onClosed;
    // A message was too big for its peer.
    std::function<void(const std::shared_ptr<Peer>&)> onTooBig;
};

// Hands out the events the peer's reader has whole. False once the WebSocket is done with: closed, refused or broken.
bool dispatch(const std::shared_ptr<Peer>& peer, const Handlers& handlers, const std::atomic<bool>& stopped)
{
    while (peer->state == Peer::State::Open && !stopped) {
        std::optional<Event> event = peer->reader.next();
        if (!event)
            return true;
        switch (event->kind) {
        case Event::Kind::Text:
        case Event::Kind::Binary:
            handlers.onMessage(peer, std::move(event->payload), event->kind == Event::Kind::Text);
            break;
        case Event::Kind::Ping:
            peer->queueBytes(frame(Opcode::Pong, event->payload.data(), event->payload.size(), peer->mask()), false,
                             false);
            break;
        case Event::Kind::Pong:
            break;
        case Event::Kind::Close:
            closeWith(*peer, kCloseNormal);
            return false;
        case Event::Kind::TooBig:
            handlers.onTooBig(peer);
            return false;
        case Event::Kind::Invalid:
            closeWith(*peer, kCloseProtocolError);
            return false;
        }
    }
    return true;
}

// A server's side of the opening handshake, once the head is in. False when it refused the client.
bool answer(Peer& peer)
{
    const size_t end = headEnd(peer.head);
    const std::optional<Head> head = parseHead(peer.head);
    const HandshakeAnswer reply =
        head ? answerHandshake(*head) : HandshakeAnswer{false, "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n"};
    peer.queueBytes({reply.response.begin(), reply.response.end()}, false, !reply.upgraded, true);
    if (!reply.upgraded)
        return false;
    peer.state = Peer::State::Open;
    peer.reader.feed(reinterpret_cast<const uint8_t*>(peer.head.data()) + end, peer.head.size() - end);
    peer.head.clear();
    return true;
}

// Reads whatever arrived and acts on it. False once the peer is done with, as for dispatch, or the connection ended.
bool readFrom(const std::shared_ptr<Peer>& peer, const Handlers& handlers, const std::atomic<bool>& stopped)
{
    uint8_t buffer[64 << 10];
    const long n = receiveSome(peer->socket, buffer, sizeof buffer);
    if (n < 0 && wouldBlock(socketError()))
        return true;
    if (n <= 0)
        return false;
    if (peer->state == Peer::State::Closing)
        return true;
    if (peer->state == Peer::State::Open) {
        peer->reader.feed(buffer, size_t(n));
        return dispatch(peer, handlers, stopped);
    }
    peer->head.append(reinterpret_cast<const char*>(buffer), size_t(n));
    if (headEnd(peer->head) != std::string::npos)
        return answer(*peer) && dispatch(peer, handlers, stopped);
    if (peer->head.size() <= kMaxHeadBytes)
        return true;
    const std::string refusal = "HTTP/1.1 431 Request Header Fields Too Large\r\nConnection: close\r\n\r\n";
    peer->queueBytes({refusal.begin(), refusal.end()}, false, true, true);
    return false;
}

// Polls a listener (servers) and its peers on its own thread: reads, writes, closes, and lingers over closed ones. The
// thread holds the loop, so a side destroyed from one of its own handlers leaves it to wind down by itself.
class Loop : public std::enable_shared_from_this<Loop> {
public:
    using OnAccept = std::function<void(Socket, const std::string& address)>;
    using OnTick = std::function<void(Steady::time_point now)>;

    const std::shared_ptr<Wake>& wake() const { return wake_; }

    void start(Socket listener, Handlers handlers, OnAccept onAccept = {}, OnTick onTick = {})
    {
        listener_ = listener;
        handlers_ = std::move(handlers);
        onAccept_ = std::move(onAccept);
        onTick_ = std::move(onTick);
        thread_ = std::thread([self = shared_from_this()] { self->run(); });
    }

    // No handler starts after it returns. Called from a handler, the thread closes the sockets on its way out.
    void stop()
    {
        stopped_ = true;
        wake_->signal();
        if (thread_.get_id() == std::this_thread::get_id())
            thread_.detach();
        else if (thread_.joinable())
            thread_.join();
        else
            closeAll();
    }

    // On the loop's thread, or before it starts.
    void add(const std::shared_ptr<Peer>& peer) { peers_.push_back(peer); }
    // Closes the WebSocket from this side: what is queued and a close frame go out first. On the loop's thread.
    void close(const std::shared_ptr<Peer>& peer) { closing(peer); }

private:
    void run()
    {
        for (const auto& peer : peers_)
            if (!dispatch(peer, handlers_, stopped_))
                closing(peer);
        std::vector<pollfd> fds;
        while (!stopped_) {
            fds.clear();
            fds.push_back({wake_->socket(), POLLIN, 0});
            if (listener_ != kNoSocket)
                fds.push_back({listener_, POLLIN, 0});
            const size_t first = fds.size();
            Steady::time_point next = Steady::now() + std::chrono::milliseconds(kMaxPollMs);
            for (const auto& peer : peers_) {
                fds.push_back({peer->socket, short(POLLIN | (peer->wantsToWrite() ? POLLOUT : 0)), 0});
                if (peer->helloBy)
                    next = std::min(next, *peer->helloBy);
                if (lingering(*peer))
                    next = std::min(next, peer->lingerUntil);
            }
            pollSockets(fds.data(), fds.size(), millisecondsUntil(next));
            if (fds[0].revents & POLLIN)
                wake_->drain();
            if (!stopped_ && listener_ != kNoSocket && (fds[1].revents & POLLIN))
                acceptOne();
            serve(fds, first);
            if (!stopped_ && onTick_)
                onTick_(Steady::now());
            reap();
        }
        closeAll();
    }

    void acceptOne()
    {
        sockaddr_storage from{};
        socklen_t size = sizeof from;
        const Socket socket = ::accept(listener_, reinterpret_cast<sockaddr*>(&from), &size);
        if (socket == kNoSocket)
            return;
        setNonBlocking(socket);
        prepareConnection(socket);
        onAccept_(socket, numericAddress(from));
    }

    // The peers polled in `fds` from `first` on; one accepted meanwhile waits for the next round.
    void serve(const std::vector<pollfd>& fds, size_t first)
    {
        const std::vector<std::shared_ptr<Peer>> polled(peers_.begin(), peers_.begin() + (fds.size() - first));
        for (size_t i = 0; i < polled.size() && !stopped_; ++i) {
            const auto& peer = polled[i];
            const bool readable = fds[first + i].revents & (POLLIN | POLLHUP | POLLERR);
            const bool alive = !readable || readFrom(peer, handlers_, stopped_);
            if (!alive && peer->state == Peer::State::Closing) {
                finish(peer);
                continue;
            }
            if (!alive)
                closing(peer);
            if (!peer->flush())
                finish(peer);
            else if (peer->lastOut)
                closing(peer);
        }
    }

    static bool lingering(const Peer& peer) { return peer.lingerUntil != Steady::time_point{}; }

    // The WebSocket ends here: the handler hears of it once, and once the last bytes are out the socket lingers.
    void closing(const std::shared_ptr<Peer>& peer)
    {
        if (peer->state != Peer::State::Closing) {
            if (peer->state == Peer::State::Open)
                closeWith(*peer, kCloseNormal);
            else
                peer->endHandshake();
            peer->state = Peer::State::Closing;
            handlers_.onClosed(peer);
        }
        if (peer->lastOut && !lingering(*peer)) {
            shutdownSending(peer->socket);
            peer->lingerUntil = Steady::now() + kLinger;
        }
    }

    // The connection is over: ended by the other side, or broken.
    void finish(const std::shared_ptr<Peer>& peer)
    {
        if (peer->state != Peer::State::Closing) {
            peer->state = Peer::State::Closing;
            handlers_.onClosed(peer);
        }
        peer->lastOut = true;
        peer->lingerUntil = Steady::now();
    }

    void reap()
    {
        const Steady::time_point now = Steady::now();
        std::erase_if(peers_, [&](const std::shared_ptr<Peer>& peer) {
            if (!lingering(*peer) || peer->lingerUntil > now)
                return false;
            closeSocket(peer->socket);
            return true;
        });
    }

    void closeAll()
    {
        for (const auto& peer : peers_)
            closeSocket(peer->socket);
        peers_.clear();
        if (listener_ != kNoSocket)
            closeSocket(std::exchange(listener_, kNoSocket));
    }

    const std::shared_ptr<Wake> wake_ = std::make_shared<Wake>();
    Socket listener_ = kNoSocket;
    Handlers handlers_;
    OnAccept onAccept_;
    OnTick onTick_;
    std::vector<std::shared_ptr<Peer>> peers_;
    std::atomic<bool> stopped_{false};
    std::thread thread_;
};

Socket listenSocket(int family, uint16_t port, bool loopbackOnly)
{
    const Socket socket = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kNoSocket)
        return kNoSocket;
    const int on = 1, off = 0;
#ifdef _WIN32
    ::setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof on);
#else
    ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
#endif
    sockaddr_storage at{};
    socklen_t size = 0;
    if (family == AF_INET6) {
        ::setsockopt(socket, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof off);
        auto& six = reinterpret_cast<sockaddr_in6&>(at);
        six.sin6_family = AF_INET6;
        six.sin6_port = htons(port);
        six.sin6_addr = in6addr_any;
        size = sizeof six;
    } else {
        in_addr address{};
        address.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
        reinterpret_cast<sockaddr_in&>(at) = ipv4Endpoint(address, port);
        size = sizeof(sockaddr_in);
    }
    if (::bind(socket, reinterpret_cast<sockaddr*>(&at), size) != 0 || ::listen(socket, SOMAXCONN) != 0 ||
        !setNonBlocking(socket)) {
        closeSocket(socket);
        return kNoSocket;
    }
    noInherit(socket);
    return socket;
}

uint16_t boundPort(Socket socket)
{
    sockaddr_storage at{};
    socklen_t size = sizeof at;
    ::getsockname(socket, reinterpret_cast<sockaddr*>(&at), &size);
    return ntohs(at.ss_family == AF_INET6 ? reinterpret_cast<sockaddr_in6&>(at).sin6_port
                                          : reinterpret_cast<sockaddr_in&>(at).sin_port);
}

// Waits until `socket` is ready for `events` or `until` passes.
bool waitFor(Socket socket, short events, Steady::time_point until)
{
    for (;;) {
        pollfd fd{socket, events, 0};
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - Steady::now()).count();
        if (left <= 0)
            return false;
        const int ready = pollSockets(&fd, 1, int(std::min<int64_t>(left, kMaxPollMs)));
        if (ready > 0)
            return (fd.revents & events) != 0 || (fd.revents & (POLLERR | POLLHUP)) == 0;
        if (ready < 0)
            return false;
    }
}

// A connected, non-blocking TCP socket to `address`, or none by `until`.
Socket connectTo(const std::string& address, uint16_t port, Steady::time_point until)
{
    addrinfo hints{};
    hints.ai_flags = AI_NUMERICHOST;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(address.c_str(), std::to_string(port).c_str(), &hints, &found) != 0)
        return kNoSocket;
    const Socket socket = ::socket(found->ai_family, SOCK_STREAM, IPPROTO_TCP);
    bool connected = socket != kNoSocket && setNonBlocking(socket);
    if (connected && ::connect(socket, found->ai_addr, socklen_t(found->ai_addrlen)) != 0)
        connected = wouldBlock(socketError()) && waitFor(socket, POLLOUT, until);
    freeaddrinfo(found);
    if (connected) {
        int error = 0;
        socklen_t size = sizeof error;
        ::getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size);
        connected = error == 0;
    }
    if (!connected) {
        if (socket != kNoSocket)
            closeSocket(socket);
        return kNoSocket;
    }
    prepareConnection(socket);
    return socket;
}

// Sends the opening handshake and reads the answer; what came after the head is left in `rest`.
bool openingHandshake(Socket socket, const std::string& hostHeader, Steady::time_point until, std::string& rest)
{
    const std::string key = randomKey();
    const std::string request = handshakeRequest(hostHeader, key);
    for (size_t sent = 0; sent < request.size();) {
        const long n = sendSome(socket, request.data() + sent, request.size() - sent);
        if (n < 0 && wouldBlock(socketError()) && waitFor(socket, POLLOUT, until))
            continue;
        if (n <= 0)
            return false;
        sent += size_t(n);
    }
    std::string head;
    char buffer[4096];
    while (headEnd(head) == std::string::npos) {
        if (head.size() > kMaxHeadBytes || !waitFor(socket, POLLIN, until))
            return false;
        const long n = receiveSome(socket, buffer, sizeof buffer);
        if (n < 0 && wouldBlock(socketError()))
            continue;
        if (n <= 0)
            return false;
        head.append(buffer, size_t(n));
    }
    const std::optional<Head> response = parseHead(head);
    if (!response || !handshakeAccepted(*response, key))
        return false;
    rest = head.substr(headEnd(head));
    return true;
}

std::string hostHeader(const std::string& host, uint16_t port)
{
    const std::string url = webSocketUrl(host, port);
    return url.substr(5, url.size() - 6);
}

} // namespace

// All state but the document's is touched on the loop's thread only, or after it stopped.
struct WebSocketServer::Impl {
    DocumentServer& document;
    const Pairing pairing;
    const std::shared_ptr<Loop> loop = std::make_shared<Loop>();
    std::map<ClientId, std::shared_ptr<Peer>> peers;

    Impl(DocumentServer& d, Pairing p) : document(d), pairing(std::move(p)) {}

    size_t unpairedCount() const
    {
        return std::count_if(peers.begin(), peers.end(), [](const auto& entry) { return !entry.second->paired; });
    }

    size_t unpairedFrom(const std::string& address) const
    {
        return std::count_if(peers.begin(), peers.end(), [&](const auto& entry) {
            return !entry.second->paired && entry.second->address == address;
        });
    }

    void accept(Socket socket, const std::string& address)
    {
        const bool loopback = pairing.trustLoopback && !address.empty() && loopbackAddress(address);
        if (!loopback &&
            (unpairedCount() >= pairing.maxUnpaired || unpairedFrom(address) >= pairing.maxUnpairedPerHost)) {
            closeSocket(socket);
            return;
        }
        auto peer = std::make_shared<Peer>(socket, Encoding::MessagePack, false, loop->wake(),
                                           loopback ? kMaxMessageBytes : kMaxUnpairedMessageBytes);
        peer->paired = loopback;
        peer->address = address;
        if (!loopback)
            peer->helloBy = Steady::now() + pairing.helloTimeout;
        peer->id = document.connect([weakPeer = std::weak_ptr<Peer>(peer)](const Message& message) {
            auto p = weakPeer.lock();
            if (p && worthSending(message, p->pending, p->video))
                send(p, message);
        }, loopback);
        peers[peer->id] = peer;
        loop->add(peer);
    }

    void message(const std::shared_ptr<Peer>& peer, const std::vector<uint8_t>& payload, bool text)
    {
        peer->encoding = text ? Encoding::Json : Encoding::MessagePack;
        if (peer->refused)
            return;
        try {
            receive(peer, payload);
        } catch (const std::exception& e) {
            peer->refused = !peer->paired;
            send(peer, failed(e), peer->refused);
        }
    }

    void receive(const std::shared_ptr<Peer>& peer, const std::vector<uint8_t>& payload)
    {
        const std::optional<Message> message = decode(payload.data(), payload.size(), peer->encoding);
        if (!peer->paired && !presentsToken(message, pairing.token)) {
            refuse(peer);
            return;
        }
        if (!peer->paired) {
            peer->paired = true;
            peer->helloBy.reset();
            peer->reader.setLimit(kMaxMessageBytes);
        }
        if (message)
            document.receive(peer->id, *message);
        else
            send(peer, undecodable());
    }

    void refuse(const std::shared_ptr<Peer>& peer)
    {
        peer->refused = true;
        send(peer, unpaired(), true);
    }

    // Before pairing a message over its limit is refused unread, as one without the token; after, it closes the
    // connection.
    void tooBig(const std::shared_ptr<Peer>& peer)
    {
        if (peer->paired)
            closeWith(*peer, kCloseTooBig);
        else
            refuse(peer);
    }

    void closed(const std::shared_ptr<Peer>& peer)
    {
        if (peers.erase(peer->id))
            document.disconnect(peer->id);
    }

    // A client from another machine that has not paired in time is closed.
    void tick(Steady::time_point now)
    {
        std::vector<std::shared_ptr<Peer>> late;
        for (const auto& [id, peer] : peers)
            if (peer->helloBy && now >= *peer->helloBy)
                late.push_back(peer);
        for (const auto& peer : late) {
            peer->helloBy.reset();
            loop->close(peer);
        }
    }

    void start(Socket listener)
    {
        Handlers handlers;
        handlers.onMessage = [this](const std::shared_ptr<Peer>& p, std::vector<uint8_t>&& payload, bool text) {
            message(p, payload, text);
        };
        handlers.onClosed = [this](const std::shared_ptr<Peer>& p) { closed(p); };
        handlers.onTooBig = [this](const std::shared_ptr<Peer>& p) { tooBig(p); };
        loop->start(listener, std::move(handlers), [this](Socket s, const std::string& address) { accept(s, address); },
                    [this](Steady::time_point now) { tick(now); });
    }

    void stop()
    {
        loop->stop();
        for (auto& [id, peer] : peers)
            document.disconnect(id);
        peers.clear();
    }
};

WebSocketServer::WebSocketServer(DocumentServer& document, uint16_t port, Pairing pairing, bool loopbackOnly)
    : impl_(std::make_shared<Impl>(document, std::move(pairing)))
{
    startSockets();
    Socket listener = loopbackOnly ? kNoSocket : listenSocket(AF_INET6, port, false);
    if (listener == kNoSocket)
        listener = listenSocket(AF_INET, port, loopbackOnly);
    if (listener == kNoSocket)
        throw std::runtime_error("cannot listen on port " + std::to_string(port));
    port_ = boundPort(listener);
    impl_->start(listener);
}

WebSocketServer::~WebSocketServer()
{
    impl_->stop();
}

// Callbacks run on the loop's thread; once destroyed, none starts again.
struct WebSocketClient::Impl {
    const std::shared_ptr<Loop> loop = std::make_shared<Loop>();
    std::shared_ptr<Peer> peer;
};

WebSocketClient::WebSocketClient(const std::string& host, uint16_t port, Encoding encoding, OnMessage onMessage,
                                 OnClose onClose)
    : impl_(std::make_shared<Impl>())
{
    startSockets();
    const std::vector<std::string> addresses = resolveHost(host);
    const Steady::time_point until = Steady::now() + kSetupTimeout;
    std::string rest;
    Socket socket = kNoSocket;
    for (const std::string& address : addresses) {
        socket = connectTo(address, port, until);
        if (socket != kNoSocket && openingHandshake(socket, hostHeader(host, port), until, rest))
            break;
        if (socket != kNoSocket)
            closeSocket(std::exchange(socket, kNoSocket));
    }
    if (socket == kNoSocket) {
        const bool named = addresses != std::vector<std::string>{host};
        throw std::runtime_error("cannot connect to " + webSocketUrl(host, port) +
                                 (named ? " (" + joined(addresses) + ")" : ""));
    }

    Loop& loop = *impl_->loop;
    impl_->peer = std::make_shared<Peer>(socket, encoding, true, loop.wake(), kMaxReceivedByClient);
    impl_->peer->state = Peer::State::Open;
    impl_->peer->reader.feed(reinterpret_cast<const uint8_t*>(rest.data()), rest.size());
    loop.add(impl_->peer);

    Handlers handlers;
    handlers.onMessage = [onMessage](const std::shared_ptr<Peer>& p, std::vector<uint8_t>&& payload, bool text) {
        p->encoding = text ? Encoding::Json : Encoding::MessagePack;
        if (const std::optional<Message> message = decode(payload.data(), payload.size(), p->encoding))
            onMessage(*message);
    };
    handlers.onClosed = [onClose](const std::shared_ptr<Peer>&) {
        if (onClose)
            onClose();
    };
    handlers.onTooBig = [](const std::shared_ptr<Peer>& p) { closeWith(*p, kCloseTooBig); };
    loop.start(kNoSocket, std::move(handlers));
}

WebSocketClient::~WebSocketClient()
{
    impl_->loop->stop();
}

void WebSocketClient::send(const Message& message)
{
    azl::send(impl_->peer, message);
}

} // namespace azl
