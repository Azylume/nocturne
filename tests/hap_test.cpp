#include "test_client.hpp"

#include "media/hap_movie.hpp"
#include "media/media_library.hpp"

namespace {

std::vector<uint8_t> payload(int frame, size_t size)
{
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i)
        bytes[i] = uint8_t(frame * 31 + i * 7);
    return bytes;
}

} // namespace

TEST_CASE("HAP frames written into a movie come back from its track as stored; probing decodes the first one")
{
    TempDir dir;
    for (const azl::HapCodec codec :
         {azl::HapCodec::Hap, azl::HapCodec::HapAlpha, azl::HapCodec::HapQ, azl::HapCodec::HapQAlpha}) {
        INFO("codec " << int(codec));
        CHECK(azl::hapCodecOf(azl::fourccOf(codec)) == codec);
        const fs::path file = dir.path / ("clip" + std::to_string(int(codec)) + ".mov");
        azl::HapMovieWriter writer(file, codec, 64, 32, 30, 1);
        for (int i = 0; i < 5; ++i)
            writer.append(payload(i, 100 + size_t(i)));
        writer.finish();

        const auto frames = azl::readHapFrames(file, 10);
        REQUIRE(frames.size() == 5);
        for (int i = 0; i < 5; ++i)
            CHECK(frames[size_t(i)] == payload(i, 100 + size_t(i)));
        const azl::ProbeResult probe = azl::probeMedia(file);
        CHECK(probe.status == azl::MediaStatus::Corrupt);
        CHECK(probe.reason.find("HAP frame") != std::string::npos);
    }
    CHECK(azl::hapCodecOf(0x61766331) == azl::HapCodec::None);
}
