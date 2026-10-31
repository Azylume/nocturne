#include "media/video_source.hpp"

#include "platform/thread_priority.hpp"
#include "media/hap_texture.hpp"

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <set>

extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void* pool);

namespace azl {
namespace {
std::atomic<int64_t> gLiveReaders{0};
char kSentinelKey;
} // namespace
} // namespace azl

// Rides on every AVAssetReader as an associated object, so it is freed with the reader.
@interface AZLReaderSentinel : NSObject
@end

@implementation AZLReaderSentinel
- (instancetype)init
{
    if ((self = [super init]))
        azl::gLiveReaders++;
    return self;
}
- (void)dealloc
{
    azl::gLiveReaders--;
}
@end

namespace azl {

namespace {

std::string describe(NSError* error)
{
    if (!error)
        return "unknown error";
    std::string text = error.localizedDescription.UTF8String;
    if (NSString* reason = error.localizedFailureReason)
        text += std::string(" (") + reason.UTF8String + ")";
    return text;
}

MediaStatus statusOf(NSError* error)
{
    const bool unsupported = [error.domain isEqualToString:AVFoundationErrorDomain] &&
                             (error.code == AVErrorFileFormatNotRecognized || error.code == AVErrorDecoderNotFound);
    return unsupported ? MediaStatus::Unsupported : MediaStatus::Corrupt;
}

struct Track {
    AVURLAsset* asset = nil;
    AVAssetTrack* track = nil;
    CMTime start = kCMTimeZero;
    double fps = 0.0;
    int frameCount = 0;
    int width = 0;
    int height = 0;
    bool fullRange = false;
    YuvMatrix matrix = YuvMatrix::Rec709;
    int turns = 0;
    // Read as stored, without VideoToolbox, when the track is HAP.
    HapCodec hap = HapCodec::None;
};

YuvMatrix matrixOf(CMFormatDescriptionRef format)
{
    CFTypeRef matrix = CMFormatDescriptionGetExtension(format, kCVImageBufferYCbCrMatrixKey);
    if (matrix && CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_601_4))
        return YuvMatrix::Rec601;
    if (matrix && CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_2020))
        return YuvMatrix::Rec2020;
    if (matrix)
        return YuvMatrix::Rec709;
    return CMVideoFormatDescriptionGetDimensions(format).height > 576 ? YuvMatrix::Rec709 : YuvMatrix::Rec601;
}

NSArray<AVAssetTrack*>* loadVideoTracks(AVURLAsset* asset, NSError** error)
{
    __block NSArray<AVAssetTrack*>* tracks = nil;
    __block NSError* loadError = nil;
    dispatch_semaphore_t loaded = dispatch_semaphore_create(0);
    [asset loadTracksWithMediaType:AVMediaTypeVideo
                 completionHandler:^(NSArray<AVAssetTrack*>* t, NSError* e) {
                   tracks = t;
                   loadError = e;
                   dispatch_semaphore_signal(loaded);
                 }];
    dispatch_semaphore_wait(loaded, DISPATCH_TIME_FOREVER);
    *error = loadError;
    return tracks;
}

// CoreMedia caches what it opened by URL, and an entry whose drive was removed stays broken after the drive is back:
// opening that path again fails with "Cannot Open". A path that opened before and fails now is opened once more,
// spelled differently ("folder/./name", then "folder/././name", ...), which CoreMedia takes for another file.
struct Spellings {
    std::mutex mutex;
    // Extra "./" per path, and the spellings that opened.
    std::map<std::string, int> dots;
    std::set<std::string> opened;
};
Spellings spellings;

std::string spelling(const std::string& path)
{
    std::lock_guard lock(spellings.mutex);
    const auto it = spellings.dots.find(path);
    const size_t slash = path.rfind('/');
    if (it == spellings.dots.end() || slash == std::string::npos)
        return path;
    std::string dots;
    for (int i = 0; i < it->second; ++i)
        dots += "./";
    return path.substr(0, slash + 1) + dots + path.substr(slash + 1);
}

void opened(const std::string& spelled)
{
    std::lock_guard lock(spellings.mutex);
    spellings.opened.insert(spelled);
}

// True when the spelling opened before, so a failure may be CoreMedia's cache: the path is spelled anew.
bool respell(const std::string& path, const std::string& spelled)
{
    std::lock_guard lock(spellings.mutex);
    if (!spellings.opened.count(spelled))
        return false;
    ++spellings.dots[path];
    return true;
}

Track openTrack(const std::string& path)
{
    std::string spelled = spelling(path);
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:spelled.c_str()]];
    Track t;
    t.asset = [AVURLAsset URLAssetWithURL:url options:@{AVURLAssetPreferPreciseDurationAndTimingKey : @YES}];
    NSError* error = nil;
    NSArray<AVAssetTrack*>* tracks = loadVideoTracks(t.asset, &error);
    if (error && respell(path, spelled)) {
        spelled = spelling(path);
        url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:spelled.c_str()]];
        t.asset = [AVURLAsset URLAssetWithURL:url options:@{AVURLAssetPreferPreciseDurationAndTimingKey : @YES}];
        error = nil;
        tracks = loadVideoTracks(t.asset, &error);
    }
    if (error)
        throw MediaError(statusOf(error), "cannot open: " + describe(error));
    opened(spelled);
    if (tracks.count == 0)
        throw MediaError(MediaStatus::Unsupported, "no video track");
    t.track = tracks.firstObject;

    t.fps = t.track.nominalFrameRate;
    if (!(t.fps > 0.0))
        throw MediaError(MediaStatus::Unsupported, "no frame rate");
    const CMTimeRange range = t.track.timeRange;
    t.start = range.start;
    t.frameCount = int(std::llround(CMTimeGetSeconds(range.duration) * t.fps));
    if (t.frameCount < 1)
        throw MediaError(MediaStatus::Corrupt, "no frames");

    NSNumber* fileSize = [[NSFileManager defaultManager] attributesOfItemAtPath:url.path error:nil][NSFileSize];
    if (fileSize && t.track.totalSampleDataLength > fileSize.longLongValue)
        throw MediaError(MediaStatus::Corrupt, "truncated: the file is smaller than the samples its index lists");

    auto format = (__bridge CMFormatDescriptionRef)t.track.formatDescriptions.firstObject;
    if (!format)
        throw MediaError(MediaStatus::Corrupt, "no format description");
    t.fullRange = CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_FullRangeVideo) == kCFBooleanTrue;
    t.matrix = matrixOf(format);
    t.hap = hapCodecOf(CMFormatDescriptionGetMediaSubType(format));
    const CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions(format);
    if (!pictureSizeOk(size.width, size.height))
        throw MediaError(MediaStatus::Unsupported,
                         "a picture of " + std::to_string(size.width) + "x" + std::to_string(size.height));
    t.width = size.width;
    t.height = size.height;
    const CGAffineTransform rotation = t.track.preferredTransform;
    t.turns = int(std::lround(std::atan2(rotation.b, rotation.a) / (M_PI / 2))) & 3;
    return t;
}

int frameIndex(const Track& t, CMTime pts)
{
    return int(std::floor(CMTimeGetSeconds(CMTimeSubtract(pts, t.start)) * t.fps + 0.25));
}

// Sample tables are in media time; the edit list (e.g. the delay B-frames add) maps it onto the track's timeline.
CMTime trackTime(const Track& t, CMTime mediaTime)
{
    for (AVAssetTrackSegment* segment in t.track.segments) {
        const CMTimeMapping mapping = segment.timeMapping;
        if (!segment.empty && CMTimeRangeContainsTime(mapping.source, mediaTime))
            return CMTimeMapTimeFromRangeToRange(mediaTime, mapping.source, mapping.target);
    }
    return mediaTime;
}

// The frames of the sync samples, from the sample table; always starts with frame 0.
std::vector<int> keyframesOf(const Track& t)
{
    std::vector<int> keys{0};
    AVSampleCursor* cursor = t.track.canProvideSampleCursors ? [t.track makeSampleCursorAtFirstSampleInDecodeOrder] : nil;
    for (bool more = cursor != nil; more; more = [cursor stepInDecodeOrderByCount:1] == 1) {
        const int index = frameIndex(t, trackTime(t, cursor.presentationTimeStamp));
        if (cursor.currentSampleSyncInfo.sampleIsFullSync && index > 0 && index < t.frameCount)
            keys.push_back(index);
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

// The sample's bytes, without a copy when they are contiguous; `scratch` holds them otherwise.
std::pair<const uint8_t*, size_t> sampleBytes(CMSampleBufferRef sample, std::vector<uint8_t>& scratch)
{
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    if (!block)
        return {nullptr, 0};
    const size_t length = CMBlockBufferGetDataLength(block);
    char* data = nullptr;
    if (CMBlockBufferIsRangeContiguous(block, 0, length) &&
        CMBlockBufferGetDataPointer(block, 0, nullptr, nullptr, &data) == kCMBlockBufferNoErr)
        return {reinterpret_cast<const uint8_t*>(data), length};
    scratch.resize(length);
    if (CMBlockBufferCopyDataBytes(block, 0, length, scratch.data()) != kCMBlockBufferNoErr)
        return {nullptr, 0};
    return {scratch.data(), length};
}

// A sample buffer and, when its bytes were not contiguous, their copy.
struct HeldSample {
    CMSampleBufferRef sample = nullptr;
    std::vector<uint8_t> scratch;

    ~HeldSample() { CFRelease(sample); }
};

// Decodes into IOSurface-backed biplanar 4:2:0 buffers in the stream's own range, so Metal can wrap them without a
// copy or a conversion. A HAP track's samples come as stored.
class AvfReader final : public SourceReader {
public:
    AvfReader(const Track& t, int fromFrame) : track_(t)
    {
        NSError* error = nil;
        reader_ = [AVAssetReader assetReaderWithAsset:t.asset error:&error];
        if (!reader_)
            throw MediaError(statusOf(error), describe(error));
        objc_setAssociatedObject(reader_, &kSentinelKey, [AZLReaderSentinel new], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        const OSType format = t.fullRange ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                                          : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        NSDictionary* settings = @{
            (id)kCVPixelBufferPixelFormatTypeKey : @(format),
            (id)kCVPixelBufferMetalCompatibilityKey : @YES,
            (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
        };
        output_ = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:t.track
                                                             outputSettings:t.hap == HapCodec::None ? settings : nil];
        output_.alwaysCopiesSampleData = NO;
        [reader_ addOutput:output_];
        // A quarter into the keyframe `fromFrame`, clear of timestamp rounding: any earlier, the range would begin in
        // the GOP before, which the reader would decode whole first.
        if (fromFrame > 0)
            reader_.timeRange = CMTimeRangeMake(
                CMTimeAdd(t.start, CMTimeMakeWithSeconds((fromFrame + 0.25) / t.fps, 600000)), kCMTimePositiveInfinity);
        if (![reader_ startReading])
            throw MediaError(statusOf(reader_.error), describe(reader_.error));
    }

    SourceRead read() override
    {
        @autoreleasepool {
            CMSampleBufferRef sample = [output_ copyNextSampleBuffer];
            // Marker buffers (no samples) come with the samples of a track read as stored.
            while (sample && CMSampleBufferGetNumSamples(sample) == 0) {
                CFRelease(sample);
                sample = [output_ copyNextSampleBuffer];
            }
            if (!sample)
                return std::nullopt;
            auto held = std::make_shared<HeldSample>();
            held->sample = sample;
            SourceSample read;
            read.index = frameIndex(track_, CMSampleBufferGetPresentationTimeStamp(sample));
            if (track_.hap != HapCodec::None)
                std::tie(read.bytes, read.size) = sampleBytes(sample, held->scratch);
            else if (CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer(sample))
                read.picture = std::shared_ptr<const void>(CVPixelBufferRetain(pixels),
                                                           [](const void* p) { CVPixelBufferRelease((CVPixelBufferRef)p); });
            read.owner = std::move(held);
            return read;
        }
    }

    void cancel() override { [reader_ cancelReading]; }

    std::optional<MediaError> failure() const override
    {
        if (reader_.status == AVAssetReaderStatusCompleted)
            return std::nullopt;
        return MediaError(statusOf(reader_.error), describe(reader_.error));
    }

private:
    Track track_;
    AVAssetReader* reader_ = nil;
    AVAssetReaderTrackOutput* output_ = nil;
};

struct HapTextures {
    std::array<MTL::Texture*, 2> textures{};

    ~HapTextures()
    {
        for (MTL::Texture* texture : textures)
            if (texture)
                CFRelease((CFTypeRef)texture);
    }
};

struct FrameResources {
    CVPixelBufferRef pixels = nullptr;
    CVMetalTextureRef luma = nullptr;
    CVMetalTextureRef chroma = nullptr;
    CVMetalTextureCacheRef cache = nullptr;

    ~FrameResources()
    {
        if (luma)
            CFRelease(luma);
        if (chroma)
            CFRelease(chroma);
        if (pixels)
            CVPixelBufferRelease(pixels);
        if (cache)
            CFRelease(cache);
    }
};

CVMetalTextureRef planeTexture(CVMetalTextureCacheRef cache, CVPixelBufferRef pixels, size_t plane,
                               MTLPixelFormat format)
{
    CVMetalTextureRef texture = nullptr;
    CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, cache, pixels, nullptr, format,
                                              CVPixelBufferGetWidthOfPlane(pixels, plane),
                                              CVPixelBufferGetHeightOfPlane(pixels, plane), plane, &texture);
    return texture;
}

class AvfSource final : public VideoSource {
public:
    // Without a device (probing) the source makes no frames.
    AvfSource(MTL::Device* device, const std::string& path) : track_(openTrack(path)), device_(device)
    {
        source_.fps = track_.fps;
        source_.frameCount = track_.frameCount;
        source_.width = track_.width;
        source_.height = track_.height;
        source_.turns = track_.turns;
        source_.hap = track_.hap;
        source_.pictureBytes = int64_t(track_.width) * track_.height * 3 / 2;
        yuvToRgb_ = yuvToRgbRows(track_.matrix, track_.fullRange);
        if (!device)
            return;
        source_.keyframes = keyframesOf(track_);
        if (CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, (__bridge id<MTLDevice>)device, nullptr,
                                      &cache_) != kCVReturnSuccess)
            throw std::runtime_error("cannot create a Metal texture cache");
    }

    ~AvfSource() override
    {
        if (cache_)
            CFRelease(cache_);
    }

    const SourceTrack& track() const override { return source_; }

    std::shared_ptr<SourceReader> startReading(int fromFrame) override
    {
        return std::make_shared<AvfReader>(track_, fromFrame);
    }

    VideoFrameRef frame(const std::shared_ptr<const void>& picture, int index) override
    {
        auto pixels = (CVPixelBufferRef)picture.get();
        auto resources = std::make_shared<FrameResources>();
        resources->pixels = CVPixelBufferRetain(pixels);
        resources->cache = (CVMetalTextureCacheRef)CFRetain(cache_);
        resources->luma = planeTexture(cache_, pixels, 0, MTLPixelFormatR8Unorm);
        resources->chroma = planeTexture(cache_, pixels, 1, MTLPixelFormatRG8Unorm);
        CVMetalTextureCacheFlush(cache_, 0);
        if (!resources->luma || !resources->chroma)
            return nullptr;
        auto frame = std::make_shared<VideoFrame>();
        frame->index = index;
        frame->luma = (__bridge MTL::Texture*)CVMetalTextureGetTexture(resources->luma);
        frame->chroma = (__bridge MTL::Texture*)CVMetalTextureGetTexture(resources->chroma);
        frame->yuvToRgb = yuvToRgb_;
        frame->turns = track_.turns;
        frame->resources = std::move(resources);
        return frame;
    }

    VideoFrameRef hapFrame(const std::vector<HapImage>& images, PictureFormat format, int index) override
    {
        auto resources = std::make_shared<HapTextures>();
        for (size_t i = 0; i < images.size(); ++i)
            if (!(resources->textures[i] = newHapTexture(device_, images[i], track_.width, track_.height)))
                return nullptr;
        auto frame = std::make_shared<VideoFrame>();
        frame->index = index;
        frame->luma = resources->textures[0];
        frame->chroma = resources->textures[1] ? resources->textures[1] : resources->textures[0];
        frame->format = format;
        frame->turns = track_.turns;
        frame->resources = std::move(resources);
        return frame;
    }

private:
    Track track_;
    SourceTrack source_;
    MTL::Device* device_ = nullptr;
    CVMetalTextureCacheRef cache_ = nullptr;
    std::array<float, 12> yuvToRgb_{};
};

} // namespace

int64_t liveAssetReaders()
{
    return gLiveReaders.load();
}

std::unique_ptr<VideoSource> openVideoSource(MediaDevice* device, const std::string& path)
{
    return std::make_unique<AvfSource>(device, path);
}

void runRead(std::function<void()> read)
{
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      @autoreleasepool {
          read();
      }
    });
}

void becomeDecodeThread()
{
    setThreadRole(ThreadRole::Decode);
}

FrameworkScope::FrameworkScope() : pool_(objc_autoreleasePoolPush()) {}

FrameworkScope::~FrameworkScope()
{
    objc_autoreleasePoolPop(pool_);
}

} // namespace azl
