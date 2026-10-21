#include "net/pairing.hpp"
#include "net/websocket.hpp"
#include "test_client.hpp"
#include "test_paths.hpp"

#include <sys/stat.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>

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
    harness.document().handle("whoami", [](const Message& m, const azl::DocumentServer::Sink& reply) {
        reply({{"type", "whoami"}, {"loopback", m["loopback"]}});
    });
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
