// The WebSocket transport on its own, without an engine: whichever implementation the build uses (Network.framework
// on macOS, plain sockets elsewhere and in nocturne_socket_tests) must behave the same on the wire.

#include "net/lan.hpp"
#include "net/socket_compat.hpp"
#include "net/websocket.hpp"
#include "net/websocket_frames.hpp"
#include "protocol/document_server.hpp"

#include <doctest.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

using azl::Message;
namespace msg = azl::msg;
namespace ws = azl::ws;

constexpr const char* kToken = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

struct Document {
    Document()
    {
        server.handle("whoami", [](const Message& m, const azl::DocumentServer::Sink& reply) {
            reply({{"type", "whoami"}, {"loopback", m["loopback"]}});
        });
    }
    azl::DocumentServer server{azl::Composition{}, [](std::shared_ptr<const azl::Composition>) {}};
};

// A WebSocket client written by hand on blocking sockets, to send what WebSocketClient never would.
class Raw {
public:
    explicit Raw(uint16_t port, const std::string& headers = "")
    {
        azl::startSockets();
        socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        REQUIRE(socket_ != azl::kNoSocket);
        azl::noSigPipe(socket_);
        azl::setTimeouts(socket_, 5.0);
        in_addr loopback{};
        loopback.s_addr = htonl(INADDR_LOOPBACK);
        const sockaddr_in at = azl::ipv4Endpoint(loopback, port);
        REQUIRE(::connect(socket_, reinterpret_cast<const sockaddr*>(&at), sizeof at) == 0);
        write("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
              "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n" +
              headers + "\r\n");
        std::string head;
        char c = 0;
        while (ws::headEnd(head) == std::string::npos && azl::receiveSome(socket_, &c, 1) == 1)
            head += c;
        upgraded = head.starts_with("HTTP/1.1 101");
    }
    ~Raw() { azl::closeSocket(socket_); }

    void send(ws::Opcode opcode, const std::string& payload, bool fin = true)
    {
        std::vector<uint8_t> f = ws::frame(opcode, reinterpret_cast<const uint8_t*>(payload.data()), payload.size(),
                                           std::array<uint8_t, 4>{1, 2, 3, 4});
        if (!fin)
            f[0] &= 0x7f;
        write({f.begin(), f.end()});
    }

    void write(const std::string& bytes) { azl::sendAll(socket_, bytes.data(), bytes.size()); }

    // The next frame from the server; none when the connection ended or nothing came in time.
    std::optional<ws::Event> next()
    {
        for (;;) {
            if (auto event = reader_.next())
                return event;
            uint8_t buffer[4096];
            const long n = azl::receiveSome(socket_, buffer, sizeof buffer);
            if (n <= 0)
                return std::nullopt;
            reader_.feed(buffer, size_t(n));
        }
    }

    // The next data message, decoded as JSON.
    Message message()
    {
        for (auto event = next(); event; event = next())
            if (event->kind == ws::Event::Kind::Text)
                return Message::parse(event->payload.begin(), event->payload.end());
        return nullptr;
    }

    // Whether the server ended the connection, whatever it sent before.
    bool closedWithin(double seconds)
    {
        azl::setTimeouts(socket_, seconds);
        char buffer[4096];
        for (;;) {
            const long n = azl::receiveSome(socket_, buffer, sizeof buffer);
            if (n == 0)
                return true;
            const int error = azl::socketError();
            if (n < 0)
                return !azl::wouldBlock(error) && error != timedOut();
        }
    }

    bool upgraded = false;

private:
    static int timedOut()
    {
#ifdef _WIN32
        return WSAETIMEDOUT;
#else
        return EAGAIN;
#endif
    }

    azl::Socket socket_ = azl::kNoSocket;
    ws::MessageReader reader_{false, size_t(64) << 20};
};

std::string hello(const std::string& token = "")
{
    return msg::hello(token).dump();
}

bool isError(const Message& reply, const std::string& text)
{
    INFO(reply.dump());
    return reply.is_object() && reply["type"] == "error" &&
           reply["message"].get<std::string>().find(text) != std::string::npos;
}

// A WebSocketClient with what it received and whether it was closed.
struct Client {
    Client(const std::string& host, uint16_t port)
        : socket(host, port, azl::Encoding::Json,
                 [this](const Message& m) {
                     std::lock_guard lock(mutex);
                     inbox.push_back(m);
                     changed.notify_all();
                 },
                 [this] {
                     std::lock_guard lock(mutex);
                     closed = true;
                     changed.notify_all();
                 })
    {
    }

    Message next()
    {
        std::unique_lock lock(mutex);
        changed.wait_for(lock, std::chrono::seconds(5), [&] { return !inbox.empty() || closed; });
        if (inbox.empty())
            return nullptr;
        Message m = inbox.front();
        inbox.pop_front();
        return m;
    }

    bool closedWithin(double seconds)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::duration<double>(seconds), [&] { return closed; });
    }

    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Message> inbox;
    bool closed = false;
    azl::WebSocketClient socket;
};

} // namespace

TEST_CASE("websocket: a web page's handshake, with its Origin header, is refused; a native client's gets in")
{
    Document document;
    azl::WebSocketServer server(document.server, 0);
    CHECK_FALSE(Raw(server.port(), "Origin: http://example.com\r\n").upgraded);
    CHECK_FALSE(Raw(server.port(), "origin: null\r\n").upgraded);
    CHECK(Raw(server.port()).upgraded);
}

TEST_CASE("websocket: a client from another machine pairs with the token in its hello, and only then")
{
    Document document;
    azl::WebSocketServer server(document.server, 0, {kToken, false});
    {
        Raw raw(server.port());
        REQUIRE(raw.upgraded);
        raw.send(ws::Opcode::Text, hello(std::string(64, 'a')));
        CHECK(isError(raw.message(), "pairing token"));
        CHECK(raw.closedWithin(5.0));
    }
    Raw raw(server.port());
    REQUIRE(raw.upgraded);
    raw.send(ws::Opcode::Text, hello(kToken));
    CHECK(raw.message()["type"] == "snapshot");
    raw.send(ws::Opcode::Text, R"({"type":"whoami"})");
    CHECK(raw.message() == Message{{"type", "whoami"}, {"loopback", false}});
}

TEST_CASE("websocket: an unpaired client is closed after its hello timeout, and only so many may wait")
{
    Document document;
    azl::WebSocketServer server(document.server, 0,
                                {.token = kToken,
                                 .trustLoopback = false,
                                 .helloTimeout = std::chrono::milliseconds(300),
                                 .maxUnpaired = 2});
    Raw first(server.port());
    Raw second(server.port());
    CHECK(first.upgraded);
    CHECK(second.upgraded);
    CHECK_FALSE(Raw(server.port()).upgraded);
    CHECK(first.closedWithin(3.0));
    CHECK(second.closedWithin(3.0));
    CHECK(Raw(server.port()).upgraded);
}

TEST_CASE("websocket: a ping is answered with a pong, and a message in fragments arrives whole")
{
    Document document;
    azl::WebSocketServer server(document.server, 0);
    Raw raw(server.port());
    REQUIRE(raw.upgraded);
    raw.send(ws::Opcode::Text, hello());
    REQUIRE(raw.message()["type"] == "snapshot");

    raw.send(ws::Opcode::Ping, "are you there");
    std::optional<ws::Event> pong = raw.next();
    REQUIRE(pong);
    CHECK(pong->kind == ws::Event::Kind::Pong);
    CHECK(std::string(pong->payload.begin(), pong->payload.end()) == "are you there");

    raw.send(ws::Opcode::Text, R"({"type":)", false);
    raw.send(ws::Opcode::Ping, "");
    raw.send(ws::Opcode::Continuation, R"("who)", false);
    raw.send(ws::Opcode::Continuation, R"(ami"})");
    CHECK(raw.message() == Message{{"type", "whoami"}, {"loopback", true}});
}

TEST_CASE("websocket: a client's close frame ends the connection; a message over the size limit does too")
{
    Document document;
    azl::WebSocketServer server(document.server, 0);
    {
        Raw raw(server.port());
        REQUIRE(raw.upgraded);
        raw.send(ws::Opcode::Close, std::string("\x03\xe8", 2));
        CHECK(raw.closedWithin(5.0));
    }
    Raw raw(server.port());
    REQUIRE(raw.upgraded);
    raw.send(ws::Opcode::Text, hello());
    REQUIRE(raw.message()["type"] == "snapshot");
    raw.send(ws::Opcode::Binary, std::string(size_t(17) << 20, '\xc0'));
    CHECK(raw.closedWithin(5.0));
}

TEST_CASE("websocket: a client reaches the server by name, IPv4 or IPv6, and the server reaches it back")
{
    Document document;
    azl::WebSocketServer server(document.server, 0);
    for (const std::string host : {"localhost", "127.0.0.1", "::1"}) {
        CAPTURE(host);
        Client client(host, server.port());
        client.socket.send(msg::hello());
        CHECK(client.next()["type"] == "snapshot");
        client.socket.send({{"type", "whoami"}, {"data", std::string(size_t(1) << 20, 'x')}});
        CHECK(client.next() == Message{{"type", "whoami"}, {"loopback", true}});
    }
}

TEST_CASE("websocket: a loopback-only server answers on 127.0.0.1 alone")
{
    Document document;
    azl::WebSocketServer server(document.server, 0, {}, true);
    Client client("127.0.0.1", server.port());
    client.socket.send(msg::hello());
    CHECK(client.next()["type"] == "snapshot");
    CHECK_THROWS(Client("::1", server.port()));
    for (const std::string& address : azl::lanAddresses()) {
        CAPTURE(address);
        CHECK_THROWS(Client(address, server.port()));
    }
}

TEST_CASE("websocket: a client is told when the server goes away")
{
    Document document;
    std::optional<azl::WebSocketServer> server(std::in_place, document.server, 0);
    Client client("127.0.0.1", server->port());
    client.socket.send(msg::hello());
    REQUIRE(client.next()["type"] == "snapshot");
    server.reset();
    CHECK(client.closedWithin(5.0));
}
