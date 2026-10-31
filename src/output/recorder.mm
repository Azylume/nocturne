#include "output/recorder.hpp"

#include "engine/screen_painter.hpp"
#include "output/ycbcr_converter.hpp"

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <VideoToolbox/VideoToolbox.h>

#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <deque>
#include <memory>
#include <future>
#include <stdexcept>
#include <thread>
#include <utility>

namespace azl {

namespace {

// Bounds the frames between the render thread and the file: those being filled, queued and held by the encoder.
constexpr int kBuffers = 8;
constexpr uint64_t kAlmostFull = uint64_t(1) << 20;
// Bounds the audio waiting for the writer; more is dropped and counted.
constexpr double kAudioBacklogSeconds = 2.0;
// A capture timestamp this far past where the track has got to starts a gap; anything closer is clock jitter.
constexpr double kAudioGapSeconds = 0.05;
// AAC takes it from any input, and it matches video work.
constexpr double kTrackRate = 48000.0;
// A movie cut off before it is finished still plays up to its last fragment.
constexpr double kFragmentSeconds = 2.0;
constexpr const char* kPartialSuffix = ".partial.mov";
// The writer checks the free space itself after this much movie, so a busy machine that delays the checking thread
// cannot let it write past the reserve.
constexpr double kSpaceCheckSeconds = 0.25;

// Converts the input's chunks to kTrackRate through a windowed-sinc low-pass below both rates' Nyquist, so a faster
// input does not alias. The stream is taken as continuous until the rate changes or a chunk's time leaves it.
class Resampler {
public:
    // Appends a chunk and returns the samples it completes, the first at `time` (input clock).
    void push(const float* samples, size_t count, double time, double rate, std::vector<float>& out, double& outTime)
    {
        out.clear();
        if (rate == kTrackRate) {
            rate_ = rate;
            out.assign(samples, samples + count);
            outTime = time;
            return;
        }
        if (rate != rate_ || std::abs(time - (anchor_ + double(base_ + int64_t(in_.size())) / rate_)) > kAudioGapSeconds)
            reset(rate, time);
        in_.insert(in_.end(), samples, samples + count);
        outTime = anchor_ + pos_ / rate_;
        const int64_t end = base_ + int64_t(in_.size());
        while (int64_t(std::floor(pos_)) + half_ < end) {
            out.push_back(at(pos_));
            pos_ += rate_ / kTrackRate;
        }
        const int64_t keep = int64_t(std::floor(pos_)) - half_;
        if (keep > base_) {
            in_.erase(in_.begin(), in_.begin() + (keep - base_));
            base_ = keep;
        }
    }

private:
    static constexpr int kZeroCrossings = 16;
    static constexpr int kSteps = 256;

    void reset(double rate, double time)
    {
        rate_ = rate;
        anchor_ = time;
        base_ = 0;
        pos_ = 0.0;
        in_.clear();
        const double cutoff = 0.95 * std::min(1.0, kTrackRate / rate);
        half_ = int(std::ceil(kZeroCrossings / cutoff));
        kernel_.assign(size_t(half_ * kSteps + 2), 0.0f);
        for (size_t i = 0; i + 1 < kernel_.size(); ++i) {
            const double x = double(i) / kSteps;
            const double sinc = x == 0.0 ? 1.0 : std::sin(M_PI * cutoff * x) / (M_PI * cutoff * x);
            const double w = x / half_;
            const double blackman = 0.42 + 0.5 * std::cos(M_PI * w) + 0.08 * std::cos(2.0 * M_PI * w);
            kernel_[i] = float(cutoff * sinc * blackman);
        }
    }

    float at(double pos) const
    {
        const int64_t centre = int64_t(std::floor(pos));
        double sum = 0.0;
        for (int64_t k = std::max(centre - half_ + 1, base_); k <= centre + half_; ++k) {
            const double d = std::abs(pos - double(k)) * kSteps;
            const auto i = size_t(d);
            if (i + 1 >= kernel_.size())
                continue;
            const double f = d - double(i);
            sum += double(in_[size_t(k - base_)]) * (kernel_[i] + f * (kernel_[i + 1] - kernel_[i]));
        }
        return float(sum);
    }

    double rate_ = 0.0;
    double anchor_ = 0.0;
    // The input index of in_[0], and of the next output sample.
    int64_t base_ = 0;
    double pos_ = 0.0;
    int half_ = 0;
    std::vector<float> in_;
    std::vector<float> kernel_;
};

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

Recorder::Space volumeSpace(const std::filesystem::path& dir)
{
    struct statvfs s {};
    if (statvfs(dir.c_str(), &s) != 0)
        return {};
    return {uint64_t(s.f_bavail) * uint64_t(s.f_frsize), uint64_t(s.f_blocks) * uint64_t(s.f_frsize)};
}

std::filesystem::path numbered(const std::filesystem::path& file, int number)
{
    char suffix[16];
    std::snprintf(suffix, sizeof suffix, " %03d", number);
    return file.parent_path() / (file.stem().string() + suffix + file.extension().string());
}

std::filesystem::path partialOf(const std::filesystem::path& file)
{
    return file.parent_path() / ("." + file.stem().string() + kPartialSuffix);
}

std::string megabytes(uint64_t bytes)
{
    return std::to_string(bytes >> 20) + " MB";
}

// Renames a finished movie into place; removes the partial file of one that did not complete.
bool placeMovie(AVAssetWriter* writer, const std::filesystem::path& partial, const std::filesystem::path& file)
{
    std::error_code error;
    if (writer.status == AVAssetWriterStatusCompleted && std::rename(partial.c_str(), file.c_str()) == 0)
        return true;
    std::filesystem::remove(partial, error);
    return false;
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

std::string timestamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm local {};
    localtime_r(&now, &local);
    char text[32];
    std::strftime(text, sizeof text, "%Y-%m-%d %H.%M.%S", &local);
    return text;
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

} // namespace

struct Recorder::Session : FrameTap, std::enable_shared_from_this<Session> {
    struct Item {
        CVPixelBufferRef buffer;
        double seconds;
    };
    struct Heard {
        std::vector<float> samples;
        double time;
        double rate;
    };
    // The frames handed to the encoder for one segment and not yet in its movie.
    struct Track {
        AVAssetWriter* writer = nil;
        AVAssetWriterInput* input = nil;
        std::mutex mutex;
        std::condition_variable drained;
        int pending = 0;
    };
    // What the encoder's output callback gets with each frame.
    struct Routed {
        std::shared_ptr<Track> track;
        double origin;
    };
    // One movie file of the recording.
    struct Segment {
        AVAssetWriter* writer = nil;
        AVAssetWriterInput* input = nil;
        AVAssetWriterInput* audioInput = nil;
        std::shared_ptr<Track> track;
        std::filesystem::path file;
        std::filesystem::path partial;
        // The engine time of its first frame, which is time 0 of its movie.
        std::optional<double> origin;
        // The next sample its audio track takes.
        int64_t audioNext = -1;
        uint64_t frames = 0;
        // The recording's dropped count when it took over.
        uint64_t droppedBefore = 0;
    };

    Session(Recorder& owner, Settings s, double fps, AudioPlan audio, SpaceProvider space,
            std::chrono::milliseconds spaceInterval);
    ~Session() override;

    Done encode(gpu::CommandBuffer& cmd, const Frame& frame, const Composition& composition, double seconds) override;
    bool encodeConverted(MTL::CommandBuffer* cmd, const Frame& frame, const Composition& composition,
                         CVPixelBufferRef buffer);
    bool encodeBGRA(MTL::CommandBuffer* cmd, const Frame& frame, const Composition& composition, CVPixelBufferRef buffer,
                    CVMetalTextureRef& texture);
    void paint(MTL::CommandBuffer* cmd, const Frame& frame, const Composition& composition, MTL::Texture* target);
    void queued(CVPixelBufferRef buffer, double seconds, bool ok);
    Segment openSegment(const std::filesystem::path& movie);
    bool rollDue(double seconds) const;
    bool stopDue(double seconds) const;
    bool spaceCheckDue(double seconds);
    void prepare();
    bool roll(std::string& reason);
    void retire(Segment old);
    void discardPrepared();
    void run();
    void watch();
    bool spaceFull();
    bool write(const Item& item, bool keyFrame);
    static void encoded(void* session, void* frame, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample);
    bool append(const Routed& routed, CMSampleBufferRef sample);
    void heard(const float* samples, size_t count, double time, double sampleRate);
    bool writeAudio(const Heard& heard);
    bool placeAudio(const std::vector<float>& samples, double time);
    void finish(std::string reason);
    void close();
    void waitIdle(bool inFlightOnly);

    Recorder& owner;
    Settings settings;
    double fps = 0.0;
    // The movie the settings name; numbered per segment when the recording rolls over.
    std::filesystem::path base;
    std::filesystem::path folder;
    int number = 1;
    // Writer thread only: the engine time of its last space check.
    std::optional<double> spaceCheckedAt;
    // The wall clock at the start, and when and why the recording finishes by itself.
    double startedAt = 0.0;
    std::optional<double> stopsAt;
    std::string stopsFor;
    // Writer thread only, but `file`, which status reads, changes under `mutex`.
    Segment segment;
    std::filesystem::path file;
    uint32_t width = 0;
    uint32_t height = 0;
    std::optional<uint64_t> screen;
    std::optional<bool> hardware;
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
    // kTrackRate, or 0 without an audio track.
    double audioRate = 0.0;
    AudioPlan audioPlan;
    // Writer thread only.
    Resampler resampler;
    std::vector<float> resampled;
    AudioAnalysis* analysis = nullptr;
    CMAudioFormatDescriptionRef audioFormat = nullptr;
    Compressor compressor;
    // An encoded frame could not be written.
    std::atomic<bool> videoFailed{false};
    // The next numbered file, opened while the current one is written. Writer thread only.
    std::future<Segment> prepared;
    // Segments rolled over and still being finished.
    dispatch_group_t retiring = dispatch_group_create();

    SpaceProvider space;
    std::chrono::milliseconds spaceInterval;
    std::atomic<SpaceLevel> spaceLevel{SpaceLevel::Unknown};
    std::atomic<uint64_t> freeBytes{0};
    std::atomic<uint64_t> reserve{0};

    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    std::deque<Item> queue;
    std::deque<Heard> audio;
    size_t audioQueued = 0;
    // The engine time of the first recorded frame, which is time 0 of the movie.
    std::optional<double> origin;
    // Each file with the engine time of its first frame, and the engine time of the last frame written.
    std::vector<std::pair<std::filesystem::path, double>> starts;
    std::optional<double> written;
    int inFlight = 0;
    bool writing = false;
    bool closing = false;
    // Why the recording ends when something other than stop closed it.
    std::string closedFor;
    std::atomic<bool> done{false};
    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> audioDropped{0};
    std::atomic<int64_t> delayMicros{0};
    std::atomic<int64_t> finishDelayMicros{0};
    std::atomic<int64_t> encoderSetupMicros{0};
    // With the harness's encoder setup stand-in: the hardware every encoder shares.
    std::mutex encoderHardware;
    std::thread thread;
    std::thread watcher;
};

Recorder::Session::Session(Recorder& owner, Settings s, double fps, AudioPlan audio, SpaceProvider space,
                           std::chrono::milliseconds spaceInterval)
    : owner(owner), settings(std::move(s)), fps(fps), audioRate(audio.state == "on" ? kTrackRate : 0.0),
      audioPlan(std::move(audio)), space(std::move(space)),
      spaceInterval(spaceInterval)
{
    Engine& engine = owner.engine_;
    const Codec codec = codecOf(settings.codec);
    screen = screenOf(settings.target, *engine.composition());
    const uint32_t compositionWidth = engine.config().width;
    const uint32_t compositionHeight = engine.config().height;
    width = (settings.width ? settings.width : compositionWidth) & ~1u;
    height = (settings.height ? settings.height : compositionHeight) & ~1u;
    if (width < 16 || height < 16 || width > 8192 || height > 8192)
        throw std::runtime_error("recording size out of range");
    const bool whole = !screen && width == compositionWidth && height == compositionHeight;

    base = movieFile(settings.path);
    if (base.extension() != ".mov")
        throw std::runtime_error("recordings are .mov files");
    folder = base.parent_path();
    file = settings.maxBytes || settings.maxSeconds > 0.0 ? numbered(base, number) : base;
    if (const std::string problem = folderProblem(settings.path); !problem.empty())
        throw std::runtime_error(problem);
    std::error_code error;
    if (settings.path.empty())
        std::filesystem::create_directories(folder, error);
    if (std::filesystem::exists(file))
        throw std::runtime_error(file.string() + " exists");
    const Space room = this->space(folder);
    const uint64_t floor = reserveOf(settings, room.total);
    if (room.free < floor)
        throw std::runtime_error("too little disk space: " + megabytes(room.free) + " free at " + folder.string() +
                                 ", below the recording reserve of " + megabytes(floor));
    const Encoder encoder = askEncoder(codec.toolbox, width, height, settings.precise);
    hardware = encoder.hardware;
    NSDictionary* bufferAttributes = azl::bufferAttributes(encoder.native, width, height, settings.precise);
    const OSType format = [bufferAttributes[(__bridge NSString*)kCVPixelBufferPixelFormatTypeKey] unsignedIntValue];
    gpu = &engine.gpuTimeline();
    if (format != kCVPixelFormatType_32BGRA) {
        converter = std::make_unique<YCbCrConverter>(engine.device(), format);
        converter->setGpuTimeline(gpu);
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

    if (audioRate > 0.0) {
        AudioStreamBasicDescription pcm{.mSampleRate = audioRate,
                                        .mFormatID = kAudioFormatLinearPCM,
                                        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
                                        .mBytesPerPacket = 4,
                                        .mFramesPerPacket = 1,
                                        .mBytesPerFrame = 4,
                                        .mChannelsPerFrame = 1,
                                        .mBitsPerChannel = 32};
        CMAudioFormatDescriptionCreate(nullptr, &pcm, 0, nullptr, 0, nullptr, nullptr, &audioFormat);
    }
    encoderSetupMicros = owner.encoderSetup_.count();
    if (const int64_t stall = encoderSetupMicros.load()) {
        std::lock_guard hardware(encoderHardware);
        std::this_thread::sleep_for(std::chrono::microseconds(stall));
    }
    compressor = makeCompressor(settings.codec, width, height, fps, settings.precise, encoded, this);
    segment = openSegment(file);
    if (audioRate > 0.0 && !segment.audioInput) {
        audioRate = 0.0;
        audioPlan.state = "unavailable";
        audioPlan.reason = "the movie writer refuses AAC at 48 kHz";
    }

    NSDictionary* poolAttributes = @{(__bridge NSString*)kCVPixelBufferPoolMinimumBufferCountKey : @(kBuffers),
                                     (__bridge NSString*)kCVPixelBufferPoolMaximumBufferAgeKey : @0};
    aux = @{(__bridge NSString*)kCVPixelBufferPoolAllocationThresholdKey : @(kBuffers)};
    if (CVPixelBufferPoolCreate(nullptr, (__bridge CFDictionaryRef)poolAttributes,
                                (__bridge CFDictionaryRef)bufferAttributes, &pool) != kCVReturnSuccess ||
        CVMetalTextureCacheCreate(nullptr, nullptr, (__bridge id<MTLDevice>)engine.device(), nullptr, &cache) !=
            kCVReturnSuccess) {
        [segment.writer cancelWriting];
        std::filesystem::remove(segment.partial, error);
        throw std::runtime_error("cannot allocate recording buffers");
    }
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
    if (watcher.joinable())
        watcher.join();
    if (thread.joinable())
        thread.join();
    if (audioFormat)
        CFRelease(audioFormat);
    if (cache)
        CFRelease(cache);
    if (pool)
        CVPixelBufferPoolRelease(pool);
}

Recorder::Session::Segment Recorder::Session::openSegment(const std::filesystem::path& movie)
{
    Segment next{.file = movie, .partial = partialOf(movie)};
    std::error_code error;
    std::filesystem::remove(next.partial, error);
    NSError* failure = nil;
    AVAssetWriter* writer = [AVAssetWriter assetWriterWithURL:[NSURL fileURLWithPath:@(next.partial.c_str())]
                                                     fileType:AVFileTypeQuickTimeMovie
                                                        error:&failure];
    if (!writer)
        throw std::runtime_error("cannot write " + next.partial.string() + ": " + describe(failure));
    writer.movieFragmentInterval = CMTimeMakeWithSeconds(kFragmentSeconds, 600);
    next.writer = writer;
    next.input = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:nil];
    next.input.expectsMediaDataInRealTime = YES;
    [writer addInput:next.input];
    next.track = std::make_shared<Track>();
    next.track->writer = writer;
    next.track->input = next.input;
    NSDictionary* aac = @{AVFormatIDKey : @(kAudioFormatMPEG4AAC),
                          AVSampleRateKey : @(audioRate),
                          AVNumberOfChannelsKey : @1,
                          AVEncoderBitRateKey : @128000};
    if (audioRate > 0.0 && audioFormat && [writer canApplyOutputSettings:aac forMediaType:AVMediaTypeAudio]) {
        next.audioInput = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio
                                                             outputSettings:aac
                                                           sourceFormatHint:audioFormat];
        next.audioInput.expectsMediaDataInRealTime = YES;
        [writer addInput:next.audioInput];
    }
    if (![writer startWriting])
        throw std::runtime_error("cannot start the movie: " + describe(writer.error));
    [writer startSessionAtSourceTime:kCMTimeZero];
    return next;
}

FrameTap::Done Recorder::Session::encode(gpu::CommandBuffer& cmd, const Frame& frame, const Composition& composition,
                                         double seconds)
{
    CVPixelBufferRef buffer = nullptr;
    if (CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(nullptr, pool, (__bridge CFDictionaryRef)aux, &buffer) !=
        kCVReturnSuccess) {
        dropped++;
        return nullptr;
    }
    CVMetalTextureRef texture = nullptr;
    const bool encoded = converter ? encodeConverted(cmd.native(), frame, composition, buffer)
                                   : encodeBGRA(cmd.native(), frame, composition, buffer, texture);
    if (!encoded) {
        CFRelease(buffer);
        dropped++;
        return nullptr;
    }
    {
        std::lock_guard lock(mutex);
        inFlight++;
    }
    return [self = shared_from_this(), buffer, texture, seconds](bool ok) {
        if (texture)
            CFRelease(texture);
        self->queued(buffer, seconds, ok);
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

void Recorder::Session::queued(CVPixelBufferRef buffer, double seconds, bool ok)
{
    std::lock_guard lock(mutex);
    inFlight--;
    if (ok && !closing) {
        queue.push_back({buffer, seconds});
        wake.notify_all();
    } else {
        CFRelease(buffer);
    }
    idle.notify_all();
}

void Recorder::Session::run()
{
    std::string reason = "stopped";
    prepare();
    for (;;) {
        std::optional<Item> item;
        std::deque<Heard> sound;
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return !queue.empty() || closing || (origin && !audio.empty()); });
            if (queue.empty() && (closing || !origin))
                break;
            if (!queue.empty()) {
                item = queue.front();
                queue.pop_front();
                if (!origin)
                    origin = item->seconds;
            }
            sound.swap(audio);
            audioQueued = 0;
            writing = true;
        }
        bool ok = true;
        bool rollOk = true;
        const bool due = item && stopDue(item->seconds);
        const bool full = item && !due && spaceCheckDue(item->seconds) && spaceFull();
        if (item && !due && !full) {
            const bool rolls = rollDue(item->seconds);
            rollOk = !rolls || roll(reason);
            ok = rollOk && write(*item, rolls);
        }
        if (item)
            CFRelease(item->buffer);
        for (const Heard& h : sound)
            ok = ok && writeAudio(h);
        {
            std::lock_guard lock(mutex);
            if (item)
                written = item->seconds;
            writing = false;
            idle.notify_all();
        }
        if (due) {
            reason = stopsFor;
            break;
        }
        if (full) {
            reason = "disk full";
            break;
        }
        if (!rollOk)
            break;
        if (!ok) {
            reason = diskFull(segment.writer.error) || volumeSpace(folder).free < kAlmostFull
                         ? "disk full"
                         : "failed: " + describe(segment.writer.error);
            break;
        }
    }
    {
        std::lock_guard lock(mutex);
        if (!closedFor.empty())
            reason = closedFor;
    }
    finish(reason);
    watcher.join();
}

bool Recorder::Session::rollDue(double seconds) const
{
    if (segment.frames == 0)
        return false;
    if (settings.maxSeconds > 0.0 && seconds - *segment.origin >= settings.maxSeconds - 1e-6)
        return true;
    if (settings.maxBytes == 0)
        return false;
    std::error_code error;
    const uintmax_t size = std::filesystem::file_size(segment.partial, error);
    return !error && size >= settings.maxBytes;
}

bool Recorder::Session::spaceCheckDue(double seconds)
{
    if (spaceCheckedAt && seconds - *spaceCheckedAt < kSpaceCheckSeconds)
        return false;
    spaceCheckedAt = seconds;
    return true;
}

bool Recorder::Session::stopDue(double seconds) const
{
    return stopsAt && startedAt + (seconds - *origin) >= *stopsAt - 1e-6;
}

// With a maximum size or length, opens the next numbered file in the background, so a rollover only swaps writers.
void Recorder::Session::prepare()
{
    if (!settings.maxBytes && settings.maxSeconds <= 0.0)
        return;
    prepared = std::async(std::launch::async, [this, next = numbered(base, number + 1)] {
        if (std::filesystem::exists(next))
            throw std::runtime_error(next.string() + " exists");
        return openSegment(next);
    });
}

bool Recorder::Session::roll(std::string& reason)
{
    const auto began = std::chrono::steady_clock::now();
    Segment opened;
    try {
        opened = prepared.get();
    } catch (const std::exception& e) {
        reason = std::string("failed: ") + e.what();
        return false;
    }
    const uint64_t droppedNow = dropped;
    std::printf("record: %s is full (%llu frames, %llu dropped), going on in %s after %.1f ms\n", segment.file.c_str(),
                (unsigned long long)segment.frames, (unsigned long long)(droppedNow - segment.droppedBefore),
                opened.file.c_str(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count());
    opened.droppedBefore = droppedNow;
    retire(std::exchange(segment, std::move(opened)));
    {
        std::lock_guard lock(mutex);
        number++;
        file = segment.file;
    }
    prepare();
    return true;
}

// Finishes a full segment on another queue; the movie is renamed into place once complete.
void Recorder::Session::retire(Segment old)
{
    AVAssetWriter* writer = old.writer;
    AVAssetWriterInput* input = old.input;
    AVAssetWriterInput* audioInput = old.audioInput;
    const std::shared_ptr<Track> track = old.track;
    const std::filesystem::path partial = old.partial;
    const std::filesystem::path movie = old.file;
    const int64_t delay = finishDelayMicros.load();
    dispatch_group_t group = retiring;
    dispatch_group_async(group, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        if (delay)
            std::this_thread::sleep_for(std::chrono::microseconds(delay));
        {
            std::unique_lock lock(track->mutex);
            track->drained.wait(lock, [&] { return track->pending == 0; });
        }
        [input markAsFinished];
        [audioInput markAsFinished];
        dispatch_group_enter(group);
        [writer finishWritingWithCompletionHandler:^{
            if (!placeMovie(writer, partial, movie))
                std::printf("record: could not finish %s: %s\n", movie.c_str(), describe(writer.error).c_str());
            dispatch_group_leave(group);
        }];
    });
}

// The file opened for a rollover that never came.
void Recorder::Session::discardPrepared()
{
    if (!prepared.valid())
        return;
    try {
        Segment spare = prepared.get();
        [spare.writer cancelWriting];
        std::error_code error;
        std::filesystem::remove(spare.partial, error);
    } catch (const std::exception&) {
    }
}

// Checks the free space every interval until the recording closes, and closes it below the reserve.
void Recorder::Session::watch()
{
    std::unique_lock lock(mutex);
    while (!closing) {
        wake.wait_for(lock, spaceInterval, [&] { return closing; });
        if (closing)
            return;
        lock.unlock();
        const bool full = spaceFull();
        lock.lock();
        if (full) {
            closedFor = "disk full";
            closing = true;
            wake.notify_all();
            idle.notify_all();
        }
    }
}

bool Recorder::Session::spaceFull()
{
    const Space room = space(folder);
    const uint64_t floor = reserveOf(settings, room.total);
    const SpaceLevel level = room.free < floor       ? SpaceLevel::Full
                             : room.free / 2 < floor ? SpaceLevel::Low
                                                     : SpaceLevel::Ok;
    freeBytes = room.free;
    reserve = floor;
    const SpaceLevel last = spaceLevel.exchange(level);
    if (level == last || (last == SpaceLevel::Unknown && level == SpaceLevel::Ok))
        return level == SpaceLevel::Full;
    if (level == SpaceLevel::Full)
        std::printf("record: %s free at %s, below the reserve of %s: stopping\n", megabytes(room.free).c_str(),
                    folder.c_str(), megabytes(floor).c_str());
    else if (level == SpaceLevel::Low)
        std::printf("record: disk space low, %s free at %s; the recording stops below %s\n",
                    megabytes(room.free).c_str(), folder.c_str(), megabytes(floor).c_str());
    else
        std::printf("record: disk space back to %s free at %s\n", megabytes(room.free).c_str(), folder.c_str());
    owner.spaceChanged();
    return level == SpaceLevel::Full;
}

bool Recorder::Session::write(const Item& item, bool keyFrame)
{
    if (const int64_t delay = delayMicros.load())
        std::this_thread::sleep_for(std::chrono::microseconds(delay));
    if (videoFailed)
        return false;
    if (!segment.origin) {
        segment.origin = item.seconds;
        std::lock_guard lock(mutex);
        starts.emplace_back(segment.file, item.seconds);
    }
    auto routed = std::make_unique<Routed>(Routed{segment.track, *segment.origin});
    {
        std::lock_guard lock(segment.track->mutex);
        segment.track->pending++;
    }
    NSDictionary* options = keyFrame ? @{(__bridge NSString*)kVTEncodeFrameOptionKey_ForceKeyFrame : @YES} : nil;
    std::unique_lock<std::mutex> hardware;
    if (encoderSetupMicros.load())
        hardware = std::unique_lock(encoderHardware);
    const CMTime time = CMTimeMake(int64_t(std::llround(item.seconds * 1e6)), 1000000);
    if (VTCompressionSessionEncodeFrame(compressor.get(), item.buffer, time, kCMTimeInvalid,
                                        (__bridge CFDictionaryRef)options, routed.get(), nullptr) != noErr) {
        std::lock_guard lock(segment.track->mutex);
        segment.track->pending--;
        segment.track->drained.notify_all();
        return false;
    }
    routed.release();
    segment.frames++;
    return true;
}

// The encoder's output, in the order the frames went in: each lands in the movie of the segment it was handed for.
void Recorder::Session::encoded(void* session, void* frame, OSStatus status, VTEncodeInfoFlags flags,
                                CMSampleBufferRef sample)
{
    auto* self = static_cast<Session*>(session);
    const std::unique_ptr<Routed> routed(static_cast<Routed*>(frame));
    if (status != noErr)
        self->videoFailed = true;
    else if (!sample || (flags & kVTEncodeInfo_FrameDropped))
        self->dropped++;
    else if (self->append(*routed, sample))
        self->frames++;
    else
        self->videoFailed = true;
    Track& track = *routed->track;
    std::lock_guard lock(track.mutex);
    track.pending--;
    track.drained.notify_all();
}

bool Recorder::Session::append(const Routed& routed, CMSampleBufferRef sample)
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

void Recorder::Session::heard(const float* samples, size_t count, double time, double sampleRate)
{
    std::lock_guard lock(mutex);
    if (closing || sampleRate <= 0.0)
        return;
    if (double(audioQueued + count) > kAudioBacklogSeconds * sampleRate) {
        audioDropped += count;
        return;
    }
    audio.push_back({std::vector<float>(samples, samples + count), time, sampleRate});
    audioQueued += count;
    wake.notify_all();
}

bool Recorder::Session::writeAudio(const Heard& heard)
{
    if (!segment.audioInput || !segment.origin)
        return true;
    double time = 0.0;
    resampler.push(heard.samples.data(), heard.samples.size(), heard.time, heard.rate, resampled, time);
    return resampled.empty() || placeAudio(resampled, time);
}

// Places samples at their capture time on the movie's clock, the one the frames are on, and keeps the track continuous
// through timestamp jitter.
bool Recorder::Session::placeAudio(const std::vector<float>& samples, double time)
{
    int64_t& audioNext = segment.audioNext;
    const auto start = int64_t(std::llround((time - *segment.origin) * audioRate));
    if (audioNext < 0 || start > audioNext + int64_t(kAudioGapSeconds * audioRate))
        audioNext = std::max<int64_t>(start, 0);
    const int64_t skip = audioNext - start;
    if (skip >= int64_t(samples.size()))
        return true;
    const size_t first = size_t(std::max<int64_t>(skip, 0));
    const size_t count = samples.size() - first;
    if (!segment.audioInput.readyForMoreMediaData) {
        audioDropped += count;
        audioNext += int64_t(count);
        return true;
    }
    const size_t bytes = count * sizeof(float);
    CMBlockBufferRef block = nullptr;
    CMSampleBufferRef sample = nullptr;
    bool ok = CMBlockBufferCreateWithMemoryBlock(nullptr, nullptr, bytes, nullptr, nullptr, 0, bytes,
                                                 kCMBlockBufferAssureMemoryNowFlag, &block) == noErr &&
              CMBlockBufferReplaceDataBytes(samples.data() + first, block, 0, bytes) == noErr &&
              CMAudioSampleBufferCreateReadyWithPacketDescriptions(nullptr, block, audioFormat, CMItemCount(count),
                                                                   CMTimeMake(audioNext, int32_t(audioRate)), nullptr,
                                                                   &sample) == noErr;
    if (ok)
        ok = [segment.audioInput appendSampleBuffer:sample];
    if (sample)
        CFRelease(sample);
    if (block)
        CFRelease(block);
    audioNext += int64_t(count);
    return ok;
}

void Recorder::Session::finish(std::string reason)
{
    {
        std::lock_guard lock(mutex);
        closing = true;
        for (const Item& item : queue)
            CFRelease(item.buffer);
        queue.clear();
        audio.clear();
        idle.notify_all();
        wake.notify_all();
    }
    VTCompressionSessionCompleteFrames(compressor.get(), kCMTimeInvalid);
    AVAssetWriter* writer = segment.writer;
    bool kept = false;
    if (writer.status == AVAssetWriterStatusWriting && segment.frames > 0) {
        [segment.input markAsFinished];
        [segment.audioInput markAsFinished];
        dispatch_semaphore_t finished = dispatch_semaphore_create(0);
        [writer finishWritingWithCompletionHandler:^{ dispatch_semaphore_signal(finished); }];
        dispatch_semaphore_wait(finished, DISPATCH_TIME_FOREVER);
        if (writer.status != AVAssetWriterStatusCompleted)
            reason = diskFull(writer.error) ? "disk full" : "failed: " + describe(writer.error);
        else if (!(kept = placeMovie(writer, segment.partial, segment.file)))
            reason = "failed: cannot rename the movie into place";
    } else if (writer.status == AVAssetWriterStatusWriting) {
        [writer cancelWriting];
    }
    std::error_code error;
    if (!kept)
        std::filesystem::remove(segment.partial, error);
    discardPrepared();
    dispatch_group_wait(retiring, DISPATCH_TIME_FOREVER);
    compressor.reset();
    owner.ended(*this, {segment.file, frames.load(), dropped.load(), reason, kept});
}

void Recorder::Session::close()
{
    std::lock_guard lock(mutex);
    closing = true;
    wake.notify_all();
}

void Recorder::Session::waitIdle(bool inFlightOnly)
{
    std::unique_lock lock(mutex);
    idle.wait(lock, [&] {
        return inFlight == 0 && (inFlightOnly || closing || (queue.empty() && (audio.empty() || !origin) && !writing));
    });
}

Recorder::Recorder(Engine& engine, double fps, OnEnded onEnded, OnSpace onSpace)
    : engine_(engine), fps_(fps), onEnded_(std::move(onEnded)), onSpace_(std::move(onSpace)), space_(volumeSpace)
{
}

Recorder::~Recorder()
{
    stop();
    std::lock_guard lock(control_);
    if (session_ && session_->thread.joinable())
        session_->thread.join();
}

void Recorder::start(Settings settings, double now, std::optional<double> gigEnd)
{
    std::lock_guard control(control_);
    std::shared_ptr<Session> last;
    {
        std::lock_guard lock(mutex_);
        if (session_ && !session_->done)
            throw std::runtime_error("already recording");
        last = session_;
    }
    if (last && last->thread.joinable())
        last->thread.join();
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
    if (session->audioPlan.state == "unavailable")
        std::printf("record: no audio track: %s\n", session->audioPlan.reason.c_str());
    session->delayMicros = writeDelay_.count();
    session->finishDelayMicros = finishDelay_.count();
    session->startedAt = now;
    if (session->settings.maxTotalSeconds > 0.0) {
        session->stopsAt = now + session->settings.maxTotalSeconds;
        session->stopsFor = "max length";
    }
    if (const double end = gigEnd.value_or(0.0) + session->settings.gigGraceSeconds;
        gigEnd && (!session->stopsAt || end < *session->stopsAt)) {
        session->stopsAt = end;
        session->stopsFor = "gig over";
    }
    session->watcher = std::thread([s = session.get()] { s->watch(); });
    session->thread = std::thread([s = session.get()] { s->run(); });
    {
        std::lock_guard lock(mutex_);
        session_ = session;
    }
    if (session->audioRate > 0.0) {
        session->analysis = audio;
        audio->setTap(session.get(), [session](const float* samples, size_t count, double time, double rate) {
            session->heard(samples, count, time, rate);
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
        if (!session_ || session_->done)
            return;
        session = session_;
    }
    engine_.setFrameTap(nullptr);
    removeAudioTap(*session);
    session->waitIdle(true);
    session->close();
    session->thread.join();
}

void Recorder::ended(Session& session, const Ended& ended)
{
    {
        std::lock_guard lock(mutex_);
        if (session_.get() == &session) {
            engine_.setFrameTap(nullptr);
            removeAudioTap(session);
        }
        session.done = true;
    }
    if (onEnded_)
        onEnded_(ended);
}

void Recorder::spaceChanged()
{
    if (onSpace_)
        onSpace_();
}

Recorder::Status Recorder::status() const
{
    std::lock_guard lock(mutex_);
    Status status;
    if (!session_)
        return status;
    status.recording = !session_->done;
    status.settings = session_->settings;
    {
        std::lock_guard sessionLock(session_->mutex);
        status.file = session_->file;
    }
    status.frames = session_->frames;
    status.dropped = session_->dropped;
    status.hardware = session_->hardware;
    status.pixels = session_->converter ? fourCC(session_->converter->format()) : "BGRA";
    status.audio = session_->audioPlan.state;
    status.audioReason = session_->audioPlan.reason;
    status.audioSource = session_->audioPlan.source;
    status.audioDropped = session_->audioDropped;
    status.space = session_->spaceLevel;
    status.freeBytes = session_->freeBytes;
    status.reserve = session_->reserve;
    status.stopsAt = session_->stopsAt;
    status.stopsFor = session_->stopsFor;
    return status;
}

std::optional<Recorder::Position> Recorder::position(double engineTime) const
{
    std::lock_guard lock(mutex_);
    if (!session_ || session_->done)
        return std::nullopt;
    std::lock_guard sessionLock(session_->mutex);
    const auto& starts = session_->starts;
    if (starts.empty())
        return Position{session_->file, 0.0, false};
    auto in = std::find_if(starts.rbegin(), starts.rend(), [&](const auto& s) { return s.second <= engineTime; });
    const auto& start = in == starts.rend() ? starts.front() : *in;
    const bool settled = session_->written && *session_->written >= engineTime;
    return Position{start.first, std::max(0.0, engineTime - start.second), settled};
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
    session->waitIdle(false);
    // The encoder lets go of what it was handed some time later.
    for (int i = 0; i < 1000 && !session->done; ++i) {
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
    space_ = provider ? std::move(provider) : SpaceProvider(volumeSpace);
    spaceInterval_ = interval;
}

uint64_t Recorder::reserveOf(const Settings& settings, uint64_t total)
{
    return std::max(settings.reserveBytes, uint64_t(double(total) * settings.reservePercent / 100.0));
}

void Recorder::removeAudioTap(Session& session)
{
    if (session.audioRate > 0.0 && session.analysis)
        session.analysis->setTap(&session, nullptr);
}

void Recorder::setWriteDelay(std::chrono::microseconds delay)
{
    std::lock_guard lock(mutex_);
    writeDelay_ = delay;
    if (session_)
        session_->delayMicros = delay.count();
}

void Recorder::setRolloverDelays(std::chrono::microseconds finish, std::chrono::microseconds encoderSetup)
{
    std::lock_guard lock(mutex_);
    finishDelay_ = finish;
    encoderSetup_ = encoderSetup;
}

void Recorder::recover(const std::filesystem::path& folder)
{
    const std::string suffix = kPartialSuffix;
    std::vector<std::filesystem::path> partials;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        const std::string name = entry.path().filename().string();
        if (name.size() > suffix.size() + 1 && name[0] == '.' && name.ends_with(suffix))
            partials.push_back(entry.path());
    }
    for (const std::filesystem::path& partial : partials) {
        const std::string name = partial.filename().string();
        const std::string stem = name.substr(1, name.size() - suffix.size() - 1);
        std::filesystem::path file = folder / (stem + ".mov");
        if (std::filesystem::exists(file))
            file = folder / (stem + " recovered.mov");
        if (!playable(partial) || std::filesystem::exists(file) || std::rename(partial.c_str(), file.c_str()) != 0) {
            std::printf("record: cannot recover %s, left as it is\n", partial.c_str());
            continue;
        }
        std::printf("record: recovered the unfinished recording %s\n", file.c_str());
    }
}

std::string Recorder::folderProblem(const std::filesystem::path& path)
{
    if (path.empty())
        return {};
    const std::filesystem::path folder = path.has_extension() ? path.parent_path() : path;
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error))
        return "the recording folder " + folder.string() + " does not exist (is its disk connected?)";
    if (::access(folder.c_str(), W_OK) != 0)
        return "cannot write to the recording folder " + folder.string();
    return {};
}

std::filesystem::path Recorder::movieFile(const std::filesystem::path& path)
{
    std::filesystem::path base = path;
    if (base.empty()) {
        const char* home = std::getenv("HOME");
        base = std::filesystem::path(home ? home : "/tmp") / "Movies" / "Nocturne";
    }
    if (base.has_extension())
        return base;
    return base / ("Nocturne " + timestamp() + ".mov");
}

} // namespace azl
