#include "media/hap.hpp"
#include "media/snappy.hpp"

#ifdef __APPLE__
#include "media/hap_texture.hpp"
#endif

#include <doctest.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <random>

namespace {

// Smooth gradients with a hard edge and a soft alpha ramp: what BC compression meets in real footage.
std::vector<uint8_t> testPicture(int width, int height)
{
    std::vector<uint8_t> rgba(size_t(width) * height * 4);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            uint8_t* p = &rgba[(size_t(y) * width + x) * 4];
            const bool left = x < width / 2;
            p[0] = uint8_t(left ? 200 : 40 + x * 100 / width);
            p[1] = uint8_t(left ? 40 + y * 60 / height : 160);
            p[2] = uint8_t(left ? 40 : 80);
            p[3] = uint8_t(y * 255 / std::max(1, height - 1));
        }
    return rgba;
}

double meanError(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int channels)
{
    double sum = 0;
    size_t n = 0;
    for (size_t i = 0; i < a.size(); i += 4)
        for (int c = 0; c < channels; ++c, ++n)
            sum += std::abs(int(a[i + size_t(c)]) - int(b[i + size_t(c)]));
    return sum / double(n);
}

} // namespace

TEST_CASE("Snappy round-trips literals, short and long copies and overlapping runs")
{
    std::mt19937 random(7);
    std::vector<uint8_t> data;
    for (int i = 0; i < 3000; ++i)
        data.push_back(uint8_t(random()));
    for (int i = 0; i < 200000; ++i)
        data.push_back(uint8_t(i % 3));
    for (int i = 0; i < 70000; ++i)
        data.push_back(data[size_t(i)]);
    for (const size_t size : {size_t(0), size_t(1), size_t(5), data.size()}) {
        const std::vector<uint8_t> packed = azl::snappyCompress(data.data(), size);
        REQUIRE(azl::snappyLength(packed.data(), packed.size()) == size);
        std::vector<uint8_t> out(size);
        REQUIRE(azl::snappyDecompress(packed.data(), packed.size(), out.data(), size));
        CHECK(std::equal(out.begin(), out.end(), data.begin()));
    }
    const std::vector<uint8_t> packed = azl::snappyCompress(data.data(), data.size());
    CHECK(packed.size() < data.size() / 4);

    std::vector<uint8_t> out(data.size());
    CHECK_FALSE(azl::snappyDecompress(packed.data(), packed.size() - 1, out.data(), out.size()));
    CHECK_FALSE(azl::snappyDecompress(packed.data(), packed.size(), out.data(), out.size() - 1));
    const uint8_t badOffset[] = {8, 0x01, 0x00};
    std::vector<uint8_t> small(8);
    CHECK_FALSE(azl::snappyDecompress(badOffset, sizeof(badOffset), small.data(), small.size()));
}

TEST_CASE("HAP frames of every variant decode to their textures, plain and chunked, close to the source picture")
{
    const int width = 66, height = 34;
    const std::vector<uint8_t> source = testPicture(width, height);
    struct Variant {
        azl::HapCodec codec;
        std::vector<azl::HapTexture> textures;
        int channels;
        double tolerance;
    };
    for (const Variant& v : {Variant{azl::HapCodec::Hap, {azl::HapTexture::BC1}, 3, 3.0},
                             Variant{azl::HapCodec::HapAlpha, {azl::HapTexture::BC3}, 4, 3.0},
                             Variant{azl::HapCodec::HapQ, {azl::HapTexture::YCoCgBC3}, 3, 3.0},
                             Variant{azl::HapCodec::HapQAlpha, {azl::HapTexture::YCoCgBC3, azl::HapTexture::BC4}, 4,
                                     3.0}}) {
        for (const int chunks : {1, 4}) {
            INFO("codec " << int(v.codec) << ", " << chunks << " chunks");
            const std::vector<uint8_t> frame = azl::encodeHapFrame(source.data(), width, height, v.codec, chunks);
            std::vector<azl::HapImage> images;
            REQUIRE(azl::decodeHapFrame(frame.data(), frame.size(), width, height, images) == "");
            REQUIRE(images.size() == v.textures.size());
            for (size_t i = 0; i < images.size(); ++i) {
                CHECK(images[i].texture == v.textures[i]);
                CHECK(images[i].blocks.size() == azl::hapTextureBytes(v.textures[i], width, height));
            }
            const std::vector<uint8_t> decoded = azl::hapToRgba(images, width, height);
            CHECK(meanError(source, decoded, v.channels) < v.tolerance);
        }
    }
}

TEST_CASE("a malformed HAP frame says what is wrong instead of decoding")
{
    const int width = 32, height = 16;
    const std::vector<uint8_t> source = testPicture(width, height);
    const std::vector<uint8_t> frame = azl::encodeHapFrame(source.data(), width, height, azl::HapCodec::Hap, 2);
    std::vector<azl::HapImage> images;

    CHECK(azl::decodeHapFrame(frame.data(), 3, width, height, images) != "");
    CHECK(azl::decodeHapFrame(frame.data(), frame.size() - 1, width, height, images) != "");
    CHECK(azl::decodeHapFrame(frame.data(), frame.size(), width * 2, height, images) != "");
    std::vector<uint8_t> unknown = frame;
    unknown[3] = 0xc7;
    CHECK(azl::decodeHapFrame(unknown.data(), unknown.size(), width, height, images) != "");
    std::vector<uint8_t> garbled = frame;
    for (size_t i = garbled.size() / 2; i < garbled.size(); ++i)
        garbled[i] = 0xff;
    CHECK(azl::decodeHapFrame(garbled.data(), garbled.size(), width, height, images) != "");
    const std::vector<uint8_t> zeros(64, 0);
    CHECK(azl::decodeHapFrame(zeros.data(), zeros.size(), width, height, images) != "");
}

#ifdef __APPLE__
TEST_CASE("a decoded HAP image uploads into a Metal BC texture block for block")
{
    MTL::Device* device = MTL::CreateSystemDefaultDevice();
    REQUIRE(device);
    const int width = 64, height = 32;
    const std::vector<uint8_t> source = testPicture(width, height);
    for (const azl::HapCodec codec : {azl::HapCodec::Hap, azl::HapCodec::HapAlpha, azl::HapCodec::HapQAlpha}) {
        const std::vector<uint8_t> frame = azl::encodeHapFrame(source.data(), width, height, codec);
        std::vector<azl::HapImage> images;
        REQUIRE(azl::decodeHapFrame(frame.data(), frame.size(), width, height, images) == "");
        for (const azl::HapImage& image : images) {
            MTL::Texture* texture = azl::newHapTexture(device, image, width, height);
            REQUIRE(texture);
            const size_t rowBytes = image.blocks.size() / size_t(height / 4);
            std::vector<uint8_t> back(image.blocks.size());
            texture->getBytes(back.data(), rowBytes, MTL::Region(0, 0, width, height), 0);
            CHECK(back == image.blocks);
            texture->release();
        }
    }
    azl::HapImage wrongSize{azl::HapTexture::BC1, std::vector<uint8_t>(8)};
    CHECK(azl::newHapTexture(device, wrongSize, width, height) == nullptr);
    device->release();
}
#endif
