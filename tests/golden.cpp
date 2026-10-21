#include "golden.hpp"

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <doctest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <optional>

namespace {

constexpr int kChannelTolerance = 8;
constexpr double kMaxBadPixelFraction = 0.0002;

CFURLRef fileUrl(const std::string& path)
{
    return CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
                                                   CFIndex(path.size()), false);
}

CGContextRef rgbaContext(azl::Image& image)
{
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(image.rgba.data(), image.width, image.height, 8, image.width * 4,
                                                 space, kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(space);
    return context;
}

bool pixelDiffers(const uint8_t* a, const uint8_t* b)
{
    for (int c = 0; c < 4; ++c)
        if (std::abs(int(a[c]) - int(b[c])) > kChannelTolerance)
            return true;
    return false;
}

size_t badPixels(const azl::Image& a, const azl::Image& b)
{
    size_t bad = 0;
    for (size_t i = 0; i < a.rgba.size(); i += 4)
        bad += pixelDiffers(&a.rgba[i], &b.rgba[i]);
    return bad;
}

} // namespace

void savePng(azl::Image image, const std::string& path)
{
    CGContextRef context = rgbaContext(image);
    CGImageRef cg = CGBitmapContextCreateImage(context);
    CFURLRef url = fileUrl(path);
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
    CGImageDestinationAddImage(dest, cg, nullptr);
    CGImageDestinationFinalize(dest);
    CFRelease(dest);
    CFRelease(url);
    CGImageRelease(cg);
    CGContextRelease(context);
}

std::optional<azl::Image> loadPng(const std::string& path)
{
    CFURLRef url = fileUrl(path);
    CGImageSourceRef source = CGImageSourceCreateWithURL(url, nullptr);
    CFRelease(url);
    if (!source)
        return std::nullopt;
    CGImageRef cg = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    CFRelease(source);
    if (!cg)
        return std::nullopt;

    azl::Image image{uint32_t(CGImageGetWidth(cg)), uint32_t(CGImageGetHeight(cg)), {}};
    image.rgba.resize(size_t(image.width) * image.height * 4);
    CGContextRef context = rgbaContext(image);
    CGContextDrawImage(context, CGRectMake(0, 0, image.width, image.height), cg);
    CGContextRelease(context);
    CGImageRelease(cg);
    return image;
}


void checkGolden(const azl::Image& actual, const std::string& name)
{
    const char* dir = std::getenv("NOCTURNE_GOLDEN_DIR");
    const std::string goldenPath = std::string(dir ? dir : NOCTURNE_GOLDEN_DIR) + "/" + name + ".png";
    const char* update = std::getenv("NOCTURNE_UPDATE_GOLDEN");
    if (update && std::strcmp(update, "1") == 0) {
        savePng(actual, goldenPath);
        MESSAGE("golden rewritten: " << goldenPath);
        return;
    }

    const std::optional<azl::Image> golden = loadPng(goldenPath);
    REQUIRE_MESSAGE(golden, "missing golden " << goldenPath << " (run with NOCTURNE_UPDATE_GOLDEN=1)");
    REQUIRE(golden->width == actual.width);
    REQUIRE(golden->height == actual.height);

    const size_t bad = badPixels(*golden, actual);
    const size_t allowed = size_t(double(actual.width) * actual.height * kMaxBadPixelFraction);
    if (bad > allowed)
        savePng(actual, name + ".actual.png");
    CHECK_MESSAGE(bad <= allowed, bad << " pixels differ from " << goldenPath << "; actual written to " << name
                                      << ".actual.png");
}
