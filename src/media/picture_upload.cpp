#include "media/picture_upload.hpp"

#include <algorithm>
#include <vector>

namespace azl {

namespace {

bool bytewise(PlaneFormat format)
{
    return format == PlaneFormat::R8 || format == PlaneFormat::RG8 || format == PlaneFormat::BGRA8;
}

// The next mip level of a tightly packed 8-bit plane: each texel the mean of the 2x2 below it.
std::vector<uint8_t> halved(const std::vector<uint8_t>& texels, uint32_t width, uint32_t height, uint32_t channels)
{
    const uint32_t w = std::max(1u, width / 2), h = std::max(1u, height / 2);
    std::vector<uint8_t> out(size_t(w) * h * channels);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            for (uint32_t c = 0; c < channels; ++c) {
                const uint32_t x0 = std::min(2 * x, width - 1), x1 = std::min(2 * x + 1, width - 1);
                const uint32_t y0 = std::min(2 * y, height - 1), y1 = std::min(2 * y + 1, height - 1);
                const auto at = [&](uint32_t u, uint32_t v) {
                    return uint32_t(texels[(size_t(v) * width + u) * channels + c]);
                };
                out[(size_t(y) * w + x) * channels + c] =
                    uint8_t((at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1) + 2) / 4);
            }
    return out;
}

void uploadMips(const gpu::Device& device, const gpu::Texture& texture, const PicturePlane& plane)
{
    const uint32_t channels = gpu::unitBytes(texture.format());
    uint32_t w = uint32_t(plane.width), h = uint32_t(plane.height);
    std::vector<uint8_t> level(size_t(w) * h * channels);
    for (uint32_t y = 0; y < h; ++y)
        std::copy_n(plane.data + y * plane.stride, size_t(w) * channels, level.data() + size_t(y) * w * channels);
    for (uint32_t i = 0; i < texture.mipLevels(); ++i) {
        device.upload(texture, i, level.data(), size_t(w) * channels);
        level = halved(level, w, h, channels);
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
}

gpu::Texture uploadPlane(const gpu::Device& device, const PicturePlane& plane, bool mipmapped)
{
    const bool mips = mipmapped && bytewise(plane.format);
    gpu::Texture texture = device.makeTexture({.format = textureFormat(plane.format),
                                               .width = uint32_t(plane.width),
                                               .height = uint32_t(plane.height),
                                               .mipLevels = mips ? 0u : 1u,
                                               .memory = gpu::Memory::Upload});
    if (!texture)
        return {};
    if (mips)
        uploadMips(device, texture, plane);
    else
        device.upload(texture, 0, plane.data, plane.stride);
    return texture;
}

} // namespace

gpu::Format textureFormat(PlaneFormat format)
{
    switch (format) {
    case PlaneFormat::R8: return gpu::Format::R8;
    case PlaneFormat::RG8: return gpu::Format::RG8;
    case PlaneFormat::R16: return gpu::Format::R16;
    case PlaneFormat::RG16: return gpu::Format::RG16;
    case PlaneFormat::BGRA8: return gpu::Format::BGRA8;
    case PlaneFormat::BC1: return gpu::Format::BC1;
    case PlaneFormat::BC3: return gpu::Format::BC3;
    case PlaneFormat::BC4: return gpu::Format::BC4;
    }
    return gpu::Format::Invalid;
}

std::array<gpu::Texture, 2> uploadPicture(const gpu::Device& device, const DecodedPicture& picture, bool mipmapped)
{
    std::array<gpu::Texture, 2> textures;
    for (int i = 0; i < picture.planeCount; ++i)
        textures[i] = uploadPlane(device, picture.planes[i], mipmapped);
    return textures;
}

gpu::Texture uploadHapImage(const gpu::Device& device, const HapImage& image, int width, int height)
{
    if (image.blocks.size() != hapTextureBytes(image.texture, width, height))
        return {};
    const PlaneFormat format = image.texture == HapTexture::BC1   ? PlaneFormat::BC1
                               : image.texture == HapTexture::BC4 ? PlaneFormat::BC4
                                                                  : PlaneFormat::BC3;
    const PicturePlane plane{format, width, height, gpu::rowBytes(textureFormat(format), uint32_t(width)),
                             image.blocks.data()};
    return uploadPlane(device, plane, false);
}

} // namespace azl
