#include "hap_count.hpp"
#include "test_files.hpp"

#include "media/media_library.hpp"
#include "media/stills.hpp"
#include "media/text_renderer.hpp"

#include <doctest.h>

#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Media work off the render path that every platform does with its own frameworks: hashing, stills and text.
namespace {

void writeFile(const fs::path& file, const std::string& content)
{
    std::ofstream(file, std::ios::binary) << content;
}

// The picture size a baseline JPEG's frame header gives; none when there is no such header.
std::optional<std::pair<int, int>> jpegSize(const std::vector<uint8_t>& jpeg)
{
    if (jpeg.size() < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8 || jpeg[jpeg.size() - 2] != 0xff ||
        jpeg.back() != 0xd9)
        return std::nullopt;
    for (size_t at = 2; at + 9 < jpeg.size();) {
        if (jpeg[at] != 0xff)
            return std::nullopt;
        const uint8_t marker = jpeg[at + 1];
        if (marker == 0xc0 || marker == 0xc1 || marker == 0xc2)
            return std::pair{jpeg[at + 7] << 8 | jpeg[at + 8], jpeg[at + 5] << 8 | jpeg[at + 6]};
        at += 2 + size_t(jpeg[at + 2] << 8 | jpeg[at + 3]);
    }
    return std::nullopt;
}

#ifdef __APPLE__
constexpr const char* kInstalledFont = "Helvetica Bold";

class Device {
public:
    const azl::MediaDevice* get() const { return &device_; }

private:
    azl::MediaDevice device_ = azl::MediaDevice::systemDefault();
};

std::vector<uint8_t> coverage(const azl::VideoFrame& frame)
{
    const auto width = size_t(frame.luma.width()), height = size_t(frame.luma.height());
    std::vector<uint8_t> rg(width * height * 2);
    frame.luma.native()->getBytes(rg.data(), width * 2, MTL::Region(0, 0, width, height), 0);
    return rg;
}
#else
constexpr const char* kInstalledFont = "Arial Bold";

struct Device {
    const azl::MediaDevice* get() const { return nullptr; }
};

std::vector<uint8_t> coverage(const azl::VideoFrame& frame)
{
    REQUIRE(frame.picture);
    const azl::PicturePlane& plane = frame.picture->planes[0];
    CHECK(plane.format == azl::PlaneFormat::RG8);
    std::vector<uint8_t> rg;
    for (int y = 0; y < plane.height; ++y)
        rg.insert(rg.end(), plane.data + size_t(y) * plane.stride,
                  plane.data + size_t(y) * plane.stride + size_t(plane.width) * 2);
    return rg;
}
#endif

// The most coverage of the fill (R) and of the outline (G).
std::pair<int, int> strongest(const std::vector<uint8_t>& rg)
{
    int fill = 0, outline = 0;
    for (size_t i = 0; i < rg.size(); i += 2) {
        fill = std::max(fill, int(rg[i]));
        outline = std::max(outline, int(rg[i + 1]));
    }
    return {fill, outline};
}

} // namespace

TEST_CASE("a file's hash is the hex SHA-256 of its content")
{
    TempDir dir;
    writeFile(dir.path / "abc.txt", "abc");
    writeFile(dir.path / "empty.txt", "");
    CHECK(azl::sha256File(dir.path / "abc.txt") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(azl::sha256File(dir.path / "empty.txt") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK_THROWS_AS(azl::sha256File(dir.path / "missing.txt"), std::runtime_error);
}

TEST_CASE("a still of a video, HAP or not, is a JPEG that fits the width asked for, turned upright")
{
    TempDir dir;
    CHECK(jpegSize(azl::videoStill(mediaDir() / "count_709_tv.mp4", 160)) == std::pair{160, 90});
    CHECK(jpegSize(azl::videoStill(mediaDir() / "count_prores.mov", 160)) == std::pair{160, 90});
    CHECK(jpegSize(azl::videoStill(mediaDir() / "rotated.mp4", 160)) == std::pair{90, 160});
    const fs::path hap = writeHapCount(dir.path / "count.mov", azl::HapCodec::HapQ, 10);
    CHECK(jpegSize(azl::videoStill(hap, 160)) == std::pair{160, 90});
    CHECK(jpegSize(azl::videoStill(mediaDir() / "count_709_tv.mp4", 1000)) == std::pair{320, 180});
    CHECK_THROWS_AS(azl::videoStill(mediaDir() / "audio_only.m4a", 160), std::runtime_error);
}

TEST_CASE("a sampled picture fills its size, cropped to its aspect, as premultiplied BGRA")
{
    TempDir dir;
    const fs::path hap = writeHapCount(dir.path / "count.mov", azl::HapCodec::HapAlpha, 30);
    for (const fs::path& file : {mediaDir() / "count_709_tv.mp4", hap}) {
        INFO(file.filename().string());
        azl::VideoSampler sampler(file, 64, 36, 3);
        for (int i = 0; i < 3; ++i) {
            const std::vector<uint8_t> bgra = sampler.picture(i);
            REQUIRE(bgra.size() == size_t(64) * 36 * 4);
            // The bottom left patch, RGB (200, 40, 40), opaque.
            const uint8_t* p = &bgra[(size_t(30) * 64 + 10) * 4];
            CHECK(std::abs(p[2] - 200) <= 8);
            CHECK(std::abs(p[1] - 40) <= 8);
            CHECK(std::abs(p[0] - 40) <= 8);
            CHECK(p[3] == 255);
        }
    }
    CHECK_THROWS_AS(azl::VideoSampler(mediaDir() / "audio_only.m4a", 64, 36, 3), std::runtime_error);
}

TEST_CASE("a missing font falls back to the system font")
{
    const Device device;
    std::set<std::string> missing;
    const azl::TextLayout layout{.text = "Fallback", .font = "No Such Font 1234", .size = 0.1};
    const azl::VideoFrameRef frame = azl::TextRenderer::rasterise(device.get(), 540, layout, missing);
    REQUIRE(frame);
    REQUIRE(frame->text);
    CHECK(frame->text->bounds[2] > 100.0f);
    CHECK(missing == std::set<std::string>{"No Such Font 1234"});

    missing.clear();
    azl::TextRenderer::rasterise(device.get(), 540, {.text = "Found", .font = kInstalledFont}, missing);
    CHECK(missing.empty());
}

TEST_CASE("a text picture holds the fill's coverage and the outline's, centred on its anchor")
{
    const Device device;
    std::set<std::string> missing;
    const azl::TextLayout plain{.text = "Nocturne\nText", .font = kInstalledFont, .size = 0.1};
    const azl::VideoFrameRef frame = azl::TextRenderer::rasterise(device.get(), 540, plain, missing);
    REQUIRE(frame);
    REQUIRE(frame->text);
    const std::array<float, 4> bounds = frame->text->bounds;
    INFO("bounds " << bounds[0] << ", " << bounds[1] << ", " << bounds[2] << " x " << bounds[3]);
    CHECK(bounds[2] > 150.0f);
    CHECK(bounds[3] > 100.0f);
    CHECK(std::abs(bounds[0] + bounds[2] / 2) < 0.1f * bounds[2]);
    CHECK(std::abs(bounds[1] + bounds[3] / 2) < 0.1f * bounds[3]);
    CHECK(strongest(coverage(*frame)) == std::pair{255, 0});

    azl::TextLayout outlined = plain;
    outlined.outline = 0.1;
    const azl::VideoFrameRef framed = azl::TextRenderer::rasterise(device.get(), 540, outlined, missing);
    REQUIRE(framed);
    CHECK(framed->text->bounds[2] > bounds[2]);
    CHECK(strongest(coverage(*framed)) == std::pair{255, 255});
    CHECK(missing.empty());
}
