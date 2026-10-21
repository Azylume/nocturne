#include "media/stills.hpp"

#include "media/hap_movie.hpp"

#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace azl {

namespace {

std::vector<uint8_t> jpegOf(CGImageRef image, double quality)
{
    NSMutableData* data = [NSMutableData data];
    CGImageDestinationRef destination =
        CGImageDestinationCreateWithData((__bridge CFMutableDataRef)data, CFSTR("public.jpeg"), 1, nullptr);
    if (!destination)
        return {};
    NSDictionary* options = @{(__bridge NSString*)kCGImageDestinationLossyCompressionQuality : @(quality)};
    CGImageDestinationAddImage(destination, image, (__bridge CFDictionaryRef)options);
    const bool ok = CGImageDestinationFinalize(destination);
    CFRelease(destination);
    if (!ok)
        return {};
    const auto* bytes = static_cast<const uint8_t*>(data.bytes);
    return {bytes, bytes + data.length};
}

// A HAP movie's frame `fraction` of the way in, decoded on the CPU (AVFoundation has no HAP decoder), at most
// `maxWidth` pixels wide; null for any other file.
CGImageRef hapImage(const std::filesystem::path& file, double fraction, uint32_t maxWidth)
{
    int width = 0, height = 0;
    const std::vector<uint8_t> rgba = readHapPicture(file, fraction, width, height);
    if (rgba.empty())
        return nullptr;
    const double scale = std::min(1.0, double(maxWidth) / width);
    const size_t w = std::max<size_t>(1, size_t(std::lround(width * scale)));
    const size_t h = std::max<size_t>(1, size_t(std::lround(height * scale)));
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CFDataRef data = CFDataCreate(nullptr, rgba.data(), CFIndex(rgba.size()));
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CGImageRef full = CGImageCreate(size_t(width), size_t(height), 8, 32, size_t(width) * 4, space,
                                    kCGImageAlphaLast, provider, nullptr, false,
                                    kCGRenderingIntentDefault);
    CGContextRef context = CGBitmapContextCreate(nullptr, w, h, 8, w * 4, space, kCGImageAlphaPremultipliedLast);
    CGImageRef image = nullptr;
    if (full && context) {
        CGContextDrawImage(context, CGRectMake(0, 0, w, h), full);
        image = CGBitmapContextCreateImage(context);
    }
    if (context)
        CGContextRelease(context);
    if (full)
        CGImageRelease(full);
    CGDataProviderRelease(provider);
    CFRelease(data);
    CGColorSpaceRelease(space);
    return image;
}

} // namespace

std::vector<uint8_t> encodeJpeg(const uint8_t* bgra, uint32_t width, uint32_t height, size_t bytesPerRow,
                                double quality)
{
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGDataProviderRef provider = CGDataProviderCreateWithData(nullptr, bgra, bytesPerRow * height, nullptr);
    CGImageRef image = CGImageCreate(width, height, 8, 32, bytesPerRow, space,
                                     kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little, provider, nullptr,
                                     false, kCGRenderingIntentDefault);
    std::vector<uint8_t> jpeg = image ? jpegOf(image, quality) : std::vector<uint8_t>{};
    if (image)
        CGImageRelease(image);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(space);
    return jpeg;
}

std::vector<uint8_t> videoStill(const std::filesystem::path& file, uint32_t width)
{
    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:file.c_str()]];
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
        AVAssetImageGenerator* generator = [AVAssetImageGenerator assetImageGeneratorWithAsset:asset];
        generator.appliesPreferredTrackTransform = YES;
        generator.maximumSize = CGSizeMake(width, width);
        generator.requestedTimeToleranceBefore = CMTimeMakeWithSeconds(0.5, 600);
        generator.requestedTimeToleranceAfter = CMTimeMakeWithSeconds(0.5, 600);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        const CMTime duration = asset.duration;
        const CMTime at = CMTIME_IS_NUMERIC(duration) ? CMTimeMultiplyByRatio(duration, 1, 3) : kCMTimeZero;
        NSError* error = nil;
        CGImageRef image = hapImage(file, 1.0 / 3.0, width);
        if (!image)
            image = [generator copyCGImageAtTime:at actualTime:nullptr error:&error];
#pragma clang diagnostic pop
        if (!image)
            throw std::runtime_error("no still in " + file.string() +
                                     (error ? ": " + std::string(error.localizedDescription.UTF8String) : ""));
        std::vector<uint8_t> jpeg = jpegOf(image, 0.75);
        CGImageRelease(image);
        if (jpeg.empty())
            throw std::runtime_error("cannot encode a still of " + file.string());
        return jpeg;
    }
}

// Seconds either side of a point a sampled picture may come from: half of the one-second GOP the show's transcodes
// have, so every picture can be a keyframe even on a short clip.
constexpr double kKeyframeReach = 0.5;

struct VideoSampler::Impl {
    AVAssetImageGenerator* generator = nil;
    CMTime duration = kCMTimeZero;
    uint32_t width = 0;
    uint32_t height = 0;
    int count = 1;
    // A HAP movie's pictures are decoded on the CPU.
    std::filesystem::path hap;
};

VideoSampler::VideoSampler(const std::filesystem::path& file, uint32_t width, uint32_t height, int count)
    : impl_(std::make_unique<Impl>())
{
    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:file.c_str()]];
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        AVAssetTrack* track = [asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
        const CMTime duration = asset.duration;
#pragma clang diagnostic pop
        if (!track || !CMTIME_IS_NUMERIC(duration) || CMTimeGetSeconds(duration) <= 0.0)
            throw std::runtime_error("no video in " + file.string());
        const CGSize natural = CGSizeApplyAffineTransform(track.naturalSize, track.preferredTransform);
        const double w = std::max(1.0, std::abs(double(natural.width)));
        const double h = std::max(1.0, std::abs(double(natural.height)));
        const double scale = std::max(width / w, height / h);
        impl_->generator = [AVAssetImageGenerator assetImageGeneratorWithAsset:asset];
        impl_->generator.appliesPreferredTrackTransform = YES;
        impl_->generator.maximumSize = CGSizeMake(std::ceil(w * scale), std::ceil(h * scale));
        const CMTime tolerance = CMTimeMaximum(CMTimeMultiplyByRatio(duration, 1, int32_t(2 * std::max(1, count))),
                                               CMTimeMakeWithSeconds(kKeyframeReach, 600));
        impl_->generator.requestedTimeToleranceBefore = tolerance;
        impl_->generator.requestedTimeToleranceAfter = tolerance;
        impl_->duration = duration;
        impl_->width = width;
        impl_->height = height;
        impl_->count = std::max(1, count);
        if (hapCodecOfFile(file) != HapCodec::None)
            impl_->hap = file;
    }
}

VideoSampler::~VideoSampler() = default;

std::vector<uint8_t> VideoSampler::picture(int i)
{
    @autoreleasepool {
        const CMTime at = CMTimeMultiplyByRatio(impl_->duration, int32_t(2 * i + 1), int32_t(2 * impl_->count));
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        CGImageRef image =
            impl_->hap.empty()
                ? [impl_->generator copyCGImageAtTime:at actualTime:nullptr error:nil]
                : hapImage(impl_->hap, (2.0 * i + 1.0) / (2.0 * impl_->count), 2 * std::max(impl_->width, impl_->height));
#pragma clang diagnostic pop
        if (!image)
            return {};
        const size_t row = size_t(impl_->width) * 4;
        std::vector<uint8_t> bgra(row * impl_->height);
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef context = CGBitmapContextCreate(bgra.data(), impl_->width, impl_->height, 8, row, space,
                                                     kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
        const double w = CGImageGetWidth(image), h = CGImageGetHeight(image);
        const double scale = std::max(impl_->width / w, impl_->height / h);
        CGContextDrawImage(context, CGRectMake((impl_->width - w * scale) / 2, (impl_->height - h * scale) / 2,
                                               w * scale, h * scale),
                           image);
        CGContextRelease(context);
        CGColorSpaceRelease(space);
        CGImageRelease(image);
        return bgra;
    }
}

} // namespace azl
