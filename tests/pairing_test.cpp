#include "net/pairing.hpp"
#include "net/websocket.hpp"
#include "test_client.hpp"
#include "test_paths.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

namespace {

constexpr const char* kToken = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

// A WebSocket client in the test, with what it received and whether the engine closed it.
struct Socket {
    Socket(uint16_t port)
        : client("127.0.0.1", port, azl::Encoding::Json,
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

    // The next message; null when the connection closed first or nothing came in time.
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
    azl::WebSocketClient client;
};

bool refusedForToken(Socket& socket)
{
    const Message reply = socket.next();
    INFO(reply.dump());
    return reply.is_object() && reply["type"] == "error" &&
           reply["message"].get<std::string>().find("pairing token") != std::string::npos &&
           socket.closedWithin(5.0);
}

// A WebSocket client written by hand, to send what WebSocketClient never would.
class RawSocket {
public:
    explicit RawSocket(uint16_t port, const std::string& headers = "")
    {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(fd_ >= 0);
        const int on = 1;
        setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
        const timeval timeout{5, 0};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(connect(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0);
        write("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
              "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n" +
              headers + "\r\n");
        std::string head;
        char c = 0;
        while (head.find("\r\n\r\n") == std::string::npos && recv(fd_, &c, 1, 0) == 1)
            head += c;
        status_ = head.substr(0, head.find("\r\n"));
    }
    ~RawSocket() { close(fd_); }

    bool upgraded() const { return status_.starts_with("HTTP/1.1 101"); }

    // One masked frame; text when `text`, else binary.
    void send(const std::vector<uint8_t>& payload, bool text = false)
    {
        std::string frame{char(text ? 0x81 : 0x82)};
        if (payload.size() < 126) {
            frame += char(0x80 | payload.size());
        } else {
            frame += char(0x80 | 127);
            for (int shift = 56; shift >= 0; shift -= 8)
                frame += char(uint64_t(payload.size()) >> shift);
        }
        frame += std::string(4, '\0');
        frame.append(payload.begin(), payload.end());
        write(frame);
    }

    // The next data message the engine sent, decoded; null when the connection closed or nothing came in time.
    Message next()
    {
        for (;;) {
            uint8_t head[2];
            if (!read(head, 2))
                return nullptr;
            uint64_t size = head[1] & 0x7f;
            const int extended = size == 126 ? 2 : size == 127 ? 8 : 0;
            if (extended) {
                uint8_t bytes[8];
                if (!read(bytes, extended))
                    return nullptr;
                size = 0;
                for (int i = 0; i < extended; ++i)
                    size = size << 8 | bytes[i];
            }
            std::vector<uint8_t> payload(size);
            if (!read(payload.data(), size))
                return nullptr;
            const int opcode = head[0] & 0x0f;
            if (opcode == 0x8)
                return nullptr;
            if (opcode == 0x1)
                return Message::parse(payload.begin(), payload.end());
            if (opcode == 0x2)
                return Message::from_msgpack(payload);
        }
    }

    // Whether the engine closed the connection, whatever it sent before.
    bool closedWithin(double seconds)
    {
        const timeval timeout{time_t(seconds), int((seconds - int(seconds)) * 1e6)};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        char buffer[4096];
        for (;;) {
            const ssize_t n = recv(fd_, buffer, sizeof buffer, 0);
            if (n == 0 || (n < 0 && errno != EAGAIN))
                return true;
            if (n < 0)
                return false;
        }
    }

private:
    void write(const std::string& bytes)
    {
        for (size_t sent = 0; sent < bytes.size();) {
            const ssize_t n = ::send(fd_, bytes.data() + sent, bytes.size() - sent, 0);
            if (n <= 0)
                return;
            sent += size_t(n);
        }
    }

    bool read(uint8_t* into, uint64_t size)
    {
        for (uint64_t got = 0; got < size;) {
            const ssize_t n = recv(fd_, into + got, size - got, 0);
            if (n <= 0)
                return false;
            got += uint64_t(n);
        }
        return true;
    }

    int fd_ = -1;
    std::string status_;
};

bool isError(const Message& reply, const std::string& text)
{
    INFO(reply.dump());
    return reply.is_object() && reply["type"] == "error" &&
           reply["message"].get<std::string>().find(text) != std::string::npos;
}

// {"type": "whoami", "deep": [[[ ... ]]]}, nested `depth` arrays deep, as MessagePack.
std::vector<uint8_t> nestedMsgpack(size_t depth)
{
    std::vector<uint8_t> bytes{0x82, 0xa4, 't', 'y', 'p', 'e', 0xa6, 'w', 'h', 'o', 'a', 'm', 'i', 0xa4, 'd', 'e', 'e', 'p'};
    bytes.insert(bytes.end(), depth, 0x91);
    bytes.push_back(0xc0);
    return bytes;
}

// A hello asking for no document, so the next reply is the answer to what follows.
void sayHello(RawSocket& raw)
{
    raw.send({0x82, 0xa4, 't', 'y', 'p', 'e', 0xa5, 'h', 'e', 'l', 'l', 'o', 0xa5, 'w', 'a', 't', 'c', 'h', 0xc2});
}

void answerWhoami(azl::DocumentServer& document)
{
    document.handle("whoami", [](const Message& m, const azl::DocumentServer::Sink& reply) {
        reply({{"type", "whoami"}, {"loopback", m["loopback"]}});
    });
}

std::pair<int, std::string> ctl(const std::string& args)
{
    const std::string command = "env -u NOCTURNE_TOKEN '" + ctlPath().string() + "' " + args + " 2>&1";
    std::string out;
    FILE* pipe = popen(command.c_str(), "r");
    REQUIRE(pipe);
    char buffer[512];
    while (fgets(buffer, sizeof buffer, pipe))
        out += buffer;
    const int status = pclose(pipe);
    return {WEXITSTATUS(status), out};
}

} // namespace

TEST_CASE("the pairing token is made once per state folder, 64 hex digits readable by its owner alone")
{
    TempDir dir;
    const fs::path file = azl::tokenFile(dir.path / "State");
    CHECK(azl::readToken(file).empty());
    const std::string token = azl::machineToken(file);
    CHECK(token.size() == 64);
    CHECK(token.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(azl::machineToken(file) == token);
    CHECK(azl::readToken(file) == token);
    struct stat info {};
    REQUIRE(stat(file.c_str(), &info) == 0);
    CHECK((info.st_mode & 0777) == 0600);

    TempDir other;
    CHECK(azl::machineToken(azl::tokenFile(other.path)) != token);

    CHECK(azl::sameToken(token, token));
    CHECK_FALSE(azl::sameToken(token.substr(1), token));
    CHECK_FALSE(azl::sameToken("", ""));
    CHECK_FALSE(azl::sameToken(std::string(64, '0'), token));

    const auto [code, out] = ctl("token '" + (dir.path / "State").string() + "'");
    CHECK(code == 0);
    CHECK(out == token + "\n");
}

TEST_CASE("a client from another machine gets in only with the pairing token in its hello, and handlers see it as "
          "remote; one from loopback needs none")
{
    azl::EngineHarness harness({.width = 160, .height = 90});
    answerWhoami(harness.document());
    azl::WebSocketServer remote(harness.document(), 0, {kToken, false});

    SUBCASE("no token")
    {
        Socket socket(remote.port());
        socket.client.send(msg::hello());
        CHECK(refusedForToken(socket));
    }
    SUBCASE("a wrong token")
    {
        Socket socket(remote.port());
        socket.client.send(msg::hello(std::string(64, 'a')));
        CHECK(refusedForToken(socket));
    }
    SUBCASE("a command before any hello")
    {
        Socket socket(remote.port());
        socket.client.send({{"type", "whoami"}, {"token", kToken}});
        CHECK(refusedForToken(socket));
    }
    SUBCASE("the token")
    {
        Socket socket(remote.port());
        socket.client.send(msg::hello(kToken));
        CHECK(socket.next()["type"] == "snapshot");
        socket.client.send({{"type", "whoami"}});
        CHECK(socket.next() == Message{{"type", "whoami"}, {"loopback", false}});
        CHECK_FALSE(socket.closedWithin(0.2));
    }
    SUBCASE("loopback")
    {
        azl::WebSocketServer local(harness.document(), 0, {kToken});
        Socket socket(local.port());
        socket.client.send(msg::hello());
        CHECK(socket.next()["type"] == "snapshot");
        socket.client.send({{"type", "whoami"}});
        CHECK(socket.next() == Message{{"type", "whoami"}, {"loopback", true}});
    }
}

TEST_CASE("an engine without a pairing token lets no client from another machine in, whatever it sends")
{
    azl::EngineHarness harness({.width = 160, .height = 90});
    azl::WebSocketServer remote(harness.document(), 0, {"", false});
    Socket socket(remote.port());
    Message hello = msg::hello();
    hello["token"] = "";
    socket.client.send(hello);
    CHECK(refusedForToken(socket));
}

TEST_CASE("nocturne-ctl presents --token or NOCTURNE_TOKEN, and says why the engine refused without it")
{
    azl::EngineHarness harness({.width = 160, .height = 90});
    azl::WebSocketServer remote(harness.document(), 0, {kToken, false});
    const std::string port = " --port " + std::to_string(remote.port());

    auto [code, out] = ctl("--token " + std::string(kToken) + port + " snapshot");
    CHECK(code == 0);
    CHECK(out.find("\"snapshot\"") != std::string::npos);

    std::tie(code, out) = ctl("--token " + std::string(kToken) + port + " switches");
    CHECK(code == 0);

    std::tie(code, out) = ctl(port + " snapshot");
    CHECK(code == 1);
    CHECK(out.find("pairing token") != std::string::npos);

    const std::string withEnv = "NOCTURNE_TOKEN=" + std::string(kToken) + " '" + ctlPath().string() + "'" + port +
                                " snapshot > /dev/null 2>&1";
    CHECK(std::system(withEnv.c_str()) == 0);
}

TEST_CASE("a hello whose type is not a string is refused for the token, and the engine keeps serving")
{
    azl::EngineHarness harness({.width = 160, .height = 90});
    azl::WebSocketServer remote(harness.document(), 0, {kToken, false});
    {
        Socket socket(remote.port());
        socket.client.send({{"type", 1}, {"token", kToken}});
        CHECK(refusedForToken(socket));
    }
    Socket socket(remote.port());
    socket.client.send(msg::hello(kToken));
    CHECK(socket.next()["type"] == "snapshot");
}

TEST_CASE("MessagePack or JSON nested deeper than the engine reads is refused as undecodable, before pairing too")
{
    azl::EngineHarness harness({.width = 160, .height = 90});
    answerWhoami(harness.document());

    SUBCASE("from another machine, before its hello")
    {
        azl::WebSocketServer remote(harness.document(), 0, {kToken, false});
        RawSocket raw(remote.port());
        REQUIRE(raw.upgraded());
        raw.send(nestedMsgpack(200000));
        CHECK(isError(raw.next(), "pairing token"));
        CHECK(raw.closedWithin(5.0));
    }
    SUBCASE("from loopback")
    {
        azl::WebSocketServer local(harness.document(), 0);
        RawSocket raw(local.port());
        REQUIRE(raw.upgraded());
        sayHello(raw);
        raw.send(nestedMsgpack(200000));
        CHECK(isError(raw.next(), "undecodable"));
        const std::string deep =
            "{\"type\":\"whoami\",\"deep\":" + std::string(200000, '[') + std::string(200000, ']') + "}";
        raw.send({deep.begin(), deep.end()}, true);
        CHECK(isError(raw.next(), "undecodable"));
        raw.send(nestedMsgpack(32));
        CHECK(raw.next() == Message{{"type", "whoami"}, {"loopback", true}});
    }
}

TEST_CASE("a message over the engine's size limit closes the connection; four bundle chunks' worth still gets in")
{
    azl::EngineHarness harness({.width = 160, .height = 90});
    answerWhoami(harness.document());
    azl::WebSocketServer local(harness.document(), 0);

    RawSocket raw(local.port());
    REQUIRE(raw.upgraded());
    raw.send(std::vector<uint8_t>(size_t(17) << 20, 0xc0));
    CHECK(raw.closedWithin(5.0));

    std::mutex mutex;
    std::condition_variable arrived;
    bool answered = false;
    azl::WebSocketClient client("127.0.0.1", local.port(), azl::Encoding::MessagePack, [&](const Message& m) {
        std::lock_guard lock(mutex);
        answered = answered || m["type"] == "whoami";
        arrived.notify_all();
    });
    client.send(msg::hello());
    client.send({{"type", "whoami"}, {"data", Message::binary(std::vector<uint8_t>(size_t(4) << 20, 7))}});
    std::unique_lock lock(mutex);
    CHECK(arrived.wait_for(lock, std::chrono::seconds(5), [&] { return answered; }));
}

TEST_CASE("a handshake with an Origin header, as every web page's has, is refused; one without gets in")
{
    azl::EngineHarness harness({.width = 160, .height = 90});
    azl::WebSocketServer local(harness.document(), 0);
    CHECK_FALSE(RawSocket(local.port(), "Origin: http://example.com\r\n").upgraded());
    CHECK_FALSE(RawSocket(local.port(), "origin: null\r\n").upgraded());
    CHECK(RawSocket(local.port()).upgraded());
}

TEST_CASE("a handler that throws answers its client with an error and the connection stays open")
{
    azl::EngineHarness harness({.width = 160, .height = 90});
    answerWhoami(harness.document());
    harness.document().handle("picky", [](const Message& m, const azl::DocumentServer::Sink& reply) {
        reply({{"type", "picky"}, {"name", m.value("name", "")}});
    });
    azl::WebSocketServer remote(harness.document(), 0, {kToken, false});
    Socket socket(remote.port());
    socket.client.send(msg::hello(kToken));
    REQUIRE(socket.next()["type"] == "snapshot");
    socket.client.send({{"type", "picky"}, {"name", 1}});
    CHECK(isError(socket.next(), "type must be string"));
    socket.client.send({{"type", "whoami"}});
    CHECK(socket.next() == Message{{"type", "whoami"}, {"loopback", false}});
}
