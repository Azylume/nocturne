#include "document/composition.hpp"
#include "effects/isf.hpp"
#include "engine/isf_library.hpp"
#include "import/xml.hpp"
#include "media/hap.hpp"
#include "media/keep_local.hpp"
#include "media/snappy.hpp"
#include "net/pairing.hpp"
#include "net/websocket.hpp"
#include "sync/node_sync.hpp"
#include "test_client.hpp"

#include <doctest.h>

#include <chrono>
#include <fstream>
#include <iterator>
#include <thread>

namespace {

constexpr azl::EngineConfig kSize{.width = 320, .height = 180};

std::string readText(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

class FixedClock : public azl::ClockLink {
public:
    std::optional<double> offsetAt(double) const override { return 0.0; }
};

std::variant<azl::IsfShader, std::string> parsedInputs(const std::string& inputs)
{
    return azl::parseIsf({"Probe", "/*{\"INPUTS\": " + inputs + "}*/\nvoid main() { gl_FragColor = vec4(1.0); }\n",
                          true});
}

std::string errorOf(const std::variant<azl::IsfShader, std::string>& parsed)
{
    const auto* error = std::get_if<std::string>(&parsed);
    return error ? *error : std::string();
}

std::string nested(const char* open, const char* inner, const char* close, int depth)
{
    std::string text;
    for (int i = 0; i < depth; ++i)
        text += open;
    text += inner;
    for (int i = 0; i < depth; ++i)
        text += close;
    return text;
}

} // namespace

TEST_SUITE_BEGIN("hardening files");

TEST_CASE("a still or thumbnail asked for by anything but a content hash names no file")
{
    TempDir dir;
    const fs::path secret = dir.path / "secret";
    std::ofstream(secret) << "private key";
    azl::EngineHarness harness(kSize, 60.0, dir.path / "library");
    Client client(harness.document(), false);
    for (const std::string& hash : {secret.string() + std::string(1, '\0'), std::string("../../secret"),
                                    secret.string()}) {
        for (const char* op : {"still", "thumbnail"}) {
            client.send({{"type", "library"}, {"op", op}, {"hash", hash}});
            Message reply = client.next();
            while (reply.value("type", "") != "library")
                reply = client.next();
            CHECK(reply.contains("error"));
            CHECK_FALSE(reply.contains("jpeg"));
        }
    }
}

TEST_CASE("a clip's hash is a content hash or empty when it comes in as JSON")
{
    const std::string hash(64, 'a');
    CHECK(Message{{"hash", hash}, {"name", "a.mp4"}}.get<azl::ClipRef>().hash == hash);
    CHECK(Message{{"hash", ""}, {"name", "Hello"}}.get<azl::ClipRef>().hash.empty());
    for (const std::string& bad :
         {std::string("/etc/passwd"), std::string("../media/x"), std::string(64, 'A'), hash + "/x"})
        CHECK_THROWS_AS(Message({{"hash", bad}, {"name", "a.mp4"}}).get<azl::ClipRef>(), azl::ProtocolError);
}

TEST_CASE("media files named by hash keep only a plain extension of the clip's name")
{
    CHECK(azl::mediaExtension("clip.mp4") == ".mp4");
    CHECK(azl::mediaExtension("Clip.MOV") == ".MOV");
    CHECK(azl::mediaExtension("clip").empty());
    CHECK(azl::mediaExtension("a.mp4/../../x").empty());
    CHECK(azl::mediaExtension("a." + std::string(12, 'm')).empty());
    CHECK(azl::mediaExtension(std::string("a.mp4\0x", 7)).empty());
    CHECK(azl::mediaExtension("a.m p4").empty());
}

TEST_CASE("a node never makes, truncates or removes a file named by a rogue main's clip hash")
{
    TempDir dir;
    const fs::path victim = dir.path / "victim";
    std::ofstream(victim) << "keep me";
    std::ofstream(victim.string() + ".part") << "keep me too";
    azl::Composition rogue = azl::defaultComposition();
    rogue.layers[0].clips[1] = azl::ClipRef{victim.string(), "victim"};
    azl::EngineHarness main(kSize, 30.0, dir.path / "main", rogue);
    azl::SyncRole role(main.document(), main.engine(), main.library(), [&] { return main.time(); },
                       {.role = azl::Role::Main, .name = "main"});
    azl::WebSocketServer server(main.document(), 0);
    const uint16_t port = server.port();

    azl::EngineHarness follower(kSize, 30.0, dir.path / "node");
    azl::NodeSync node(
        follower.document(), follower.engine(), follower.library(), [&] { return follower.time(); },
        [&]() -> std::optional<azl::Endpoint> { return azl::Endpoint{"main", "127.0.0.1", port}; },
        [](const azl::Endpoint&, uint16_t) { return std::make_unique<FixedClock>(); });
    std::this_thread::sleep_for(std::chrono::seconds(2));
    CHECK(node.status().files == 0);
    CHECK(readText(victim) == "keep me");
    CHECK(readText(victim.string() + ".part") == "keep me too");
}

TEST_CASE("a node presents its main's pairing token only to a main it was told to follow, or on this machine")
{
    CHECK(azl::presentsToken(true, "mini.local"));
    CHECK(azl::presentsToken(true, "192.168.1.20"));
    CHECK_FALSE(azl::presentsToken(false, "mini.local"));
    CHECK_FALSE(azl::presentsToken(false, "192.168.1.20"));
    CHECK(azl::presentsToken(false, "127.0.0.1"));
    CHECK(azl::presentsToken(false, "::1"));
    CHECK(azl::presentsToken(false, "localhost"));
}

TEST_CASE("layers nested past one group level are refused without reading them, however deep")
{
    const Message group = Message::parse(R"({"group": true, "layers": [{"name": "a"}, {"name": "b"}]})");
    const azl::Layer parsed = group.get<azl::Layer>();
    REQUIRE(parsed.layers.size() == 2);
    CHECK(parsed.layers[1].name == "b");

    const Message three = Message::parse(R"({"group": true, "layers": [{"group": true, "layers": [{}]}]})");
    CHECK_THROWS_AS(three.get<azl::Layer>(), azl::ProtocolError);
    const Message deep = Message::parse(nested(R"({"group": true, "layers": [)", "{}", "]}", 200000));
    CHECK_THROWS_AS(deep.get<azl::Layer>(), azl::ProtocolError);
    CHECK_THROWS_AS(Message::parse(R"({"layers": 3})").get<azl::Layer>(), azl::ProtocolError);
}

TEST_CASE("a HAP frame of an impossible size is refused before anything is allocated for it")
{
    std::vector<azl::HapImage> images;
    const std::vector<uint8_t> empty{0, 0, 0, 0xab, 0, 0, 0, 0};
    for (const auto& [w, h] : std::vector<std::pair<int, int>>{{-4, 4}, {4, -4}, {0, 4}, {20000, 4}, {4, 65535}})
        CHECK_FALSE(azl::decodeHapFrame(empty.data(), empty.size(), w, h, images).empty());
    CHECK(azl::pictureSizeOk(16384, 16384));
    CHECK_FALSE(azl::pictureSizeOk(16385, 16));

    const std::vector<uint8_t> rgba(size_t(16) * 16 * 4, 128);
    const std::vector<uint8_t> frame = azl::encodeHapFrame(rgba.data(), 16, 16, azl::HapCodec::Hap);
    CHECK(azl::decodeHapFrame(frame.data(), frame.size(), 16, 16, images).empty());
    CHECK_FALSE(azl::decodeHapFrame(frame.data(), frame.size(), 16000, 16000, images).empty());
}

TEST_CASE("a Snappy block declaring more than its bytes can hold has no length")
{
    const std::vector<uint8_t> claims{0xff, 0xff, 0xff, 0xff, 0x0f, 0x00};
    CHECK_FALSE(azl::snappyLength(claims.data(), claims.size()));
    std::vector<uint8_t> zeros(1 << 20, 0);
    const std::vector<uint8_t> packed = azl::snappyCompress(zeros.data(), zeros.size());
    CHECK(azl::snappyLength(packed.data(), packed.size()) == zeros.size());
}

TEST_CASE("malformed ISF inputs are an error of that shader, never an exception or a huge allocation")
{
    CHECK_FALSE(errorOf(parsedInputs("[5]")).empty());
    CHECK_FALSE(errorOf(parsedInputs(R"([{"NAME": 3, "TYPE": "float"}])")).empty());
    CHECK_FALSE(errorOf(parsedInputs(R"([{"NAME": "a", "TYPE": ["float"]}])")).empty());
    CHECK_FALSE(errorOf(parsedInputs(R"(["inputImage"])")).empty());
    CHECK(errorOf(parsedInputs(R"([{"NAME": "a", "TYPE": "float", "LABEL": 7}])")).empty());
    CHECK_FALSE(errorOf(parsedInputs(R"([{"NAME": "a", "TYPE": "long", "MIN": -2147483648, "MAX": 2147483647}])"))
                    .empty());
    CHECK_FALSE(errorOf(parsedInputs(R"([{"NAME": "a", "TYPE": "long", "MIN": 0, "MAX": 1e300}])")).empty());
    CHECK_FALSE(errorOf(parsedInputs(R"([{"NAME": "a", "TYPE": "long", "MIN": 0, "MAX": 256}])")).empty());
    CHECK(errorOf(parsedInputs(R"([{"NAME": "a", "TYPE": "long", "MIN": 0, "MAX": 255, "DEFAULT": 1e300}])"))
              .find("range") != std::string::npos);
    CHECK(errorOf(parsedInputs(R"([{"NAME": "a", "TYPE": "long", "MIN": 0, "MAX": 255}])")).empty());
}

TEST_CASE("one malformed shader in the user ISF folder is listed with its error; the others load")
{
    TempDir dir;
    std::ofstream(dir.path / "bad.fs") << R"(/*{"INPUTS": [{"NAME": {"x": 1}, "TYPE": "float"}, 7]}*/ void main() {})";
    std::ofstream(dir.path / "good.fs")
        << "/*{\"INPUTS\": []}*/\nvoid main() { gl_FragColor = vec4(isf_FragNormCoord, 0.0, 1.0); }\n";
    azl::UserIsfLibrary library(dir.path, dir.path / "cache");
    const std::vector<azl::UserIsfShader> shaders = library.shaders();
    REQUIRE(shaders.size() == 2);
    CHECK_FALSE(shaders[0].error.empty());
    CHECK(shaders[1].error.empty());
}

TEST_CASE("the pointwise checks read a shader of the largest file size on a thread's small stack")
{
    const size_t n = azl::UserIsfLibrary::kMaxFileBytes - 400;
    const std::string effect = R"(/*{"INPUTS": [{"NAME": "inputImage", "TYPE": "image"}]}*/)"
                               "void main() { gl_FragColor = IMG_THIS_PIXEL(" +
                               std::string(n, ' ') + "inputImage); }\n";
    const std::string transition =
        R"(/*{"INPUTS": [{"NAME": "startImage", "TYPE": "image"}, {"NAME": "endImage", "TYPE": "image"}]}*/)"
        "vec4 transition(vec2 uv) { return getFromColor(" +
        std::string(n, 'a') + ") + getToColor(uv); }\n";
    std::variant<azl::IsfShader, std::string> parsedEffect, parsedTransition;
    std::thread([&] {
        parsedEffect = azl::parseIsf({"Long", effect, true});
        parsedTransition = azl::parseIsf({"Long", transition, true});
    }).join();
    REQUIRE(std::holds_alternative<azl::IsfShader>(parsedEffect));
    CHECK(std::get<azl::IsfShader>(parsedEffect).pointwise);
    CHECK(std::holds_alternative<std::string>(parsedTransition));
}

TEST_CASE("XML nested deeper than 256 elements is malformed, however deep")
{
    CHECK(azl::parseXml(nested("<a>", "", "</a>", 256))->children.size() == 1);
    CHECK_THROWS_AS(azl::parseXml(nested("<a>", "", "</a>", 257)), std::runtime_error);
    CHECK_THROWS_AS(azl::parseXml(nested("<a>", "", "</a>", 200000)), std::runtime_error);
}

TEST_CASE("XML character references that are no character are malformed")
{
    CHECK(azl::parseXml(R"(<a b="&#65;&#x42;"/>)")->attr("b") == "AB");
    for (const char* bad : {R"(<a b="&#;"/>)", R"(<a b="&#x;"/>)", R"(<a b="&#99999999999999999999;"/>)",
                            R"(<a b="&#x110000;"/>)", R"(<a b="&#12a;"/>)"})
        CHECK_THROWS_AS(azl::parseXml(bad), std::runtime_error);
}

TEST_SUITE_END();
