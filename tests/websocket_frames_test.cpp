#include "net/websocket_frames.hpp"

#include <doctest.h>

#include <string>
#include <vector>

namespace {

using namespace azl::ws;

std::vector<uint8_t> bytes(const std::string& text)
{
    return {text.begin(), text.end()};
}

std::vector<uint8_t> masked(Opcode opcode, const std::string& payload, bool fin = true)
{
    std::vector<uint8_t> f = frame(opcode, reinterpret_cast<const uint8_t*>(payload.data()), payload.size(),
                                   std::array<uint8_t, 4>{0x12, 0x34, 0x56, 0x78});
    if (!fin)
        f[0] &= 0x7f;
    return f;
}

std::vector<Event> readAll(MessageReader& reader, const std::vector<uint8_t>& stream)
{
    reader.feed(stream.data(), stream.size());
    std::vector<Event> events;
    while (auto event = reader.next())
        events.push_back(*event);
    return events;
}

Head head(const std::string& text)
{
    const std::optional<Head> parsed = parseHead(text);
    REQUIRE(parsed);
    return *parsed;
}

const std::string kRequest = "GET / HTTP/1.1\r\nHost: 127.0.0.1:7400\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n";

} // namespace

TEST_CASE("websocket: SHA-1 and the handshake's accept key match their published examples")
{
    const std::array<uint8_t, 20> abc = sha1("abc", 3);
    CHECK(base64(abc.data(), abc.size()) == "qZk+NkcGgWq6PiVxeFDCbJzQ2J0=");
    CHECK(acceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    CHECK(base64(reinterpret_cast<const uint8_t*>("ab"), 2) == "YWI=");
}

TEST_CASE("websocket: a server upgrades a well-formed handshake and refuses one with an Origin header")
{
    const HandshakeAnswer upgraded = answerHandshake(head(kRequest + "\r\n"));
    CHECK(upgraded.upgraded);
    CHECK(upgraded.response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != std::string::npos);
    CHECK(handshakeAccepted(head(upgraded.response), "dGhlIHNhbXBsZSBub25jZQ=="));
    CHECK_FALSE(handshakeAccepted(head(upgraded.response), "AAAAAAAAAAAAAAAAAAAAAA=="));

    for (const std::string origin : {"Origin: http://example.com\r\n", "origin: null\r\n", "ORIGIN:\r\n"}) {
        CAPTURE(origin);
        const HandshakeAnswer refused = answerHandshake(head(kRequest + origin + "\r\n"));
        CHECK_FALSE(refused.upgraded);
        CHECK(refused.response.starts_with("HTTP/1.1 403"));
    }
    CHECK_FALSE(answerHandshake(head("GET / HTTP/1.1\r\nHost: x\r\n\r\n")).upgraded);
    CHECK_FALSE(answerHandshake(head("POST / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: upgrade\r\n"
                                     "Sec-WebSocket-Key: a\r\nSec-WebSocket-Version: 13\r\n\r\n"))
                    .upgraded);
    CHECK_FALSE(parseHead(kRequest));
    CHECK_FALSE(parseHead("GET / HTTP/1.1\r\nnot a header\r\n\r\n"));
}

TEST_CASE("websocket: fragments join into one message, with a ping answered in between")
{
    MessageReader reader(true, 1 << 20);
    std::vector<uint8_t> stream = masked(Opcode::Text, "hel", false);
    const std::vector<uint8_t> ping = masked(Opcode::Ping, "p");
    const std::vector<uint8_t> rest = masked(Opcode::Continuation, "lo");
    stream.insert(stream.end(), ping.begin(), ping.end());
    stream.insert(stream.end(), rest.begin(), rest.end());

    std::vector<Event> events;
    for (const uint8_t byte : stream) {
        reader.feed(&byte, 1);
        while (auto event = reader.next())
            events.push_back(*event);
    }
    REQUIRE(events.size() == 2);
    CHECK(events[0].kind == Event::Kind::Ping);
    CHECK(events[0].payload == bytes("p"));
    CHECK(events[1].kind == Event::Kind::Text);
    CHECK(events[1].payload == bytes("hello"));
}

TEST_CASE("websocket: a message over the limit is too big as soon as its header says so, before its payload")
{
    MessageReader reader(true, 64 << 10);
    std::vector<uint8_t> big = masked(Opcode::Binary, std::string(65 << 10, 'x'));
    big.resize(14);
    std::vector<Event> events = readAll(reader, big);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == Event::Kind::TooBig);
    CHECK(readAll(reader, masked(Opcode::Text, "after")).empty());

    MessageReader fragments(true, 10);
    std::vector<uint8_t> stream = masked(Opcode::Binary, "123456", false);
    const std::vector<uint8_t> more = masked(Opcode::Continuation, "78901");
    stream.insert(stream.end(), more.begin(), more.end());
    events = readAll(fragments, stream);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == Event::Kind::TooBig);

    MessageReader raised(true, 4);
    raised.setLimit(8);
    events = readAll(raised, masked(Opcode::Binary, "12345678"));
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == Event::Kind::Binary);
}

TEST_CASE("websocket: frames that break the protocol are invalid and end the reading")
{
    const std::string payload = "x";
    const auto server = frame(Opcode::Text, reinterpret_cast<const uint8_t*>(payload.data()), 1);
    const std::vector<std::vector<uint8_t>> broken = {
        server,                                          // unmasked from a client
        masked(Opcode::Continuation, "x"),               // continuation without a start
        masked(Opcode::Ping, "x", false),                // fragmented control frame
        masked(Opcode::Ping, std::string(126, 'x')),     // control frame too long
        {0x83, 0x80, 0, 0, 0, 0},                        // unknown opcode
        {0xc1, 0x80, 0, 0, 0, 0},                        // a reserved bit
        {0x88, 0x81, 0, 0, 0, 0, 1},                     // a close with one byte
        {0x82, 0xff, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, // a 64-bit length with its top bit set
    };
    for (size_t i = 0; i < broken.size(); ++i) {
        CAPTURE(i);
        MessageReader reader(true, 1 << 20);
        std::vector<Event> events = readAll(reader, broken[i]);
        REQUIRE(events.size() == 1);
        CHECK(events[0].kind == Event::Kind::Invalid);
    }
    MessageReader client(false, 1 << 20);
    std::vector<Event> events = readAll(client, server);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == Event::Kind::Text);
}

TEST_CASE("websocket: a close frame carries its status code; lengths of 126 and more use the longer forms")
{
    MessageReader reader(false, 1 << 20);
    std::vector<Event> events = readAll(reader, closeFrame(kClosePolicy));
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == Event::Kind::Close);
    CHECK(events[0].payload == std::vector<uint8_t>{0x03, 0xf0});

    for (const size_t size : {size_t(125), size_t(126), size_t(65535), size_t(65536)}) {
        CAPTURE(size);
        const std::vector<uint8_t> payload(size, 7);
        const std::vector<uint8_t> f = frame(Opcode::Binary, payload.data(), payload.size());
        CHECK(f.size() == size + (size < 126 ? 2 : size <= 65535 ? 4 : 10));
        events = readAll(reader, f);
        REQUIRE(events.size() == 1);
        CHECK(events[0].payload == payload);
    }
}
