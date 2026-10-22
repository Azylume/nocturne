#include "harness/engine_harness.hpp"
#include "net/websocket.hpp"
#include "protocol/document_mirror.hpp"

#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace {

using azl::Message;
using azl::SetLayerOpacity;
namespace msg = azl::msg;

// A client living in the test, talking to the document server directly.
class InProcessClient {
public:
    explicit InProcessClient(azl::DocumentServer& server, bool hello = true)
        : server_(server), id_(server.connect([this](const Message& m) { inbox.push_back(m); }))
    {
        if (hello)
            send(msg::hello());
    }
    ~InProcessClient() { server_.disconnect(id_); }

    void send(const Message& message) { server_.receive(id_, message); }
    azl::ClientId id() const { return id_; }

    std::vector<Message> inbox;

private:
    azl::DocumentServer& server_;
    azl::ClientId id_;
};

// Collects what a WebSocket client receives on its own queue.
class Inbox {
public:
    void push(const Message& message)
    {
        std::lock_guard lock(mutex_);
        messages_.push_back(message);
        arrived_.notify_all();
    }

    Message at(size_t index)
    {
        std::unique_lock lock(mutex_);
        REQUIRE_MESSAGE(arrived_.wait_for(lock, std::chrono::seconds(5), [&] { return messages_.size() > index; }),
                        "no message #" << index);
        return messages_[index];
    }

private:
    std::mutex mutex_;
    std::condition_variable arrived_;
    std::vector<Message> messages_;
};

Message setOpacity(double opacity)
{
    return msg::command(SetLayerOpacity{1, opacity});
}

double opacityIn(const Message& change)
{
    return change.at("opacity").get<double>();
}

int red(const azl::Image& image, uint32_t x, uint32_t y)
{
    return image.rgba[(size_t(y) * image.width + x) * 4];
}

// The test card's white edge and its dark grey background, at 960x540.
constexpr uint32_t kEdge[2] = {1, 1};
constexpr uint32_t kBackground[2] = {90, 30};

void checkBrightness(azl::EngineHarness& harness, int edge, int background)
{
    const azl::Image image = harness.readScreen();
    CHECK(std::abs(red(image, kEdge[0], kEdge[1]) - edge) <= 2);
    CHECK(std::abs(red(image, kBackground[0], kBackground[1]) - background) <= 2);
}

} // namespace

TEST_CASE("a client gets a snapshot on connect, then numbered changes")
{
    azl::EngineHarness harness({.width = 960, .height = 540});
    InProcessClient client(harness.document());

    REQUIRE(client.inbox.size() == 1);
    const Message& snapshot = client.inbox[0];
    CHECK(snapshot["type"] == "snapshot");
    CHECK(snapshot["change"] == 0);
    CHECK(snapshot["composition"]["layers"][0]["name"] == "Layer 1");
    CHECK(snapshot["composition"]["layers"][0]["opacity"] == 1.0);

    client.send(setOpacity(0.25));
    client.send(setOpacity(0.75));
    REQUIRE(client.inbox.size() == 3);
    CHECK(client.inbox[1]["type"] == "change");
    CHECK(client.inbox[1]["change"] == 1);
    CHECK(client.inbox[1]["op"] == "setLayerOpacity");
    CHECK(client.inbox[1]["cause"] == "edit");
    CHECK(opacityIn(client.inbox[1]) == 0.25);
    CHECK(client.inbox[2]["change"] == 2);
    CHECK(opacityIn(client.inbox[2]) == 0.75);
}

TEST_CASE("an edit from one client reaches every connected client")
{
    azl::EngineHarness harness({.width = 960, .height = 540});
    InProcessClient a(harness.document());
    InProcessClient b(harness.document());

    a.send(setOpacity(0.5));
    REQUIRE(a.inbox.size() == 2);
    REQUIRE(b.inbox.size() == 2);
    CHECK(a.inbox[1] == b.inbox[1]);
    CHECK(b.inbox[1]["client"] == a.id());
}

TEST_CASE("undo and redo revert and reapply another client's edit")
{
    azl::EngineHarness harness({.width = 960, .height = 540});
    InProcessClient a(harness.document());
    InProcessClient b(harness.document());

    a.send(setOpacity(0.25));
    b.send(msg::undo());
    REQUIRE(a.inbox.size() == 3);
    CHECK(a.inbox[2]["change"] == 2);
    CHECK(a.inbox[2]["cause"] == "undo");
    CHECK(opacityIn(a.inbox[2]) == 1.0);
    harness.step();
    checkBrightness(harness, 255, 20);

    b.send(msg::redo());
    REQUIRE(a.inbox.size() == 4);
    CHECK(a.inbox[3]["change"] == 3);
    CHECK(a.inbox[3]["cause"] == "redo");
    CHECK(opacityIn(a.inbox[3]) == 0.25);
    CHECK(std::equal(a.inbox.begin() + 1, a.inbox.end(), b.inbox.begin() + 1, b.inbox.end()));

    azl::DocumentMirror mirror;
    for (const Message& m : b.inbox)
        mirror.receive(m);
    CHECK(mirror.change() == 3);
    CHECK(mirror.composition().layers[0].opacity == 0.25);
}

TEST_CASE("a new edit clears the redo history")
{
    azl::EngineHarness harness({.width = 960, .height = 540});
    InProcessClient client(harness.document());

    client.send(setOpacity(0.25));
    client.send(msg::undo());
    client.send(setOpacity(0.5));
    client.send(msg::redo());
    CHECK(client.inbox.back()["type"] == "error");
    client.send(msg::undo());
    CHECK(opacityIn(client.inbox.back()) == 1.0);
}

TEST_CASE("a client that misses a change re-requests a snapshot")
{
    azl::EngineHarness harness({.width = 960, .height = 540});
    InProcessClient client(harness.document());
    client.send(setOpacity(0.1));
    client.send(setOpacity(0.2));
    client.send(setOpacity(0.3));
    REQUIRE(client.inbox.size() == 4);

    azl::DocumentMirror mirror;
    CHECK_FALSE(mirror.receive(client.inbox[0]));
    CHECK_FALSE(mirror.receive(client.inbox[1]));
    const auto request = mirror.receive(client.inbox[3]);
    REQUIRE(request);
    CHECK_FALSE(mirror.synced());

    client.send(*request);
    REQUIRE(client.inbox.size() == 5);
    CHECK_FALSE(mirror.receive(client.inbox[4]));
    CHECK(mirror.synced());
    CHECK(mirror.change() == 3);
    CHECK(mirror.composition().layers[0].opacity == 0.3);
}

TEST_CASE("a rejected command is answered only to its sender and changes nothing")
{
    azl::EngineHarness harness({.width = 960, .height = 540});
    InProcessClient a(harness.document());
    InProcessClient b(harness.document());
    InProcessClient silent(harness.document(), false);

    a.send(setOpacity(2.0));
    a.send(msg::command(SetLayerOpacity{99, 0.5}));
    a.send(msg::undo());
    a.send({{"type", "command"}, {"op", "explode"}});
    silent.send(setOpacity(0.5));

    REQUIRE(a.inbox.size() == 5);
    for (size_t i = 1; i < a.inbox.size(); ++i)
        CHECK(a.inbox[i]["type"] == "error");
    CHECK(b.inbox.size() == 1);
    REQUIRE(silent.inbox.size() == 1);
    CHECK(silent.inbox[0]["type"] == "error");
}

TEST_CASE("a layer's opacity set through the protocol changes the rendered frame")
{
    azl::EngineHarness harness({.width = 960, .height = 540});
    InProcessClient client(harness.document());
    harness.step();
    checkBrightness(harness, 255, 20);

    client.send(setOpacity(0.5));
    REQUIRE(client.inbox.size() == 2);
    CHECK(client.inbox[1]["change"] == 1);
    CHECK(opacityIn(client.inbox[1]) == 0.5);
    harness.step();
    checkBrightness(harness, 128, 10);

    client.send(setOpacity(0.0));
    harness.step();
    checkBrightness(harness, 0, 0);
}

std::string connectError(const std::string& host, uint16_t port)
{
    try {
        azl::WebSocketClient client(host, port, azl::Encoding::Json, [](const Message&) {});
    } catch (const std::exception& e) {
        return e.what();
    }
    return "";
}

bool mentions(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

TEST_CASE("a client reaches the engine by host name, IPv4 or IPv6")
{
    const std::vector<std::string> local = azl::resolveHost("localhost");
    CHECK(std::find(local.begin(), local.end(), "127.0.0.1") != local.end());
    CHECK(azl::resolveHost("::1") == std::vector<std::string>{"::1"});

    azl::EngineHarness harness({.width = 960, .height = 540});
    azl::WebSocketServer server(harness.document(), 0);
    for (const std::string host : {"localhost", "127.0.0.1", "::1"}) {
        CAPTURE(host);
        Inbox inbox;
        azl::WebSocketClient client(host, server.port(), azl::Encoding::Json, [&](const Message& m) { inbox.push(m); });
        client.send(msg::hello());
        CHECK(inbox.at(0)["type"] == "snapshot");
    }
}

TEST_CASE("a host name that does not resolve, or whose addresses refuse, fails saying which")
{
    const std::string unknown = connectError("nocturne-no-such-host.invalid", azl::kDefaultPort);
    CHECK(mentions(unknown, "cannot resolve host \"nocturne-no-such-host.invalid\""));

    uint16_t closed = 0;
    {
        azl::EngineHarness harness({.width = 960, .height = 540});
        azl::WebSocketServer server(harness.document(), 0);
        closed = server.port();
    }
    const std::string refused = connectError("localhost", closed);
    CHECK(mentions(refused, "cannot connect to ws://localhost:" + std::to_string(closed) + "/ ("));
    CHECK(mentions(refused, "127.0.0.1"));
    CHECK(connectError("127.0.0.1", closed) == "cannot connect to ws://127.0.0.1:" + std::to_string(closed) + "/");
}

TEST_CASE("over a real WebSocket, an edit reaches every client and the rendered frame follows")
{
    azl::EngineHarness harness({.width = 960, .height = 540});
    azl::WebSocketServer server(harness.document(), 0);
    Inbox packed, text;
    azl::WebSocketClient a("127.0.0.1", server.port(), azl::Encoding::MessagePack,
                           [&](const Message& m) { packed.push(m); });
    azl::WebSocketClient b("127.0.0.1", server.port(), azl::Encoding::Json, [&](const Message& m) { text.push(m); });
    a.send(msg::hello());
    b.send(msg::hello());
    CHECK(packed.at(0)["type"] == "snapshot");
    CHECK(text.at(0)["type"] == "snapshot");

    a.send(setOpacity(0.5));
    const Message change = text.at(1);
    CHECK(change["change"] == 1);
    CHECK(opacityIn(change) == 0.5);
    CHECK(packed.at(1) == change);
    harness.step();
    checkBrightness(harness, 128, 10);

    b.send(msg::undo());
    CHECK(packed.at(2)["cause"] == "undo");
    CHECK(text.at(2)["change"] == 2);
    harness.step();
    checkBrightness(harness, 255, 20);
}

TEST_CASE("the commands of one gesture make one undo step")
{
    azl::EngineHarness harness({.width = 960, .height = 540});
    InProcessClient a(harness.document());
    InProcessClient b(harness.document());
    auto inGesture = [](const azl::Edit& edit, uint64_t gesture) {
        Message m = msg::command(edit);
        m["gesture"] = gesture;
        return m;
    };

    for (int i = 1; i <= 30; ++i)
        a.send(inGesture(SetLayerOpacity{1, 1.0 - i / 100.0}, 7));
    CHECK(a.inbox.back()["history"]["undo"] == 1);
    CHECK(a.inbox.back()["history"]["undoOp"] == "setLayerOpacity");
    a.send(msg::undo());
    CHECK(opacityIn(a.inbox.back()) == 1.0);
    CHECK(a.inbox.back()["history"] == Message{{"undo", 0}, {"redo", 1}, {"undoOp", ""}, {"redoOp", "setLayerOpacity"}});
    a.send(msg::redo());
    CHECK(opacityIn(a.inbox.back()) == doctest::Approx(0.7));

    // A move is two cells in one gesture: one undo puts the clip back.
    const azl::ClipRef clip{"", "text", {}, 0, std::nullopt, {}, azl::TextClip{.text = "hi"}};
    a.send(msg::command(azl::SetSlot{1, 1, clip}));
    a.send(inGesture(azl::SetSlot{1, 2, clip}, 8));
    a.send(inGesture(azl::SetSlot{1, 1, std::nullopt}, 8));
    CHECK(a.inbox.back()["history"]["undo"] == 3);
    a.send(msg::undo());
    azl::Composition document = harness.document().document();
    CHECK(document.layers[0].clips.contains(1));
    CHECK_FALSE(document.layers[0].clips.contains(2));

    // Another client's edit, or another gesture, starts a new step.
    a.send(inGesture(SetLayerOpacity{1, 0.5}, 9));
    b.send(inGesture(SetLayerOpacity{1, 0.4}, 9));
    a.send(inGesture(SetLayerOpacity{1, 0.3}, 9));
    CHECK(a.inbox.back()["history"]["undo"] == 5);
    a.send(inGesture(SetLayerOpacity{1, 0.2}, 10));
    CHECK(a.inbox.back()["history"]["undo"] == 6);
    a.send({{"type", "command"}, {"op", "setLayerOpacity"}, {"layer", 1}, {"opacity", 0.1}, {"gesture", "x"}});
    CHECK(a.inbox.back()["type"] == "error");
    CHECK(b.inbox.front()["history"]["undo"] == 0);
}

TEST_CASE("a batch applies its edits as one change and one undo step, or none of them")
{
    azl::EngineHarness harness({.width = 960, .height = 540});
    InProcessClient a(harness.document());
    InProcessClient b(harness.document());
    auto layer = [&] { return harness.document().document().layers[0]; };

    a.send(msg::command(azl::Batch{{SetLayerOpacity{1, 0.5}, azl::AddEffect{1, std::nullopt, {0, "colorize", {}}}}}));
    REQUIRE(b.inbox.size() == 2);
    const Message change = b.inbox[1];
    CHECK(change["op"] == "batch");
    CHECK(change["history"]["undo"] == 1);
    CHECK(change["history"]["undoOp"] == "batch");
    const uint64_t fx = change["edits"][1]["effect"]["id"];
    CHECK(fx != 0);
    CHECK(layer().opacity == 0.5);
    REQUIRE(layer().effects.size() == 1);
    CHECK(layer().effects[0].id == fx);
    azl::DocumentMirror mirror;
    for (const Message& m : b.inbox)
        mirror.receive(m);
    CHECK(mirror.composition().layers[0].effects == layer().effects);

    // A batch with an edit that does not apply, or with a live action, changes nothing.
    a.send(msg::command(azl::Batch{{SetLayerOpacity{1, 0.2}, SetLayerOpacity{99, 0.3}}}));
    CHECK(a.inbox.back()["type"] == "error");
    a.send(msg::command(azl::Batch{{SetLayerOpacity{1, 0.2}, azl::SelectDeck{1}}}));
    CHECK(a.inbox.back()["type"] == "error");
    CHECK(b.inbox.size() == 2);
    CHECK(layer().opacity == 0.5);

    a.send(msg::undo());
    CHECK(layer().opacity == 1.0);
    CHECK(layer().effects.empty());
    a.send(msg::redo());
    CHECK(layer().opacity == 0.5);
    REQUIRE(layer().effects.size() == 1);
    CHECK(layer().effects[0].id == fx);

    // Batches setting the same values within a gesture make one step, as single edits do.
    for (int i = 1; i <= 5; ++i) {
        Message m = msg::command(azl::Batch{{SetLayerOpacity{1, i / 10.0}, azl::SetEffectParam{fx, "hue", i / 10.0}}});
        m["gesture"] = uint64_t(3);
        a.send(m);
    }
    CHECK(a.inbox.back()["history"]["undo"] == 2);
    a.send(msg::undo());
    CHECK(layer().opacity == 0.5);
}

TEST_CASE("decode reads MessagePack and JSON up to 64 levels deep and in UTF-8 only")
{
    const auto decoded = [](const std::vector<uint8_t>& bytes, azl::Encoding encoding) {
        return azl::decode(bytes.data(), bytes.size(), encoding).has_value();
    };
    const auto nested = [](size_t depth, azl::Encoding encoding) {
        Message m = Message::array();
        for (size_t i = 1; i < depth; ++i)
            m = Message::array({m});
        return azl::encode({{"type", "x"}, {"deep", m}, {"bin", Message::binary({1, 2, 3}, 9)}, {"n", -1.5}}, encoding);
    };
    for (const azl::Encoding encoding : {azl::Encoding::MessagePack, azl::Encoding::Json}) {
        CHECK(decoded(nested(63, encoding), encoding));
        CHECK_FALSE(decoded(nested(64, encoding), encoding));
    }

    std::vector<uint8_t> deep{0x81, 0xa1, 'a'};
    deep.insert(deep.end(), 500000, 0x91);
    deep.push_back(0xc0);
    CHECK_FALSE(decoded(deep, azl::Encoding::MessagePack));
    const std::string text = "{\"a\":" + std::string(500000, '[') + std::string(500000, ']') + "}";
    CHECK_FALSE(decoded({text.begin(), text.end()}, azl::Encoding::Json));
    CHECK_FALSE(decoded({0x81, 0xa1, 'a', 0xdd, 0xff, 0xff, 0xff, 0xff}, azl::Encoding::MessagePack));

    CHECK(decoded(Message::to_msgpack({{"type", "caf\xc3\xa9"}}), azl::Encoding::MessagePack));
    CHECK_FALSE(decoded(Message::to_msgpack({{"type", "caf\xe9"}}), azl::Encoding::MessagePack));
    CHECK_FALSE(decoded(Message::to_msgpack({{"type", "x"}, {"\xff", 1}}), azl::Encoding::MessagePack));
    CHECK_FALSE(decoded(Message::to_msgpack({{"type", "x"}, {"list", {1, "\xc3"}}}), azl::Encoding::MessagePack));

    const std::vector<uint8_t> json = azl::encode({{"type", "caf\xe9"}}, azl::Encoding::Json);
    CHECK(std::string(json.begin(), json.end()) == "{\"type\":\"caf\xef\xbf\xbd\"}");
}
