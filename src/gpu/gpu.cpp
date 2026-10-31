#include "gpu/gpu.hpp"

namespace azl::gpu {

bool isCompressed(Format format)
{
    return format == Format::BC1 || format == Format::BC3 || format == Format::BC4;
}

uint32_t unitBytes(Format format)
{
    switch (format) {
    case Format::R8: return 1;
    case Format::RG8:
    case Format::R16: return 2;
    case Format::BGRA8:
    case Format::RGBA8:
    case Format::RG16: return 4;
    case Format::RGBA16F:
    case Format::RGBA16:
    case Format::BC1:
    case Format::BC4: return 8;
    case Format::BC3: return 16;
    case Format::Invalid: break;
    }
    return 0;
}

size_t rowBytes(Format format, uint32_t width)
{
    const size_t units = isCompressed(format) ? (width + 3) / 4 : width;
    return units * unitBytes(format);
}

uint32_t rowCount(Format format, uint32_t height)
{
    return isCompressed(format) ? (height + 3) / 4 : height;
}

} // namespace azl::gpu
