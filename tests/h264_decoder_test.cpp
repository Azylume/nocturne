#include <doctest.h>

#include "output/h264_stream.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace rv = azl::return_video;

constexpr uint32_t kWidth = 160, kHeight = 96;
constexpr int kFrames = 12;

fs::path mediaDir()
{
    const char* dir = std::getenv("NOCTURNE_MEDIA_DIR");
    return dir ? dir : NOCTURNE_MEDIA_DIR;
}

// Eight vertical bars, BGRA.
std::vector<uint8_t> bars()
{
    static constexpr uint8_t kColours[8][3] = {{255, 255, 255}, {0, 255, 255}, {255, 255, 0}, {0, 255, 0},
                                               {255, 0, 255},   {0, 0, 255},   {255, 0, 0},   {0, 0, 0}};
    std::vector<uint8_t> bgra(size_t(kWidth) * kHeight * 4);
    for (uint32_t y = 0; y < kHeight; ++y)
        for (uint32_t x = 0; x < kWidth; ++x) {
            const uint8_t* c = kColours[x * 8 / kWidth];
            uint8_t* p = bgra.data() + (size_t(y) * kWidth + x) * 4;
            p[0] = c[0], p[1] = c[1], p[2] = c[2], p[3] = 255;
        }
    return bgra;
}

void writeBytes(std::ofstream& out, const std::vector<uint8_t>& bytes)
{
    const auto size = uint32_t(bytes.size());
    out.write(reinterpret_cast<const char*>(&size), 4);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

std::vector<uint8_t> readBytes(std::ifstream& in)
{
    uint32_t size = 0;
    in.read(reinterpret_cast<char*>(&size), 4);
    std::vector<uint8_t> bytes(size);
    in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size));
    return bytes;
}

// Each packet: key (1 byte), config and data (each a little-endian 32-bit size, then the bytes).
std::vector<rv::Packet> readPackets(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    std::vector<rv::Packet> packets;
    for (char key; in.get(key);) {
        rv::Packet packet;
        packet.key = key != 0;
        packet.config = readBytes(in);
        packet.data = readBytes(in);
        if (in)
            packets.push_back(std::move(packet));
    }
    return packets;
}

#ifdef __APPLE__
void writePackets(const fs::path& file)
{
    azl::H264Encoder encoder(kWidth, kHeight, 30.0, 2000);
    const std::vector<uint8_t> picture = bars();
    std::ofstream out(file, std::ios::binary);
    for (int i = 0; i < kFrames; ++i) {
        CVPixelBufferRef buffer = encoder.newBuffer();
        REQUIRE(buffer);
        CVPixelBufferLockBaseAddress(buffer, 0);
        auto* base = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
        const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
        for (uint32_t y = 0; y < kHeight; ++y)
            std::memcpy(base + y * stride, picture.data() + size_t(y) * kWidth * 4, size_t(kWidth) * 4);
        CVPixelBufferUnlockBaseAddress(buffer, 0);
        const std::optional<rv::Packet> packet = encoder.encode(buffer, i / 30.0, i == 0);
        CVPixelBufferRelease(buffer);
        REQUIRE(packet);
        out.put(packet->key ? 1 : 0);
        writeBytes(out, packet->config);
        writeBytes(out, packet->data);
    }
}
#endif

} // namespace

// The packets in tests/media/return_video_bars.bin were encoded on a Mac by VideoToolbox (H264Encoder), as an engine
// sends its outputs to a UI. On a Mac this rewrites them first:
//   NOCTURNE_RETURN_VIDEO_FIXTURE_OUT=tests/media build/nocturne_tests -tc="return video: packets*"
TEST_CASE("return video: packets a Mac encoded decode to their picture on every platform")
{
    const fs::path file = mediaDir() / "return_video_bars.bin";
#ifdef __APPLE__
    if (const char* out = std::getenv("NOCTURNE_RETURN_VIDEO_FIXTURE_OUT"))
        writePackets(fs::path(out) / file.filename());
#endif
    const std::vector<rv::Packet> packets = readPackets(file);
    REQUIRE(packets.size() == size_t(kFrames));
    REQUIRE(packets.front().key);

    azl::H264Decoder decoder;
    rv::Packet withoutConfig = packets[1];
    withoutConfig.config.clear();
    CHECK_FALSE(decoder.decode(withoutConfig)); // Nothing before a key frame with its parameter sets.

    const std::vector<uint8_t> expected = bars();
    int decoded = 0;
    for (const rv::Packet& packet : packets) {
        const std::optional<azl::H264Decoder::Picture> picture = decoder.decode(packet);
        if (!picture)
            continue;
        ++decoded;
        REQUIRE(picture->width == kWidth);
        REQUIRE(picture->height == kHeight);
        REQUIRE(picture->bgra.size() == expected.size());
        double error = 0;
        for (size_t i = 0; i < expected.size(); ++i)
            if (i % 4 != 3)
                error += std::abs(int(picture->bgra[i]) - int(expected[i]));
        const double meanError = error / double(expected.size() / 4 * 3);
        CHECK(meanError < 8.0);
    }
    CHECK(decoded == kFrames);
}
