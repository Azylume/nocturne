#include "hap_count.hpp"
#include "test_files.hpp"

#include "media/hap.hpp"
#include "media/media_library.hpp"
#include "media/video_decoder.hpp"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

// The decoders of every platform (AVFoundation on Apple, FFmpeg elsewhere) against the same clips and the same
// expectations: the count clips' bar code and colour patches (tests/media/make_media.sh), read off the decoded
// pictures on the CPU.
namespace {

// Straight-alpha RGBA.
struct Rgba {
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;

    const uint8_t* at(int x, int y) const { return &pixels[(size_t(y) * width + x) * 4]; }
};

// The decoded planes of a Y'CbCr picture at 8 bits, or a HAP picture's textures.
struct Planes {
    std::vector<uint8_t> luma;
    std::vector<uint8_t> chroma;
    int width = 0;
    int height = 0;
    std::vector<azl::HapImage> hap;
};

azl::HapTexture hapTextureOf(azl::PictureFormat format, int plane)
{
    if (plane == 1)
        return azl::HapTexture::BC4;
    switch (format) {
    case azl::PictureFormat::Rgb: return azl::HapTexture::BC1;
    case azl::PictureFormat::Rgba: return azl::HapTexture::BC3;
    default: return azl::HapTexture::YCoCgBC3;
    }
}

#ifdef __APPLE__
std::vector<uint8_t> textureBytes(MTL::Texture* texture, size_t rowBytes, size_t rows)
{
    std::vector<uint8_t> bytes(rowBytes * rows);
    texture->getBytes(bytes.data(), rowBytes, MTL::Region(0, 0, texture->width(), texture->height()), 0);
    return bytes;
}

Planes planesOf(const azl::VideoFrame& frame, int width, int height)
{
    Planes p{{}, {}, width, height, {}};
    if (frame.format == azl::PictureFormat::YCbCr) {
        p.luma = textureBytes(frame.luma, size_t(width), size_t(height));
        p.chroma = textureBytes(frame.chroma, size_t((width + 1) / 2) * 2, size_t((height + 1) / 2));
        return p;
    }
    const size_t rows = size_t(height + 3) / 4;
    const int planes = frame.format == azl::PictureFormat::YCoCgAlpha ? 2 : 1;
    for (int i = 0; i < planes; ++i) {
        const azl::HapTexture texture = hapTextureOf(frame.format, i);
        const size_t bytes = azl::hapTextureBytes(texture, width, height);
        p.hap.push_back({texture, textureBytes(i == 0 ? frame.luma : frame.chroma, bytes / rows, rows)});
    }
    return p;
}
#else
// Rows of `rowBytes` from a plane, keeping the high byte of 16-bit texels.
std::vector<uint8_t> planeBytes(const azl::PicturePlane& plane, size_t rowBytes)
{
    const bool deep = plane.format == azl::PlaneFormat::R16 || plane.format == azl::PlaneFormat::RG16;
    std::vector<uint8_t> bytes(rowBytes * size_t(plane.height));
    for (int y = 0; y < plane.height; ++y)
        for (size_t x = 0; x < rowBytes; ++x)
            bytes[size_t(y) * rowBytes + x] =
                deep ? plane.data[size_t(y) * plane.stride + x * 2 + 1] : plane.data[size_t(y) * plane.stride + x];
    return bytes;
}

Planes planesOf(const azl::VideoFrame& frame, int width, int height)
{
    Planes p{{}, {}, width, height, {}};
    REQUIRE(frame.picture);
    const azl::DecodedPicture& picture = *frame.picture;
    if (frame.format == azl::PictureFormat::YCbCr) {
        REQUIRE(picture.planeCount == 2);
        p.luma = planeBytes(picture.planes[0], size_t(width));
        p.chroma = planeBytes(picture.planes[1], size_t((width + 1) / 2) * 2);
        return p;
    }
    const size_t rows = size_t(height + 3) / 4;
    for (int i = 0; i < picture.planeCount; ++i) {
        const azl::PicturePlane& plane = picture.planes[size_t(i)];
        const azl::HapTexture texture = hapTextureOf(frame.format, i);
        const size_t bytes = azl::hapTextureBytes(texture, width, height);
        REQUIRE(plane.stride * rows == bytes);
        p.hap.push_back({texture, std::vector<uint8_t>(plane.data, plane.data + bytes)});
    }
    return p;
}
#endif

uint8_t unorm(float v)
{
    return uint8_t(std::clamp(std::lround(v * 255.0f), 0L, 255L));
}

Rgba rgbaOf(const azl::VideoFrame& frame, int width, int height)
{
    const Planes p = planesOf(frame, width, height);
    if (!p.hap.empty())
        return {azl::hapToRgba(p.hap, width, height), width, height};
    Rgba out{std::vector<uint8_t>(size_t(width) * height * 4), width, height};
    const std::array<float, 12>& m = frame.yuvToRgb;
    const int chromaWidth = (width + 1) / 2;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const float luma = p.luma[size_t(y) * width + x] / 255.0f;
            const uint8_t* c = &p.chroma[(size_t(y / 2) * chromaWidth + x / 2) * 2];
            const float cb = c[0] / 255.0f, cr = c[1] / 255.0f;
            uint8_t* o = &out.pixels[(size_t(y) * width + x) * 4];
            for (int i = 0; i < 3; ++i)
                o[i] = unorm(m[size_t(i) * 4] * luma + m[size_t(i) * 4 + 1] * cb + m[size_t(i) * 4 + 2] * cr +
                             m[size_t(i) * 4 + 3]);
            o[3] = 255;
        }
    return out;
}

// The frame index in the bar code: 8 bits, 40 pixels each, across the top half. Fails unless every bit is clearly
// black or white.
int barCode(const Rgba& image)
{
    int index = 0;
    for (int bit = 0; bit < 8; ++bit) {
        const uint8_t* p = image.at(20 + 40 * bit, 45);
        CHECK_MESSAGE((p[0] < 24 || p[0] > 231), "bit " << bit << " is not black or white: " << int(p[0]));
        index = index * 2 + (p[0] > 128);
    }
    return index;
}

void checkColour(const Rgba& image, int x, int y, int r, int g, int b)
{
    const uint8_t* p = image.at(x, y);
    INFO("pixel " << x << "," << y << " is " << int(p[0]) << "," << int(p[1]) << "," << int(p[2]));
    CHECK(std::abs(p[0] - r) <= 4);
    CHECK(std::abs(p[1] - g) <= 4);
    CHECK(std::abs(p[2] - b) <= 4);
}

class Device {
public:
#ifdef __APPLE__
    Device() : device_(MTL::CreateSystemDefaultDevice()) {}
    ~Device() { device_->release(); }
    azl::MediaDevice* get() const { return device_; }

private:
    MTL::Device* device_;
#else
    azl::MediaDevice* get() const { return nullptr; }
#endif
};

struct Decoded {
    int shown = -1;
    Rgba picture;
};

Decoded decodeAt(azl::VideoDecoder& decoder, int index, bool forward = true)
{
    const azl::VideoFrameRef frame = decoder.waitFrame(index, forward);
    REQUIRE(frame);
    CHECK(frame->index == index);
    Decoded d;
    d.picture = rgbaOf(*frame, 320, 180);
    d.shown = barCode(d.picture);
    return d;
}

void checkCountClip(const fs::path& file, bool alpha, int frames = 60)
{
    INFO(file.filename().string());
    const Device device;
    azl::VideoDecoder decoder(device.get(), file.string(), {});
    REQUIRE(decoder.length() == azl::ClipLength{frames, 30.0});
    for (int i = 0; i < frames; ++i) {
        const Decoded d = decodeAt(decoder, i);
        CHECK(d.shown == i);
        checkColour(d.picture, 80, 135, 200, 40, 40);
        checkColour(d.picture, 240, 135, 40, 160, 80);
        CHECK(std::abs(d.picture.at(240, 135)[3] - (alpha ? 128 : 255)) <= 4);
    }
    CHECK(azl::probeMedia(file).status == azl::MediaStatus::Ok);
}

} // namespace

TEST_CASE("H.264, HEVC and ProRes clips decode every frame with its bar code and colours, in both ranges")
{
    for (const char* name : {"count_709_tv.mp4", "count_601_pc.mp4", "count_hevc.mp4", "count_prores.mov"})
        checkCountClip(mediaDir() / name, false);
}

TEST_CASE("every HAP variant, plain and chunked, decodes every frame with its bar code and colours")
{
    TempDir dir;
    for (const azl::HapCodec codec : kHapCodecs)
        for (const int chunks : {1, 4})
            checkCountClip(writeHapCount(dir.path / ("count" + std::to_string(int(codec)) + "_" +
                                                     std::to_string(chunks) + ".mov"),
                                         codec, 60, chunks),
                           hasAlpha(codec));
}

// The committed movies were written by each platform's own container code, AVAssetWriter on macOS and the FFmpeg
// muxer on Windows: 12 count frames of HAP Q Alpha in 2 chunks. On a platform, this rewrites its own movie first:
//   NOCTURNE_HAP_FIXTURE_OUT=tests/media build/nocturne_tests -tc="HAP movies written on*"
TEST_CASE("HAP movies written on macOS and on Windows decode alike on every platform")
{
#ifdef __APPLE__
    constexpr const char* kOwnMovie = "hap_q_alpha_macos.mov";
#else
    constexpr const char* kOwnMovie = "hap_q_alpha_windows.mov";
#endif
    if (const char* out = std::getenv("NOCTURNE_HAP_FIXTURE_OUT"))
        writeHapCount(fs::path(out) / kOwnMovie, azl::HapCodec::HapQAlpha, 12, 2);
    for (const char* name : {"hap_q_alpha_macos.mov", "hap_q_alpha_windows.mov"}) {
        const fs::path file = mediaDir() / name;
        CHECK(azl::hapCodecOfFile(file) == azl::HapCodec::HapQAlpha);
        CHECK(azl::readHapFrames(file, 20).size() == 12);
        checkCountClip(file, true, 12);
    }
}

TEST_CASE("a clip plays backward through its GOPs, seeks anywhere and wraps from its last frame to its first")
{
    const Device device;
    azl::VideoDecoder decoder(device.get(), (mediaDir() / "count_100.mp4").string(), {});
    REQUIRE(decoder.length() == azl::ClipLength{100, 30.0});
    for (int i = 99; i >= 0; i -= 3)
        CHECK(decodeAt(decoder, i, false).shown == i);
    for (const int i : {70, 10, 45, 29, 30, 31, 89, 60})
        CHECK(decodeAt(decoder, i).shown == i);
    for (const int i : {96, 97, 98, 99, 0, 1, 2})
        CHECK(decodeAt(decoder, i).shown == i);
}

TEST_CASE("a hole in a stream shows the next frame, and a variable frame rate stream plays in order")
{
    const Device device;
    auto playAll = [&](const char* name) {
        azl::VideoDecoder decoder(device.get(), (mediaDir() / name).string(), {});
        std::vector<int> shown;
        for (int i = 0; i < decoder.length().frames; ++i)
            shown.push_back(decodeAt(decoder, i).shown);
        CHECK(std::is_sorted(shown.begin(), shown.end()));
        CHECK(shown.back() > 50);
        return shown;
    };
    // Frames 10 to 19 are missing from the gapped clip; its frame rate comes from the frames it has.
    const std::vector<int> gap = playAll("count_gap.mp4");
    const auto nine = std::find(gap.begin(), gap.end(), 9);
    REQUIRE(nine != gap.end());
    const auto next = std::find_if(nine, gap.end(), [](int shown) { return shown != 9; });
    REQUIRE(next != gap.end());
    CHECK(*next == 20);
    playAll("count_vfr.mp4");
}

TEST_CASE("a clip's rotation metadata comes with its frames as quarter turns")
{
    const Device device;
    azl::VideoDecoder rotated(device.get(), (mediaDir() / "rotated.mp4").string(), {});
    azl::VideoDecoder stored(device.get(), (mediaDir() / "count_709_tv.mp4").string(), {});
    const azl::VideoFrameRef turned = rotated.waitFrame(0, true);
    const azl::VideoFrameRef plain = stored.waitFrame(0, true);
    REQUIRE(turned);
    REQUIRE(plain);
    CHECK(turned->turns == 1);
    CHECK(plain->turns == 0);
}

TEST_CASE("probing flags truncated files as corrupt and files without video as unsupported")
{
    CHECK(azl::probeMedia(mediaDir() / "truncated.mp4").status == azl::MediaStatus::Corrupt);
    CHECK(azl::probeMedia(mediaDir() / "truncated_faststart.mp4").status == azl::MediaStatus::Corrupt);
    const azl::ProbeResult audio = azl::probeMedia(mediaDir() / "audio_only.m4a");
    CHECK(audio.status == azl::MediaStatus::Unsupported);
    CHECK_FALSE(audio.reason.empty());
}
