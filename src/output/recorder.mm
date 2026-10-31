#include "output/recorder.hpp"

#include "engine/screen_painter.hpp"
#include "output/ycbcr_converter.hpp"

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace azl {

namespace {

// Bounds the frames between the render thread and the file: those being filled, queued and held by the encoder.
constexpr int kBuffers = 8;
// A movie cut off before it is finished still plays up to its last fragment.
constexpr double kFragmentSeconds = 2.0;

struct Codec {
    AVVideoCodecType writer;
    CMVideoCodecType toolbox;
};

Codec codecOf(const std::string& name)
{
    if (name == "hevc")
        return {AVVideoCodecTypeHEVC, kCMVideoCodecType_HEVC};
    if (name == "prores")
        return {AVVideoCodecTypeAppleProRes422, kCMVideoCodecType_AppleProRes422};
    throw std::runtime_error("unknown codec " + name + " (hevc or prores)");
}

std::optional<uint64_t> screenOf(const std::string& target, const Composition& composition)
{
    if (target == "composition")
        return std::nullopt;
    if (target.rfind("screen:", 0) == 0) {
        const uint64_t id = std::strtoull(target.c_str() + 7, nullptr, 10);
        for (const Screen& screen : composition.screens)
            if (screen.id == id)
                return id;
    }
    throw std::runtime_error("unknown recording target " + target + " (composition or screen:N)");
}

bool diskFull(NSError* error)
{
    for (NSError* e = error; e; e = e.userInfo[NSUnderlyingErrorKey]) {
        if ([e.domain isEqualToString:AVFoundationErrorDomain] && e.code == AVErrorDiskFull)
            return true;
        if ([e.domain isEqualToString:NSPOSIXErrorDomain] && e.code == ENOSPC)
            return true;
        if ([e.domain isEqualToString:NSOSStatusErrorDomain] && e.code == dskFulErr)
            return true;
    }
    return false;
}

struct Encoder {
    // Whether it runs in hardware, once known.
    std::optional<bool> hardware;
    // The pixel buffers it takes without converting them; nil when it does not say.
    NSDictionary* native = nil;
};

// Unless `precise`, HEVC is asked for 8-bit Main, so the encoder takes 8-bit '420v' instead of 10-bit 'x420'.
Encoder askEncoder(CMVideoCodecType codec, uint32_t width, uint32_t height, bool precise)
{
    NSDictionary* spec = @{(__bridge NSString*)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder : @YES};
    VTCompressionSessionRef session = nullptr;
    if (VTCompressionSessionCreate(nullptr, int32_t(width), int32_t(height), codec, (__bridge CFDictionaryRef)spec,
                                   nullptr, nullptr, nullptr, nullptr, &session) != noErr)
        return {};
    Encoder encoder;
    if (codec == kCMVideoCodecType_HEVC && !precise)
        VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_HEVC_Main_AutoLevel);
    CFBooleanRef hardware = nullptr;
    if (VTSessionCopyProperty(session, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder, nullptr,
                              &hardware) == noErr && hardware) {
        encoder.hardware = CFBooleanGetValue(hardware);
        CFRelease(hardware);
    }
    if (CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(session))
        encoder.native = [(__bridge NSDictionary*)CVPixelBufferPoolGetPixelBufferAttributes(pool) copy];
    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    return encoder;
}

// The encoder's own pixel format when the recorder can write it, with the padding and alignment it asks for; else
// BGRA, which the encoder converts itself. Unless `precise`, ProRes gets 8-bit '2vuy' instead of 16-bit 'v216': the
// composition has 8 bits per channel, and the Mac mini's GPU pays for every byte written.
NSMutableDictionary* bufferAttributes(NSDictionary* native, uint32_t width, uint32_t height, bool precise)
{
    NSMutableDictionary* attributes = [@{
        (__bridge NSString*)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
        (__bridge NSString*)kCVPixelBufferWidthKey : @(width),
        (__bridge NSString*)kCVPixelBufferHeightKey : @(height),
        (__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey : @{},
        (__bridge NSString*)kCVPixelBufferMetalCompatibilityKey : @YES,
        (__bridge NSString*)kCVBufferPropagatedAttachmentsKey : @{
            (__bridge NSString*)kCVImageBufferColorPrimariesKey : (__bridge NSString*)kCVImageBufferColorPrimaries_ITU_R_709_2,
            (__bridge NSString*)kCVImageBufferTransferFunctionKey :
                (__bridge NSString*)kCVImageBufferTransferFunction_ITU_R_709_2,
            (__bridge NSString*)kCVImageBufferYCbCrMatrixKey : (__bridge NSString*)kCVImageBufferYCbCrMatrix_ITU_R_709_2,
        },
    } mutableCopy];
    NSNumber* format = native[(__bridge NSString*)kCVPixelBufferPixelFormatTypeKey];
    if (!precise && format.unsignedIntValue == kCVPixelFormatType_422YpCbCr16)
        format = @(kCVPixelFormatType_422YpCbCr8);
    if (!format || !YCbCrConverter::writes(format.unsignedIntValue) || width % 4 || height % 2)
        return attributes;
    attributes[(__bridge NSString*)kCVPixelBufferPixelFormatTypeKey] = format;
    for (NSString* key : @[(__bridge NSString*)kCVPixelBufferExtendedPixelsLeftKey,
                           (__bridge NSString*)kCVPixelBufferExtendedPixelsTopKey,
                           (__bridge NSString*)kCVPixelBufferExtendedPixelsRightKey,
                           (__bridge NSString*)kCVPixelBufferExtendedPixelsBottomKey,
                           (__bridge NSString*)kCVPixelBufferBytesPerRowAlignmentKey,
                           (__bridge NSString*)kCVPixelBufferPlaneAlignmentKey])
        if (native[key])
            attributes[key] = native[key];
    return attributes;
}

struct CompressorRelease {
    void operator()(VTCompressionSessionRef session) const
    {
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
    }
};
using Compressor = std::unique_ptr<std::remove_pointer_t<VTCompressionSessionRef>, CompressorRelease>;

// The one encoder of a recording; every file it rolls over to takes its output as it is.
Compressor makeCompressor(const std::string& codec, uint32_t width, uint32_t height, double fps, bool precise,
                          VTCompressionOutputCallback output, void* refcon)
{
    NSDictionary* spec = @{(__bridge NSString*)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder : @YES};
    VTCompressionSessionRef session = nullptr;
    if (VTCompressionSessionCreate(nullptr, int32_t(width), int32_t(height), codecOf(codec).toolbox,
                                   (__bridge CFDictionaryRef)spec, nullptr, nullptr, output, refcon, &session) != noErr)
        throw std::runtime_error("this machine cannot encode " + codec + " at " + std::to_string(width) + "x" +
                                 std::to_string(height));
    Compressor compressor(session);
    NSMutableDictionary* properties = [@{
        (__bridge NSString*)kVTCompressionPropertyKey_RealTime : @YES,
        (__bridge NSString*)kVTCompressionPropertyKey_ColorPrimaries :
            (__bridge NSString*)kCVImageBufferColorPrimaries_ITU_R_709_2,
        (__bridge NSString*)kVTCompressionPropertyKey_TransferFunction :
            (__bridge NSString*)kCVImageBufferTransferFunction_ITU_R_709_2,
        (__bridge NSString*)kVTCompressionPropertyKey_YCbCrMatrix :
            (__bridge NSString*)kCVImageBufferYCbCrMatrix_ITU_R_709_2,
    } mutableCopy];
    if (codec == "hevc") {
        properties[(__bridge NSString*)kVTCompressionPropertyKey_AverageBitRate] =
            @(int64_t(double(width) * height * fps * 0.2));
        properties[(__bridge NSString*)kVTCompressionPropertyKey_ExpectedFrameRate] = @(fps);
        properties[(__bridge NSString*)kVTCompressionPropertyKey_AllowFrameReordering] = @NO;
        if (!precise)
            properties[(__bridge NSString*)kVTCompressionPropertyKey_ProfileLevel] =
                (__bridge NSString*)kVTProfileLevel_HEVC_Main_AutoLevel;
    }
    VTSessionSetProperties(session, (__bridge CFDictionaryRef)properties);
    VTCompressionSessionPrepareToEncodeFrames(session);
    return compressor;
}

std::string fourCC(OSType code)
{
    return {char(code >> 24), char(code >> 16), char(code >> 8), char(code)};
}

std::string describe(NSError* error)
{
    return error ? error.localizedDescription.UTF8String : "unknown error";
}

bool playable(const std::filesystem::path& file)
{
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:@(file.c_str())] options:nil];
    dispatch_semaphore_t loaded = dispatch_semaphore_create(0);
    __block bool frames = false;
    [asset loadTracksWithMediaType:AVMediaTypeVideo
                 completionHandler:^(NSArray<AVAssetTrack*>* tracks, NSError*) {
                     if (tracks.count == 0) {
                         dispatch_semaphore_signal(loaded);
                         return;
                     }
                     [tracks[0] loadValuesAsynchronouslyForKeys:@[ @"timeRange" ]
                                              completionHandler:^{
                                                  frames = CMTimeGetSeconds(tracks[0].timeRange.duration) > 0.0;
                                                  dispatch_semaphore_signal(loaded);
                                              }];
                 }];
    dispatch_semaphore_wait(loaded, DISPATCH_TIME_FOREVER);
    return frames;
}

// One movie file: an AVAssetWriter taking the encoder's compressed frames as they are and, when asked, encoding an
// AAC track itself.
struct AvfMovie final : recording::MovieFile {
    // The frames handed to the encoder for this file and not yet in its movie.
    struct Track {
        AVAssetWriter* writer = nil;
        AVAssetWriterInput* input = nil;
        std::mutex mutex;
        std::condition_variable drained;
        int pending = 0;
    };

    ~AvfMovie() override
    {
        if (audioFormat)
            CFRelease(audioFormat);
    }

    bool hasAudio() const override { return audioInput != nil; }
    bool audioReady() const override { return audioInput.readyForMoreMediaData; }

    bool appendAudio(const float* samples, size_t count, int64_t at) override
    {
        const size_t bytes = count * sizeof(float);
        CMBlockBufferRef block = nullptr;
        CMSampleBufferRef sample = nullptr;
        bool ok = CMBlockBufferCreateWithMemoryBlock(nullptr, nullptr, bytes, nullptr, nullptr, 0, bytes,
                                                     kCMBlockBufferAssureMemoryNowFlag, &block) == noErr &&
                  CMBlockBufferReplaceDataBytes(samples, block, 0, bytes) == noErr &&
                  CMAudioSampleBufferCreateReadyWithPacketDescriptions(nullptr, block, audioFormat, CMItemCount(count),
                                                                       CMTimeMake(at, int32_t(audioRate)), nullptr,
                                                                       &sample) == noErr;
        if (ok)
            ok = [audioInput appendSampleBuffer:sample];
        if (sample)
            CFRelease(sample);
        if (block)
            CFRelease(block);
        return ok;
    }

    bool finish() override
    {
        {
            std::unique_lock lock(track->mutex);
            track->drained.wait(lock, [&] { return track->pending == 0; });
        }
        [input markAsFinished];
        [audioInput markAsFinished];
        dispatch_semaphore_t finished = dispatch_semaphore_create(0);
        [writer finishWritingWithCompletionHandler:^{ dispatch_semaphore_signal(finished); }];
        dispatch_semaphore_wait(finished, DISPATCH_TIME_FOREVER);
        return writer.status == AVAssetWriterStatusCompleted;
    }

    void cancel() override { [writer cancelWriting]; }
    bool writing() const override { return writer.status == AVAssetWriterStatusWriting; }
    bool diskFull() const override { return azl::diskFull(writer.error); }
    std::string error() const override { return describe(writer.error); }

    AVAssetWriter* writer = nil;
    AVAssetWriterInput* input = nil;
    AVAssetWriterInput* audioInput = nil;
    std::shared_ptr<Track> track;
    double audioRate = 0.0;
    CMAudioFormatDescriptionRef audioFormat = nullptr;
};

// The recording's one VideoToolbox session; its output callback appends each frame to the file it was handed for.
class AvfEncoder final : public recording::MovieEncoder {
public:
    AvfEncoder(const std::string& codec, uint32_t width, uint32_t height, double fps, bool precise,
               std::optional<bool> hardware, double audioRate, Counts& counts)
        : counts_(counts), hardware_(hardware)
    {
        if (audioRate > 0.0) {
            AudioStreamBasicDescription pcm{.mSampleRate = audioRate,
                                            .mFormatID = kAudioFormatLinearPCM,
                                            .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
                                            .mBytesPerPacket = 4,
                                            .mFramesPerPacket = 1,
                                            .mBytesPerFrame = 4,
                                            .mChannelsPerFrame = 1,
                                            .mBitsPerChannel = 32};
            CMAudioFormatDescriptionCreate(nullptr, &pcm, 0, nullptr, 0, nullptr, nullptr, &audioFormat_);
        }
        compressor_ = makeCompressor(codec, width, height, fps, precise, encoded, this);
    }

    ~AvfEncoder() override
    {
        compressor_.reset();
        if (audioFormat_)
            CFRelease(audioFormat_);
    }

    std::unique_ptr<recording::MovieFile> open(const std::filesystem::path& partial, double audioRate) override
    {
        auto next = std::make_unique<AvfMovie>();
        NSError* failure = nil;
        AVAssetWriter* writer = [AVAssetWriter assetWriterWithURL:[NSURL fileURLWithPath:@(partial.c_str())]
                                                         fileType:AVFileTypeQuickTimeMovie
                                                            error:&failure];
        if (!writer)
            throw std::runtime_error("cannot write " + partial.string() + ": " + describe(failure));
        writer.movieFragmentInterval = CMTimeMakeWithSeconds(kFragmentSeconds, 600);
        next->writer = writer;
        next->input = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:nil];
        next->input.expectsMediaDataInRealTime = YES;
        [writer addInput:next->input];
        next->track = std::make_shared<AvfMovie::Track>();
        next->track->writer = writer;
        next->track->input = next->input;
        NSDictionary* aac = @{AVFormatIDKey : @(kAudioFormatMPEG4AAC),
                              AVSampleRateKey : @(audioRate),
                              AVNumberOfChannelsKey : @1,
                              AVEncoderBitRateKey : @128000};
        if (audioRate > 0.0 && audioFormat_ && [writer canApplyOutputSettings:aac forMediaType:AVMediaTypeAudio]) {
            next->audioInput = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio
                                                                  outputSettings:aac
                                                                sourceFormatHint:audioFormat_];
            next->audioInput.expectsMediaDataInRealTime = YES;
            [writer addInput:next->audioInput];
            next->audioRate = audioRate;
            next->audioFormat = static_cast<CMAudioFormatDescriptionRef>(CFRetain(audioFormat_));
        }
        if (![writer startWriting])
            throw std::runtime_error("cannot start the movie: " + describe(writer.error));
        [writer startSessionAtSourceTime:kCMTimeZero];
        return next;
    }

    bool encode(const recording::Picture& picture, double seconds, double origin, bool key,
                recording::MovieFile& file) override
    {
        const std::shared_ptr<AvfMovie::Track>& track = static_cast<AvfMovie&>(file).track;
        auto routed = std::make_unique<Routed>(Routed{track, origin});
        {
            std::lock_guard lock(track->mutex);
            track->pending++;
        }
        NSDictionary* options = key ? @{(__bridge NSString*)kVTEncodeFrameOptionKey_ForceKeyFrame : @YES} : nil;
        const CMTime time = CMTimeMake(int64_t(std::llround(seconds * 1e6)), 1000000);
        if (VTCompressionSessionEncodeFrame(compressor_.get(), static_cast<CVPixelBufferRef>(picture.get()), time,
                                            kCMTimeInvalid, (__bridge CFDictionaryRef)options, routed.get(),
                                            nullptr) != noErr) {
            std::lock_guard lock(track->mutex);
            track->pending--;
            track->drained.notify_all();
            return false;
        }
        routed.release();
        return true;
    }

    void complete() override { VTCompressionSessionCompleteFrames(compressor_.get(), kCMTimeInvalid); }
    std::optional<bool> hardware() const override { return hardware_; }

private:
    // What the encoder's output callback gets with each frame.
    struct Routed {
        std::shared_ptr<AvfMovie::Track> track;
        double origin;
    };

    // The encoder's output, in the order the frames went in: each lands in the movie of the file it was handed for.
    static void encoded(void* encoder, void* frame, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample)
    {
        auto* self = static_cast<AvfEncoder*>(encoder);
        const std::unique_ptr<Routed> routed(static_cast<Routed*>(frame));
        if (status != noErr)
            self->counts_.failed = true;
        else if (!sample || (flags & kVTEncodeInfo_FrameDropped))
            self->counts_.dropped++;
        else if (append(*routed, sample))
            self->counts_.frames++;
        else
            self->counts_.failed = true;
        AvfMovie::Track& track = *routed->track;
        std::lock_guard lock(track.mutex);
        track.pending--;
        track.drained.notify_all();
    }

    static bool append(const Routed& routed, CMSampleBufferRef sample)
    {
        CMItemCount count = 0;
        CMSampleBufferGetSampleTimingInfoArray(sample, 0, nullptr, &count);
        std::vector<CMSampleTimingInfo> timing(static_cast<size_t>(count));
        CMSampleBufferGetSampleTimingInfoArray(sample, count, timing.data(), &count);
        const CMTime origin = CMTimeMake(int64_t(std::llround(routed.origin * 1e6)), 1000000);
        for (CMSampleTimingInfo& t : timing) {
            if (CMTIME_IS_VALID(t.presentationTimeStamp))
                t.presentationTimeStamp = CMTimeSubtract(t.presentationTimeStamp, origin);
            if (CMTIME_IS_VALID(t.decodeTimeStamp))
                t.decodeTimeStamp = CMTimeSubtract(t.decodeTimeStamp, origin);
        }
        CMSampleBufferRef moved = nullptr;
        if (CMSampleBufferCreateCopyWithNewTiming(nullptr, sample, count, timing.data(), &moved) != noErr)
            return false;
        AVAssetWriterInput* input = routed.track->input;
        bool ok = true;
        while (ok && !input.readyForMoreMediaData) {
            ok = routed.track->writer.status == AVAssetWriterStatusWriting;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ok = ok && [input appendSampleBuffer:moved];
        CFRelease(moved);
        return ok;
    }

    Counts& counts_;
    std::optional<bool> hardware_;
    CMAudioFormatDescriptionRef audioFormat_ = nullptr;
    Compressor compressor_;
};

} // namespace

// Fills a pooled IOSurface-backed pixel buffer from each frame on the frame's command buffer and hands it to the
// recording's writer once the GPU finished it.
struct Recorder::Session : FrameTap, std::enable_shared_from_this<Session> {
    Session(Recorder& owner, Settings s, double fps, AudioPlan audio, SpaceProvider space,
            std::chrono::milliseconds spaceInterval);
    ~Session() override;

    Done encode(gpu::CommandBuffer& cmd, const Frame& frame, const Composition& composition, double seconds) override;
    bool encodeConverted(MTL::CommandBuffer* cmd, const Frame& frame, const Composition& composition,
                         CVPixelBufferRef buffer);
    bool encodeBGRA(MTL::CommandBuffer* cmd, const Frame& frame, const Composition& composition, CVPixelBufferRef buffer,
                    CVMetalTextureRef& texture);
    void paint(MTL::CommandBuffer* cmd, const Frame& frame, const Composition& composition, MTL::Texture* target);

    Recorder& owner;
    std::unique_ptr<recording::Writer> writer;
    uint32_t width = 0;
    uint32_t height = 0;
    std::optional<uint64_t> screen;
    // The engine's render timeline: the session encodes into the frame's command buffer.
    const profile::GpuTimeline* gpu = nullptr;
    std::unique_ptr<ScreenPainter> painter;
    // Writes the encoder's own format; without one the buffers are BGRA.
    std::unique_ptr<YCbCrConverter> converter;
    // What the painter paints for the converter.
    NS::SharedPtr<MTL::Texture> painted;

    CVPixelBufferPoolRef pool = nullptr;
    CVMetalTextureCacheRef cache = nullptr;
    NSDictionary* aux = nil;
    AudioAnalysis* analysis = nullptr;
};

Recorder::Session::Session(Recorder& owner, Settings s, double fps, AudioPlan audio, SpaceProvider space,
                           std::chrono::milliseconds spaceInterval)
    : owner(owner)
{
    Engine& engine = owner.engine_;
    const Codec codec = codecOf(s.codec);
    screen = screenOf(s.target, *engine.composition());
    const uint32_t compositionWidth = engine.config().width;
    const uint32_t compositionHeight = engine.config().height;
    width = (s.width ? s.width : compositionWidth) & ~1u;
    height = (s.height ? s.height : compositionHeight) & ~1u;
    if (width < 16 || height < 16 || width > 8192 || height > 8192)
        throw std::runtime_error("recording size out of range");
    const bool whole = !screen && width == compositionWidth && height == compositionHeight;

    writer = std::make_unique<recording::Writer>(
        std::move(s), std::move(audio), std::move(space), spaceInterval,
        [this](const Ended& ended) { this->owner.ended(*this, ended); },
        [&owner] {
            if (owner.onSpace_)
                owner.onSpace_();
        });
    const Settings& settings = writer->settings;
    const Encoder encoder = askEncoder(codec.toolbox, width, height, settings.precise);
    NSDictionary* bufferAttributes = azl::bufferAttributes(encoder.native, width, height, settings.precise);
    const OSType format = [bufferAttributes[(__bridge NSString*)kCVPixelBufferPixelFormatTypeKey] unsignedIntValue];
    gpu = &engine.gpuTimeline();
    if (format != kCVPixelFormatType_32BGRA) {
        converter = std::make_unique<YCbCrConverter>(engine.device(), format);
    }
    if (!whole) {
        painter = std::make_unique<ScreenPainter>(engine.gpuDevice(), kFrameFormat,
                                                  std::array<uint32_t, 2>{compositionWidth, compositionHeight});
        painter->setGpuTimeline(gpu);
    }
    if (!whole && converter) {
        auto* desc =
            MTL::TextureDescriptor::texture2DDescriptor(gpu::metalFormat(kFrameFormat), width, height, false);
        desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        desc->setStorageMode(MTL::StorageModePrivate);
        painted = NS::TransferPtr(engine.device()->newTexture(desc));
    }

    const double audioRate = writer->audioRate;
    writer->open(
        [&](recording::MovieEncoder::Counts& counts) {
            return std::make_unique<AvfEncoder>(settings.codec, width, height, fps, settings.precise, encoder.hardware,
                                                audioRate, counts);
        },
        owner.encoderSetup_);

    NSDictionary* poolAttributes = @{(__bridge NSString*)kCVPixelBufferPoolMinimumBufferCountKey : @(kBuffers),
                                     (__bridge NSString*)kCVPixelBufferPoolMaximumBufferAgeKey : @0};
    aux = @{(__bridge NSString*)kCVPixelBufferPoolAllocationThresholdKey : @(kBuffers)};
    if (CVPixelBufferPoolCreate(nullptr, (__bridge CFDictionaryRef)poolAttributes,
                                (__bridge CFDictionaryRef)bufferAttributes, &pool) != kCVReturnSuccess ||
        CVMetalTextureCacheCreate(nullptr, nullptr, (__bridge id<MTLDevice>)engine.device(), nullptr, &cache) !=
            kCVReturnSuccess)
        throw std::runtime_error("cannot allocate recording buffers");
    // Allocates every buffer now, so the render thread only ever reuses them.
    std::vector<CVPixelBufferRef> warm;
    for (int i = 0; i < kBuffers; ++i) {
        CVPixelBufferRef buffer = nullptr;
        if (CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(nullptr, pool, (__bridge CFDictionaryRef)aux,
                                                                &buffer) == kCVReturnSuccess)
            warm.push_back(buffer);
    }
    for (CVPixelBufferRef buffer : warm) {
        if (converter) {
            converter->prepare(buffer);
            CFRelease(buffer);
            continue;
        }
        CVMetalTextureRef texture = nullptr;
        if (CVMetalTextureCacheCreateTextureFromImage(nullptr, cache, buffer, nullptr, MTLPixelFormatBGRA8Unorm, width,
                                                      height, 0, &texture) == kCVReturnSuccess)
            CFRelease(texture);
        CFRelease(buffer);
    }
}

Recorder::Session::~Session()
{
    writer.reset();
    if (cache)
        CFRelease(cache);
    if (pool)
        CVPixelBufferPoolRelease(pool);
}

FrameTap::Done Recorder::Session::encode(gpu::CommandBuffer& cmd, const Frame& frame, const Composition& composition,
                                         double seconds)
{
    CVPixelBufferRef buffer = nullptr;
    if (CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(nullptr, pool, (__bridge CFDictionaryRef)aux, &buffer) !=
        kCVReturnSuccess) {
        writer->drop();
        return nullptr;
    }
    CVMetalTextureRef texture = nullptr;
    const bool encoded = converter ? encodeConverted(cmd.native(), frame, composition, buffer)
                                   : encodeBGRA(cmd.native(), frame, composition, buffer, texture);
    if (!encoded) {
        CFRelease(buffer);
        writer->drop();
        return nullptr;
    }
    writer->begin();
    return [self = shared_from_this(), buffer, texture, seconds](bool ok) {
        if (texture)
            CFRelease(texture);
        self->writer->queued(recording::Picture(buffer, [](CVPixelBufferRef b) { CFRelease(b); }), seconds, ok);
    };
}

bool Recorder::Session::encodeConverted(MTL::CommandBuffer* cmd, const Frame& frame, const Composition& composition,
                                        CVPixelBufferRef buffer)
{
    MTL::Texture* source = frame.texture.native();
    if (painter) {
        paint(cmd, frame, composition, painted.get());
        source = painted.get();
    }
    return converter->encode(cmd, source, buffer);
}

bool Recorder::Session::encodeBGRA(MTL::CommandBuffer* cmd, const Frame& frame, const Composition& composition,
                                   CVPixelBufferRef buffer, CVMetalTextureRef& texture)
{
    if (CVMetalTextureCacheCreateTextureFromImage(nullptr, cache, buffer, nullptr, MTLPixelFormatBGRA8Unorm, width,
                                                  height, 0, &texture) != kCVReturnSuccess)
        return false;
    auto* target = (__bridge MTL::Texture*)CVMetalTextureGetTexture(texture);
    if (painter) {
        paint(cmd, frame, composition, target);
    } else {
        auto* pass = MTL::BlitPassDescriptor::blitPassDescriptor();
        AZL_GPU_ZONE(gpu, pass, "record copy");
        MTL::BlitCommandEncoder* copy = cmd->blitCommandEncoder(pass);
        copy->copyFromTexture(frame.texture.native(), 0, 0, MTL::Origin(0, 0, 0), MTL::Size(width, height, 1), target,
                              0, 0, MTL::Origin(0, 0, 0));
        copy->endEncoding();
    }
    if (target->storageMode() == MTL::StorageModeManaged) {
        auto* pass = MTL::BlitPassDescriptor::blitPassDescriptor();
        AZL_GPU_ZONE(gpu, pass, "record sync");
        MTL::BlitCommandEncoder* sync = cmd->blitCommandEncoder(pass);
        sync->synchronizeResource(target);
        sync->endEncoding();
    }
    return true;
}

void Recorder::Session::paint(MTL::CommandBuffer* cmd, const Frame& frame, const Composition& composition,
                              MTL::Texture* target)
{
    Screen shown{.slices = {Slice{}}};
    if (screen)
        for (const Screen& s : composition.screens)
            if (s.id == *screen)
                shown = s;
    gpu::CommandBuffer painting = gpu::CommandBuffer::wrap(cmd);
    painter->encode(painting, &frame, screenView(composition, shown, false, std::nullopt),
                    gpu::Texture::retain(target));
}

Recorder::Recorder(Engine& engine, double fps, OnEnded onEnded, OnSpace onSpace)
    : engine_(engine), fps_(fps), onEnded_(std::move(onEnded)), onSpace_(std::move(onSpace)),
      space_(recording::volumeSpace)
{
}

Recorder::~Recorder()
{
    stop();
    std::lock_guard lock(control_);
    if (session_)
        session_->writer->join();
}

void Recorder::start(Settings settings, double now, std::optional<double> gigEnd)
{
    std::lock_guard control(control_);
    std::shared_ptr<Session> last;
    {
        std::lock_guard lock(mutex_);
        if (session_ && !session_->writer->done)
            throw std::runtime_error("already recording");
        last = session_;
    }
    if (last)
        last->writer->join();
    AudioPlan plan = audioPlan(settings.audio);
    AudioAnalysis* audio = nullptr;
    SpaceProvider space;
    std::chrono::milliseconds interval;
    {
        std::lock_guard lock(mutex_);
        audio = audio_;
        space = space_;
        interval = spaceInterval_;
    }
    auto session =
        std::make_shared<Session>(*this, std::move(settings), fps_, std::move(plan), std::move(space), interval);
    recording::Writer& writer = *session->writer;
    if (writer.audioPlan.state == "unavailable")
        std::printf("record: no audio track: %s\n", writer.audioPlan.reason.c_str());
    writer.start(now, gigEnd, writeDelay_, finishDelay_);
    {
        std::lock_guard lock(mutex_);
        session_ = session;
    }
    if (writer.audioRate > 0.0) {
        session->analysis = audio;
        audio->setTap(session.get(), [session](const float* samples, size_t count, double time, double rate) {
            session->writer->heard(samples, count, time, rate);
        });
    }
    engine_.setFrameTap(session);
}

void Recorder::stop()
{
    std::lock_guard control(control_);
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(mutex_);
        if (!session_ || session_->writer->done)
            return;
        session = session_;
    }
    engine_.setFrameTap(nullptr);
    removeAudioTap(*session);
    session->writer->waitIdle(true);
    session->writer->close();
    session->writer->join();
}

void Recorder::ended(Session& session, const Ended& ended)
{
    {
        std::lock_guard lock(mutex_);
        if (session_.get() == &session) {
            engine_.setFrameTap(nullptr);
            removeAudioTap(session);
        }
        session.writer->done = true;
    }
    if (onEnded_)
        onEnded_(ended);
}

Recorder::Status Recorder::status() const
{
    std::lock_guard lock(mutex_);
    Status status;
    if (!session_)
        return status;
    const recording::Writer& writer = *session_->writer;
    status.recording = !writer.done;
    status.settings = writer.settings;
    status.file = writer.currentFile();
    status.frames = writer.counts.frames;
    status.dropped = writer.counts.dropped;
    status.hardware = writer.hardware;
    status.pixels = session_->converter ? fourCC(session_->converter->format()) : "BGRA";
    status.audio = writer.audioPlan.state;
    status.audioReason = writer.audioPlan.reason;
    status.audioSource = writer.audioPlan.source;
    status.audioDropped = writer.audioDropped;
    status.space = writer.spaceLevel;
    status.freeBytes = writer.freeBytes;
    status.reserve = writer.reserve;
    status.stopsAt = writer.stopsAt;
    status.stopsFor = writer.stopsFor;
    return status;
}

std::optional<Recorder::Position> Recorder::position(double engineTime) const
{
    std::lock_guard lock(mutex_);
    if (!session_ || session_->writer->done)
        return std::nullopt;
    return session_->writer->position(engineTime);
}

void Recorder::waitUntilWritten()
{
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(mutex_);
        session = session_;
    }
    if (!session)
        return;
    session->writer->waitIdle(false);
    // The encoder lets go of what it was handed some time later.
    for (int i = 0; i < 1000 && !session->writer->done; ++i) {
        CVPixelBufferRef buffer = nullptr;
        if (CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(nullptr, session->pool,
                                                                (__bridge CFDictionaryRef)session->aux,
                                                                &buffer) == kCVReturnSuccess) {
            CFRelease(buffer);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void Recorder::setAudio(AudioAnalysis* audio, AudioSourceOf source)
{
    std::lock_guard lock(mutex_);
    audio_ = audio;
    audioSource_ = std::move(source);
}

Recorder::AudioPlan Recorder::audioPlan(bool wanted) const
{
    if (!wanted)
        return {};
    AudioAnalysis* audio = nullptr;
    AudioSourceOf sourceOf;
    {
        std::lock_guard lock(mutex_);
        audio = audio_;
        sourceOf = audioSource_;
    }
    if (!audio)
        return {.state = "unavailable", .reason = "no audio analysis"};
    const AudioSource source = sourceOf ? sourceOf() : AudioSource{.name = "audio input"};
    if (source.name.empty())
        return {.state = "unavailable", .reason = source.reason.empty() ? "no audio input running" : source.reason};
    if (audio->sampleRate() <= 0.0)
        return {.state = "unavailable", .reason = "no audio input running"};
    return {.state = "on", .source = source.name};
}

void Recorder::setSpaceCheck(SpaceProvider provider, std::chrono::milliseconds interval)
{
    std::lock_guard lock(mutex_);
    space_ = provider ? std::move(provider) : SpaceProvider(recording::volumeSpace);
    spaceInterval_ = interval;
}

uint64_t Recorder::reserveOf(const Settings& settings, uint64_t total)
{
    return recording::reserveOf(settings, total);
}

void Recorder::removeAudioTap(Session& session)
{
    if (session.writer->audioRate > 0.0 && session.analysis)
        session.analysis->setTap(&session, nullptr);
}

void Recorder::setWriteDelay(std::chrono::microseconds delay)
{
    std::lock_guard lock(mutex_);
    writeDelay_ = delay;
    if (session_)
        session_->writer->delayMicros = delay.count();
}

void Recorder::setRolloverDelays(std::chrono::microseconds finish, std::chrono::microseconds encoderSetup)
{
    std::lock_guard lock(mutex_);
    finishDelay_ = finish;
    encoderSetup_ = encoderSetup;
}

void Recorder::recover(const std::filesystem::path& folder)
{
    recording::recover(folder, playable);
}

std::string Recorder::folderProblem(const std::filesystem::path& path)
{
    return recording::folderProblem(path);
}

std::filesystem::path Recorder::movieFile(const std::filesystem::path& path)
{
    return recording::movieFile(path);
}

} // namespace azl

