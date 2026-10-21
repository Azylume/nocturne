#include "output/h264_stream.hpp"

#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace azl {

namespace {

void appendUnit(std::vector<uint8_t>& avcc, const uint8_t* bytes, size_t size)
{
    for (int shift = 24; shift >= 0; shift -= 8)
        avcc.push_back(uint8_t(size >> shift));
    avcc.insert(avcc.end(), bytes, bytes + size);
}

std::vector<uint8_t> parameterSets(CMFormatDescriptionRef format)
{
    std::vector<uint8_t> config;
    size_t count = 0;
    if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, nullptr, nullptr, &count, nullptr) != noErr)
        return config;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* bytes = nullptr;
        size_t size = 0;
        if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, i, &bytes, &size, nullptr, nullptr) == noErr)
            appendUnit(config, bytes, size);
    }
    return config;
}

bool isKey(CMSampleBufferRef sample)
{
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (!attachments || CFArrayGetCount(attachments) == 0)
        return true;
    auto first = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
    return !CFDictionaryContainsKey(first, kCMSampleAttachmentKey_NotSync);
}

NSDictionary* bt709()
{
    return @{
        (__bridge NSString*)kCVImageBufferColorPrimariesKey : (__bridge NSString*)kCVImageBufferColorPrimaries_ITU_R_709_2,
        (__bridge NSString*)kCVImageBufferTransferFunctionKey : (__bridge NSString*)kCVImageBufferTransferFunction_ITU_R_709_2,
        (__bridge NSString*)kCVImageBufferYCbCrMatrixKey : (__bridge NSString*)kCVImageBufferYCbCrMatrix_ITU_R_709_2,
    };
}

} // namespace

struct H264Encoder::Session {
    VTCompressionSessionRef session = nullptr;
    std::optional<return_video::Packet> out;

    ~Session()
    {
        if (session) {
            VTCompressionSessionInvalidate(session);
            CFRelease(session);
        }
    }

    static void output(void* refcon, void*, OSStatus status, VTEncodeInfoFlags, CMSampleBufferRef sample)
    {
        auto* self = static_cast<Session*>(refcon);
        if (status != noErr || !sample || !CMSampleBufferDataIsReady(sample))
            return;
        CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
        if (!block)
            return;
        return_video::Packet packet;
        packet.data.resize(CMBlockBufferGetDataLength(block));
        if (CMBlockBufferCopyDataBytes(block, 0, packet.data.size(), packet.data.data()) != kCMBlockBufferNoErr)
            return;
        packet.key = isKey(sample);
        if (packet.key)
            packet.config = parameterSets(CMSampleBufferGetFormatDescription(sample));
        self->out = std::move(packet);
    }
};

H264Encoder::H264Encoder(uint32_t width, uint32_t height, double fps, uint32_t kbps)
    : session_(std::make_unique<Session>())
{
    NSDictionary* spec = @{(__bridge NSString*)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder : @YES};
    NSDictionary* source = @{
        (__bridge NSString*)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
        (__bridge NSString*)kCVPixelBufferWidthKey : @(width),
        (__bridge NSString*)kCVPixelBufferHeightKey : @(height),
        (__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey : @{},
        (__bridge NSString*)kCVPixelBufferMetalCompatibilityKey : @YES,
    };
    if (VTCompressionSessionCreate(nullptr, int32_t(width), int32_t(height), kCMVideoCodecType_H264,
                                   (__bridge CFDictionaryRef)spec, (__bridge CFDictionaryRef)source, nullptr,
                                   &Session::output, session_.get(), &session_->session) != noErr)
        throw std::runtime_error("no H.264 encoder");
    VTCompressionSessionRef s = session_->session;
    const int keyFrames = std::max(1, int(std::lround(fps * return_video::kKeySeconds)));
    VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    VTSessionSetProperty(s, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
    VTSessionSetProperty(s, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_Main_AutoLevel);
    VTSessionSetProperty(s, kVTCompressionPropertyKey_ExpectedFrameRate, (__bridge CFNumberRef)@(fps));
    VTSessionSetProperty(s, kVTCompressionPropertyKey_MaxKeyFrameInterval, (__bridge CFNumberRef)@(keyFrames));
    VTSessionSetProperty(s, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration,
                         (__bridge CFNumberRef)@(return_video::kKeySeconds));
    VTSessionSetProperty(s, kVTCompressionPropertyKey_AverageBitRate, (__bridge CFNumberRef)@(kbps * 1000));
    VTSessionSetProperty(s, kVTCompressionPropertyKey_DataRateLimits,
                         (__bridge CFArrayRef)@[@(kbps * 1000 / 8), @1.0]);
    VTSessionSetProperty(s, kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2);
    VTSessionSetProperty(s, kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2);
    VTSessionSetProperty(s, kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2);
    VTCompressionSessionPrepareToEncodeFrames(s);
    // Apple's software encoder, which takes over where the hardware refuses the size (the Intel Mac mini's below 640
    // pixels wide), does not know the property at all.
    CFBooleanRef hardware = nullptr;
    const OSStatus asked =
        VTSessionCopyProperty(s, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder, nullptr, &hardware);
    if (asked == kVTPropertyNotSupportedErr)
        hardware_ = false;
    else if (asked == noErr && hardware) {
        hardware_ = CFBooleanGetValue(hardware);
        CFRelease(hardware);
    }
}

H264Encoder::~H264Encoder() = default;

CVPixelBufferRef H264Encoder::newBuffer()
{
    CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(session_->session);
    CVPixelBufferRef buffer = nullptr;
    if (!pool || CVPixelBufferPoolCreatePixelBuffer(nullptr, pool, &buffer) != kCVReturnSuccess)
        return nullptr;
    CVBufferSetAttachments(buffer, (__bridge CFDictionaryRef)bt709(), kCVAttachmentMode_ShouldPropagate);
    return buffer;
}

std::optional<return_video::Packet> H264Encoder::encode(CVPixelBufferRef pixels, double seconds, bool key)
{
    session_->out.reset();
    NSDictionary* options = key ? @{(__bridge NSString*)kVTEncodeFrameOptionKey_ForceKeyFrame : @YES} : nil;
    if (VTCompressionSessionEncodeFrame(session_->session, pixels, CMTimeMakeWithSeconds(seconds, 90000),
                                        kCMTimeInvalid, (__bridge CFDictionaryRef)options, nullptr, nullptr) != noErr)
        return std::nullopt;
    VTCompressionSessionCompleteFrames(session_->session, kCMTimeInvalid);
    return std::move(session_->out);
}

struct H264Decoder::Session {
    std::vector<uint8_t> config;
    CMVideoFormatDescriptionRef format = nullptr;
    VTDecompressionSessionRef session = nullptr;
    std::optional<Picture> out;

    ~Session() { reset(); }

    void reset()
    {
        if (session) {
            VTDecompressionSessionInvalidate(session);
            CFRelease(session);
            session = nullptr;
        }
        if (format) {
            CFRelease(format);
            format = nullptr;
        }
        config.clear();
    }

    bool open(const std::vector<uint8_t>& sets)
    {
        reset();
        const auto units = return_video::nalUnits(sets);
        if (units.size() < 2)
            return false;
        std::vector<const uint8_t*> pointers;
        std::vector<size_t> sizes;
        for (const auto& unit : units) {
            pointers.push_back(unit.data());
            sizes.push_back(unit.size());
        }
        if (CMVideoFormatDescriptionCreateFromH264ParameterSets(nullptr, units.size(), pointers.data(), sizes.data(), 4,
                                                                &format) != noErr)
            return false;
        NSDictionary* destination = @{
            (__bridge NSString*)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
        };
        VTDecompressionOutputCallbackRecord callback{&Session::output, this};
        if (VTDecompressionSessionCreate(nullptr, format, nullptr, (__bridge CFDictionaryRef)destination, &callback,
                                         &session) != noErr) {
            reset();
            return false;
        }
        config = sets;
        return true;
    }

    static void output(void* refcon, void*, OSStatus status, VTDecodeInfoFlags, CVImageBufferRef image, CMTime,
                       CMTime)
    {
        auto* self = static_cast<Session*>(refcon);
        if (status != noErr || !image)
            return;
        CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
        Picture picture{uint32_t(CVPixelBufferGetWidth(image)), uint32_t(CVPixelBufferGetHeight(image)), {}};
        const size_t row = size_t(picture.width) * 4;
        const size_t stride = CVPixelBufferGetBytesPerRow(image);
        const auto* base = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(image));
        picture.bgra.resize(row * picture.height);
        for (uint32_t y = 0; y < picture.height; ++y)
            std::memcpy(picture.bgra.data() + y * row, base + y * stride, row);
        CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
        self->out = std::move(picture);
    }
};

H264Decoder::H264Decoder() : session_(std::make_unique<Session>()) {}

H264Decoder::~H264Decoder() = default;

std::optional<H264Decoder::Picture> H264Decoder::decode(const return_video::Packet& packet)
{
    Session& s = *session_;
    if (!packet.config.empty() && packet.config != s.config && !s.open(packet.config))
        return std::nullopt;
    if (!s.session || packet.data.empty())
        return std::nullopt;
    CMBlockBufferRef block = nullptr;
    if (CMBlockBufferCreateWithMemoryBlock(nullptr, nullptr, packet.data.size(), nullptr, nullptr, 0,
                                           packet.data.size(), kCMBlockBufferAssureMemoryNowFlag,
                                           &block) != kCMBlockBufferNoErr)
        return std::nullopt;
    CMBlockBufferReplaceDataBytes(packet.data.data(), block, 0, packet.data.size());
    CMSampleBufferRef sample = nullptr;
    const size_t size = packet.data.size();
    const OSStatus made = CMSampleBufferCreateReady(nullptr, block, s.format, 1, 0, nullptr, 1, &size, &sample);
    CFRelease(block);
    if (made != noErr)
        return std::nullopt;
    s.out.reset();
    VTDecompressionSessionDecodeFrame(s.session, sample, 0, nullptr, nullptr);
    VTDecompressionSessionWaitForAsynchronousFrames(s.session);
    CFRelease(sample);
    return std::move(s.out);
}

} // namespace azl
