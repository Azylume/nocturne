#include "gpu/gpu.hpp"
#include "media/picture_upload.hpp"

#include "doctest.h"

#include <vector>

namespace gpu = azl::gpu;

namespace {

std::vector<uint8_t> readBack(const gpu::Device& device, const gpu::Queue& queue, const gpu::Texture& texture)
{
    gpu::Scope scope;
    gpu::Readback readback(device, texture);
    gpu::CommandBuffer cmd = queue.commandBuffer();
    readback.encode(cmd);
    cmd.commit();
    cmd.waitUntilCompleted();
    REQUIRE(cmd.completion().ok);
    return {readback.data(), readback.data() + readback.size()};
}

} // namespace

TEST_CASE("a decoded picture uploads plane by plane, text with a mip chain of 2x2 means")
{
    const gpu::Device device = gpu::Device::systemDefault();
    const gpu::Queue queue = device.makeQueue();
    // NV12 with padded rows: the upload honours the stride.
    std::vector<uint8_t> luma(8 * 16, 0), chroma(4 * 16, 0);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            luma[size_t(y) * 16 + x] = uint8_t(y * 8 + x);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 8; ++x)
            chroma[size_t(y) * 16 + x] = uint8_t(100 + y * 8 + x);
    azl::DecodedPicture picture;
    picture.planes[0] = {azl::PlaneFormat::R8, 8, 8, 16, luma.data()};
    picture.planes[1] = {azl::PlaneFormat::RG8, 4, 4, 16, chroma.data()};
    picture.planeCount = 2;
    const auto textures = azl::uploadPicture(device, picture);
    REQUIRE(textures[0]);
    REQUIRE(textures[1]);
    CHECK(textures[0].format() == gpu::Format::R8);
    CHECK(textures[1].format() == gpu::Format::RG8);
    const std::vector<uint8_t> back = readBack(device, queue, textures[0]);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            CHECK(back[size_t(y) * 8 + x] == uint8_t(y * 8 + x));
    const std::vector<uint8_t> backChroma = readBack(device, queue, textures[1]);
    CHECK(backChroma[0] == 100);
    CHECK(backChroma[4 * 2 * 3 + 7] == uint8_t(100 + 3 * 8 + 7));

    std::vector<uint8_t> rg(4 * 4 * 2);
    for (size_t i = 0; i < rg.size(); ++i)
        rg[i] = uint8_t(i * 10);
    azl::DecodedPicture text;
    text.planes[0] = {azl::PlaneFormat::RG8, 4, 4, 8, rg.data()};
    text.planeCount = 1;
    const auto mipped = azl::uploadPicture(device, text, true);
    REQUIRE(mipped[0]);
    CHECK(!mipped[1]);
    CHECK(mipped[0].mipLevels() == 3);
    // Level 1, texel (0, 0), red: the rounded mean of texels (0,0), (1,0), (0,1), (1,1) of level 0.
    const gpu::Buffer level1 = device.makeBuffer(2 * 2 * 2);
    {
        gpu::Scope scope;
        gpu::CommandBuffer cmd = queue.commandBuffer();
        cmd.copy(mipped[0], level1, 2 * 2, 0, 1);
        cmd.commit();
        cmd.waitUntilCompleted();
    }
    const auto* mip = static_cast<const uint8_t*>(level1.contents());
    CHECK(mip[0] == uint8_t((0 + 20 + 80 + 100 + 2) / 4));
}
