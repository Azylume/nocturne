#include "golden.hpp"
#include "hap_clips.hpp"

#include "media/media_library.hpp"

#include <fstream>

namespace {

const char* codecName(azl::HapCodec codec)
{
    switch (codec) {
    case azl::HapCodec::Hap: return "hap";
    case azl::HapCodec::HapAlpha: return "hap_alpha";
    case azl::HapCodec::HapQ: return "hap_q";
    case azl::HapCodec::HapQAlpha: return "hap_q_alpha";
    case azl::HapCodec::None: break;
    }
    return "";
}

// Where tests/media/make_hap_goldens.sh wants the fixtures, to decode them with ffmpeg; else a temporary folder.
fs::path fixtureDir(const TempDir& temp)
{
    const char* dir = std::getenv("NOCTURNE_HAP_FIXTURE_DIR");
    return dir ? fs::path(dir) : temp.path;
}

// Overwrites the movie's sample data from `from` to `to` (fractions of the file) with a pattern, leaving its index.
void corrupt(const fs::path& file, double from, double to)
{
    std::fstream io(file, std::ios::in | std::ios::out | std::ios::binary);
    const auto size = double(fs::file_size(file));
    io.seekp(std::streamoff(size * from));
    for (auto at = std::streamoff(size * from); at < std::streamoff(size * to); ++at)
        io.put(char(0xa5 ^ (at & 0xff)));
}

} // namespace

// The goldens are ffmpeg's own HAP decoder's pictures of these fixtures (tests/media/make_hap_goldens.sh), composited
// over black: the engine's GPU decode matches an independent reference decode.
TEST_CASE("one frame of each HAP variant matches the reference decode")
{
    TempDir temp;
    for (const azl::HapCodec codec : kHapCodecs)
        writeHapCount(fixtureDir(temp) / (std::string(codecName(codec)) + ".mov"), codec, 30, 2);
    for (const azl::HapCodec codec : kHapCodecs) {
        CAPTURE(codecName(codec));
        const fs::path file = fixtureDir(temp) / (std::string(codecName(codec)) + ".mov");
        azl::EngineHarness harness({.width = kHapWidth, .height = kHapHeight}, 30.0);
        Client client(harness.document());
        playMedia(client, file, {azl::SyncMode::Timeline, azl::PlayMode::Loop, 0});
        harness.step(8);
        checkGolden(harness.readScreen(), std::string("hap_") + codecName(codec) + "_frame7");
    }
}

TEST_CASE("a HAP file whose frames are all corrupt is flagged with the reason; one bad frame only drops that frame")
{
    TempDir media;
    azl::EngineHarness harness({.width = 960, .height = 540}, 30.0);
    Client client(harness.document());

    const fs::path broken = writeHapCount(media.path / "broken.mov", azl::HapCodec::HapQ, 30);
    corrupt(broken, 0.001, 0.97);
    const Message added = addMedia(client, broken);
    REQUIRE(added["type"] == "library");
    CHECK(added["entry"]["status"] == "corrupt");
    CHECK(added["entry"]["reason"].get<std::string>().find("HAP frame") != std::string::npos);

    // Frames read fine until one does not decode; that one is counted as an error and the clip plays on.
    const fs::path file = writeHapCount(media.path / "count.mov", azl::HapCodec::Hap, 60);
    const auto frames = azl::readHapFrames(file, 60);
    const double size = double(fs::file_size(file));
    std::vector<uint8_t> bytes(fs::file_size(file));
    std::ifstream(file, std::ios::binary).read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    const auto at = std::search(bytes.begin(), bytes.end(), frames[20].begin(), frames[20].end());
    REQUIRE(at != bytes.end());
    const double start = double(at - bytes.begin()) + 8;
    corrupt(file, start / size, (start + double(frames[20].size()) - 16) / size);

    playMedia(client, file, {azl::SyncMode::Timeline, azl::PlayMode::Loop, 0});
    CHECK(frameAfterStep(harness) == 0);
    CHECK(frameAfterStep(harness, 18) == 18);
    CHECK(frameAfterStep(harness) == 19);
    harness.step();
    CHECK(frameAfterStep(harness) == 21);
    CHECK(frameAfterStep(harness, 10) == 31);
    CHECK(harness.decodeStats().errors >= 1);
    checkHapPatches(harness.readScreen(), azl::HapCodec::Hap);
}
