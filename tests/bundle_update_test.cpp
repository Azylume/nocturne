#include "temp_folder.hpp"

#include "engine/bundle_update.hpp"
#include "platform/crypto.hpp"
#include "protocol/bundle_push.hpp"

#include <doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using azl::Message;

constexpr const char* kBuild = "20261031120000-abc1234";

// A bundle with an engine binary that carries kBuild, and a payload that packs to more than one part.
fs::path fakeBundle(const fs::path& folder)
{
    const fs::path app = folder / "Build 7.app";
    fs::create_directories(app / "Contents" / "MacOS");
    fs::create_directories(app / "Contents" / "Resources");
    std::ofstream(app / "Contents" / "MacOS" / "nocturne", std::ios::binary)
        << "engine bytes nocturne-build " << kBuild << std::string(1, '\0') << "more bytes";
    std::vector<char> payload(3 << 20);
    azl::randomBytes(payload.data(), payload.size());
    std::ofstream(app / "Contents" / "Resources" / "payload", std::ios::binary)
        .write(payload.data(), std::streamsize(payload.size()));
    return app;
}

std::optional<std::string> noRefusal()
{
    return std::nullopt;
}

} // namespace

TEST_CASE("a bundle is pushed in parts and unpacked as Nocturne.app for the installer")
{
    TempFolder laptop, venue;
    const fs::path app = fakeBundle(laptop.path);
    std::vector<fs::path> installed;
    azl::BundleUpdate update({.incoming = venue.path / "incoming",
                              .install = [&](const fs::path& unpacked) -> std::optional<std::string> {
                                  installed.push_back(unpacked);
                                  return std::nullopt;
                              }});
    azl::BundlePush push(app);
    CHECK(push.build() == kBuild);
    Message reply = update.receive(push.start(), noRefusal);
    int parts = 0;
    while (reply["state"] == "receiving") {
        const std::optional<Message> part = push.next();
        REQUIRE(part);
        reply = update.receive(*part, noRefusal);
        ++parts;
    }
    CHECK(parts > 1);
    CHECK(reply == Message{{"type", "machine"}, {"op", "update"}, {"state", "installing"}, {"build", kBuild}});
    REQUIRE(installed.size() == 1);
    CHECK(installed[0].filename() == "Nocturne.app");
    CHECK(azl::bundleBuildId(installed[0]) == kBuild);
    CHECK(fs::file_size(installed[0] / "Contents" / "Resources" / "payload") ==
          fs::file_size(app / "Contents" / "Resources" / "payload"));
}

#ifdef _WIN32
TEST_CASE("Update says plainly that it is not available on Windows yet, and installs nothing")
{
    TempFolder state;
    azl::BundleUpdate update(azl::installedUpdate(state.path));
    CHECK(update.served());
    REQUIRE(update.refused());
    CHECK(update.refused()->find("Update is not available on Windows yet") != std::string::npos);
    CHECK(update.refused()->find("no signed Windows builds") != std::string::npos);
    const Message reply = update.receive({{"type", "machine"}, {"op", "update"}, {"size", 10}, {"build", kBuild}},
                                         [&] { return update.refused(); });
    CHECK(reply["type"] == "error");
    CHECK(reply["message"] == *update.refused());
}
#endif
