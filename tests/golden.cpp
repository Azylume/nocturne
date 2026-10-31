#include "golden.hpp"

#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#endif
#include <doctest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>

namespace {

constexpr int kChannelTolerance = 8;
constexpr double kMaxBadPixelFraction = 0.0002;

#ifdef __APPLE__
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
#endif

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

// tests/golden/<name>.png, or off Apple tests/golden/vulkan/<name>.png where Vulkan has its own: a case whose
// difference from Metal was looked at and explained (see tests/AGENTS.md).
std::string goldenPath(const std::string& name, [[maybe_unused]] bool update)
{
    const char* dir = std::getenv("NOCTURNE_GOLDEN_DIR");
    const std::string root = dir ? dir : NOCTURNE_GOLDEN_DIR;
#ifndef __APPLE__
    const std::string vulkan = root + "/vulkan/" + name + ".png";
    std::error_code ec;
    if (update || std::filesystem::exists(vulkan, ec))
        return vulkan;
#endif
    return root + "/" + name + ".png";
}

} // namespace

#ifdef __APPLE__
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
#endif


void checkGolden(const azl::Image& actual, const std::string& name)
{
    const char* updateVar = std::getenv("NOCTURNE_UPDATE_GOLDEN");
    const bool update = updateVar && std::strcmp(updateVar, "1") == 0;
    const std::string goldenPath = ::goldenPath(name, update);
    if (update) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(goldenPath).parent_path(), ec);
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
