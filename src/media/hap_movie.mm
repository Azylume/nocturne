#include "media/hap_movie.hpp"

#include "media/hap.hpp"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

namespace azl {

namespace {

constexpr uint32_t fourcc(const char (&code)[5])
{
    return uint32_t(uint8_t(code[0])) << 24 | uint32_t(uint8_t(code[1])) << 16 | uint32_t(uint8_t(code[2])) << 8 |
           uint32_t(uint8_t(code[3]));
}

std::string describe(NSError* error)
{
    return error ? std::string(error.localizedDescription.UTF8String) : "unknown error";
}

NSString* formatName(HapCodec codec)
{
    switch (codec) {
    case HapCodec::Hap: return @"Hap";
    case HapCodec::HapAlpha: return @"Hap Alpha";
    case HapCodec::HapQ: return @"Hap Q";
    case HapCodec::HapQAlpha: return @"Hap Q Alpha";
    case HapCodec::None: break;
    }
    return @"";
}

} // namespace

HapCodec hapCodecOf(uint32_t code)
{
    if (code == fourcc("Hap1"))
        return HapCodec::Hap;
    if (code == fourcc("Hap5"))
        return HapCodec::HapAlpha;
    if (code == fourcc("HapY"))
        return HapCodec::HapQ;
    if (code == fourcc("HapM"))
        return HapCodec::HapQAlpha;
    return HapCodec::None;
}

uint32_t fourccOf(HapCodec codec)
{
    switch (codec) {
    case HapCodec::Hap: return fourcc("Hap1");
    case HapCodec::HapAlpha: return fourcc("Hap5");
    case HapCodec::HapQ: return fourcc("HapY");
    case HapCodec::HapQAlpha: return fourcc("HapM");
    case HapCodec::None: break;
    }
    return 0;
}

struct HapMovieWriter::Impl {
    AVAssetWriter* writer = nil;
    AVAssetWriterInput* input = nil;
    CMFormatDescriptionRef format = nullptr;
    CMTime frameDuration = kCMTimeZero;
    int64_t frames = 0;
    bool finished = false;

    ~Impl()
    {
        if (!finished && writer)
            [writer cancelWriting];
        if (format)
            CFRelease(format);
    }
};

HapMovieWriter::HapMovieWriter(const std::filesystem::path& file, HapCodec codec, int width, int height, int fpsNum,
                               int fpsDen)
    : impl_(std::make_unique<Impl>())
{
    @autoreleasepool {
        if (codec == HapCodec::None || width <= 0 || height <= 0 || fpsNum <= 0 || fpsDen <= 0)
            throw std::runtime_error("invalid HAP movie settings");
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
        NSError* error = nil;
        impl_->writer = [AVAssetWriter assetWriterWithURL:[NSURL fileURLWithPath:@(file.c_str())]
                                                 fileType:AVFileTypeQuickTimeMovie
                                                    error:&error];
        if (!impl_->writer)
            throw std::runtime_error("cannot write " + file.string() + ": " + describe(error));
        const bool alpha = codec == HapCodec::HapAlpha || codec == HapCodec::HapQAlpha;
        NSDictionary* extensions = @{
            (id)kCMFormatDescriptionExtension_FormatName : formatName(codec),
            (id)kCMFormatDescriptionExtension_Depth : @(alpha ? 32 : 24),
        };
        if (CMVideoFormatDescriptionCreate(kCFAllocatorDefault, fourccOf(codec), width, height,
                                           (__bridge CFDictionaryRef)extensions, &impl_->format) != noErr)
            throw std::runtime_error("cannot describe the HAP track");
        impl_->input = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                                          outputSettings:nil
                                                        sourceFormatHint:impl_->format];
        impl_->input.expectsMediaDataInRealTime = NO;
        impl_->input.mediaTimeScale = fpsNum;
        if (![impl_->writer canAddInput:impl_->input])
            throw std::runtime_error("the movie cannot take a HAP track");
        [impl_->writer addInput:impl_->input];
        if (![impl_->writer startWriting])
            throw std::runtime_error("cannot start writing: " + describe(impl_->writer.error));
        [impl_->writer startSessionAtSourceTime:kCMTimeZero];
        impl_->frameDuration = CMTimeMake(fpsDen, fpsNum);
    }
}

HapMovieWriter::~HapMovieWriter() = default;

void HapMovieWriter::append(const std::vector<uint8_t>& frame)
{
    @autoreleasepool {
        CMBlockBufferRef block = nullptr;
        if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, frame.size(), kCFAllocatorDefault, nullptr,
                                               0, frame.size(), kCMBlockBufferAssureMemoryNowFlag, &block) != noErr ||
            CMBlockBufferReplaceDataBytes(frame.data(), block, 0, frame.size()) != noErr) {
            if (block)
                CFRelease(block);
            throw std::runtime_error("cannot buffer a HAP frame");
        }
        CMSampleTimingInfo timing{impl_->frameDuration, CMTimeMultiply(impl_->frameDuration, int32_t(impl_->frames)),
                                  kCMTimeInvalid};
        const size_t size = frame.size();
        CMSampleBufferRef sample = nullptr;
        const OSStatus made = CMSampleBufferCreateReady(kCFAllocatorDefault, block, impl_->format, 1, 1, &timing, 1,
                                                        &size, &sample);
        CFRelease(block);
        if (made != noErr)
            throw std::runtime_error("cannot make a HAP sample");
        while (!impl_->input.readyForMoreMediaData && impl_->writer.status == AVAssetWriterStatusWriting)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool appended = [impl_->input appendSampleBuffer:sample];
        CFRelease(sample);
        if (!appended)
            throw std::runtime_error("cannot write a HAP frame: " + describe(impl_->writer.error));
        ++impl_->frames;
    }
}

void HapMovieWriter::finish()
{
    @autoreleasepool {
        [impl_->input markAsFinished];
        [impl_->writer endSessionAtSourceTime:CMTimeMultiply(impl_->frameDuration, int32_t(impl_->frames))];
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [impl_->writer finishWritingWithCompletionHandler:^{
          dispatch_semaphore_signal(done);
        }];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        impl_->finished = true;
        if (impl_->writer.status != AVAssetWriterStatusCompleted)
            throw std::runtime_error("cannot finish the movie: " + describe(impl_->writer.error));
    }
}

namespace {

// The track holds its asset weakly: the caller keeps `asset`.
AVAssetTrack* videoTrack(const std::filesystem::path& file, AVURLAsset* __strong& asset)
{
    asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:@(file.c_str())] options:nil];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return [asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
#pragma clang diagnostic pop
}

HapCodec codecOf(AVAssetTrack* track)
{
    auto format = (__bridge CMFormatDescriptionRef)track.formatDescriptions.firstObject;
    return format ? hapCodecOf(CMFormatDescriptionGetMediaSubType(format)) : HapCodec::None;
}

} // namespace

HapCodec hapCodecOfFile(const std::filesystem::path& file)
{
    @autoreleasepool {
        AVURLAsset* asset = nil;
        AVAssetTrack* track = videoTrack(file, asset);
        return track ? codecOf(track) : HapCodec::None;
    }
}

std::vector<uint8_t> readHapPicture(const std::filesystem::path& file, double fraction, int& width, int& height)
{
    @autoreleasepool {
        AVURLAsset* asset = nil;
        AVAssetTrack* track = videoTrack(file, asset);
        if (!track || codecOf(track) == HapCodec::None)
            return {};
        auto format = (__bridge CMFormatDescriptionRef)track.formatDescriptions.firstObject;
        const CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions(format);
        AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:asset error:nil];
        if (!reader)
            return {};
        AVAssetReaderTrackOutput* output = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track
                                                                                      outputSettings:nil];
        [reader addOutput:output];
        const CMTimeRange range = track.timeRange;
        reader.timeRange = CMTimeRangeMake(
            CMTimeAdd(range.start, CMTimeMultiplyByFloat64(range.duration, std::clamp(fraction, 0.0, 0.999))),
            kCMTimePositiveInfinity);
        if (![reader startReading])
            return {};
        CMSampleBufferRef sample = [output copyNextSampleBuffer];
        while (sample && CMSampleBufferGetNumSamples(sample) == 0) {
            CFRelease(sample);
            sample = [output copyNextSampleBuffer];
        }
        [reader cancelReading];
        if (!sample)
            return {};
        CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
        std::vector<uint8_t> bytes(block ? CMBlockBufferGetDataLength(block) : 0);
        if (!bytes.empty())
            CMBlockBufferCopyDataBytes(block, 0, bytes.size(), bytes.data());
        CFRelease(sample);
        std::vector<HapImage> images;
        if (!decodeHapFrame(bytes.data(), bytes.size(), size.width, size.height, images).empty())
            return {};
        width = size.width;
        height = size.height;
        return hapToRgba(images, width, height);
    }
}

std::vector<std::vector<uint8_t>> readHapFrames(const std::filesystem::path& file, int count)
{
    @autoreleasepool {
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:@(file.c_str())] options:nil];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        AVAssetTrack* track = [asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
#pragma clang diagnostic pop
        if (!track)
            throw std::runtime_error(file.string() + " has no video track");
        NSError* error = nil;
        AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:asset error:&error];
        if (!reader)
            throw std::runtime_error("cannot read " + file.string() + ": " + describe(error));
        AVAssetReaderTrackOutput* output = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track
                                                                                      outputSettings:nil];
        output.alwaysCopiesSampleData = NO;
        [reader addOutput:output];
        if (![reader startReading])
            throw std::runtime_error("cannot read " + file.string() + ": " + describe(reader.error));
        std::vector<std::vector<uint8_t>> frames;
        while (int(frames.size()) < count) {
            CMSampleBufferRef sample = [output copyNextSampleBuffer];
            if (!sample)
                break;
            if (CMSampleBufferGetNumSamples(sample) == 0) {
                CFRelease(sample);
                continue;
            }
            CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
            std::vector<uint8_t> bytes(block ? CMBlockBufferGetDataLength(block) : 0);
            if (!bytes.empty())
                CMBlockBufferCopyDataBytes(block, 0, bytes.size(), bytes.data());
            frames.push_back(std::move(bytes));
            CFRelease(sample);
        }
        [reader cancelReading];
        return frames;
    }
}

} // namespace azl
