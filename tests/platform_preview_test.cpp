#include <doctest.h>

#include "platform/preview_channel.hpp"
#include "platform/process.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using azl::PreviewChannelReader;
using azl::PreviewChannelWriter;

// A BGRA picture whose bytes all say `seed`, plus their position, so a row out of place shows.
std::vector<uint8_t> picture(uint32_t width, uint32_t height, uint8_t seed)
{
    std::vector<uint8_t> bgra(size_t(width) * height * 4);
    for (size_t i = 0; i < bgra.size(); ++i)
        bgra[i] = uint8_t(seed + i / 4 % 251);
    return bgra;
}

std::string readAll(azl::NativeHandle handle)
{
    std::string out;
    char buffer[256];
    for (long n; (n = azl::readHandle(handle, buffer, sizeof buffer)) > 0;)
        out.append(buffer, size_t(n));
    return out;
}

} // namespace

TEST_CASE("platform: a preview picture written by the engine side reads back whole, with its frame")
{
    const std::unique_ptr<PreviewChannelWriter> writer = PreviewChannelWriter::create(37, 21);
    REQUIRE(writer);
    CHECK(writer->width() == 37);
    CHECK(writer->height() == 21);
    CHECK(writer->stride() >= 37 * 4);

    const std::unique_ptr<PreviewChannelReader> reader = PreviewChannelReader::open(writer->id());
    REQUIRE(reader);
    PreviewChannelReader::Picture read;
    CHECK_FALSE(reader->read(read)); // Nothing painted yet.

    const std::vector<uint8_t> first = picture(37, 21, 3);
    writer->write(first.data(), 37 * 4, 5);
    REQUIRE(reader->read(read));
    CHECK(read.width == 37);
    CHECK(read.height == 21);
    CHECK(read.frame == 5u);
    CHECK(read.bgra == first);

    const std::vector<uint8_t> second = picture(37, 21, 99);
    uint8_t* rows = writer->begin();
    CHECK_FALSE(reader->read(read)); // Being painted.
    for (uint32_t y = 0; y < 21; ++y)
        std::copy_n(second.data() + y * 37 * 4, 37 * 4, rows + y * writer->stride());
    writer->finish(6);
    REQUIRE(reader->read(read));
    CHECK(read.frame == 6u);
    CHECK(read.bgra == second);

    CHECK_FALSE(PreviewChannelReader::open(0));
}

TEST_CASE("platform: a preview reader never takes a picture the writer was painting while it copied")
{
    constexpr uint32_t kWidth = 320, kHeight = 180;
    const std::unique_ptr<PreviewChannelWriter> writer = PreviewChannelWriter::create(kWidth, kHeight);
    REQUIRE(writer);
    const std::unique_ptr<PreviewChannelReader> reader = PreviewChannelReader::open(writer->id());
    REQUIRE(reader);

    std::atomic<bool> done{false};
    std::thread painter([&] {
        std::vector<uint8_t> bgra(size_t(kWidth) * kHeight * 4);
        for (int64_t frame = 0; frame < 2000; ++frame) {
            std::fill(bgra.begin(), bgra.end(), uint8_t(frame));
            writer->write(bgra.data(), kWidth * 4, frame);
        }
        done = true;
    });
    size_t taken = 0, torn = 0;
    PreviewChannelReader::Picture read;
    while (!done) {
        if (!reader->read(read))
            continue;
        ++taken;
        const uint8_t expected = uint8_t(*read.frame);
        torn += size_t(std::count_if(read.bgra.begin(), read.bgra.end(), [&](uint8_t b) { return b != expected; }));
    }
    painter.join();
    MESSAGE(taken << " pictures taken while 2000 were painted");
    CHECK(torn == 0);
    REQUIRE(reader->read(read));
    CHECK(read.frame == 1999u);
}

TEST_CASE("platform: another process reads a preview picture by its id")
{
    const std::unique_ptr<PreviewChannelWriter> writer = PreviewChannelWriter::create(64, 32);
    REQUIRE(writer);
    const std::vector<uint8_t> bgra = picture(64, 32, 7);
    writer->write(bgra.data(), 64 * 4, 42);
    uint64_t sum = 0;
    for (uint8_t b : bgra)
        sum += b;

    const std::optional<azl::PipeEnds> pipe = azl::makePipe();
    REQUIRE(pipe);
    std::string error;
    std::optional<azl::Process> child = azl::spawnProcess(
        {.program = NOCTURNE_PLATFORM_CHILD, .args = {"preview", std::to_string(writer->id())},
         .output = azl::Stdio::to(pipe->write)},
        &error);
    azl::closeHandle(pipe->write);
    REQUIRE_MESSAGE(child, error);
    const std::string output = readAll(pipe->read);
    azl::closeHandle(pipe->read);
    CHECK(child->wait() == 0);
    CHECK(output == "64x32 frame 42 sum " + std::to_string(sum));
}
