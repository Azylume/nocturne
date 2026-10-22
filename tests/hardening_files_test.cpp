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

TEST_SUITE_END();
