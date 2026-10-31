#include "output/ycbcr_converter.hpp"


#include <IOSurface/IOSurface.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <cmath>
#include <functional>

#include <stdexcept>

namespace azl {

namespace {

YCbCrFormat layoutOf(OSType format)
{
    using enum gpu::Format;
    switch (format) {
    case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
        return {YCbCrPacking::BiPlanar420, {{RGBA8, 4, 1}, {RGBA8, 4, 2}}, 255.0f, 1.0f / 255.0f};
    // 10 bits in the high bits of 16.
    case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange:
        return {YCbCrPacking::BiPlanar420, {{RGBA16, 4, 1}, {RGBA16, 4, 2}}, 1023.0f, 64.0f / 65535.0f};
    case kCVPixelFormatType_422YpCbCr8:
        return {YCbCrPacking::Pairs422, {{RGBA8, 2, 1}}, 255.0f, 1.0f / 255.0f};
    case kCVPixelFormatType_422YpCbCr16:
        return {YCbCrPacking::Pairs422, {{RGBA16, 2, 1}}, 65535.0f, 1.0f / 65535.0f};
    default:
        return {};
    }
}

std::vector<YCbCrPlane> planesOf(OSType format)
{
    return layoutOf(format).planes;
}

constexpr int kProbe = 16;

// VideoToolbox's conversion into `format` of a small BGRA picture that `paint` fills; null when it fails.
CVPixelBufferRef convertedByVideoToolbox(VTPixelTransferSessionRef transfer, OSType format,
                                         const std::function<void(int x, int y, uint8_t* bgra)>& paint)
{
    NSDictionary* attributes = @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{}};
    CVPixelBufferRef picture = nullptr;
    CVPixelBufferRef out = nullptr;
    CVPixelBufferCreate(nullptr, kProbe, kProbe, kCVPixelFormatType_32BGRA, (__bridge CFDictionaryRef)attributes,
                        &picture);
    CVPixelBufferCreate(nullptr, kProbe, kProbe, format, (__bridge CFDictionaryRef)attributes, &out);
    bool ok = picture && out;
    if (ok) {
        CVBufferSetAttachment(out, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2,
                              kCVAttachmentMode_ShouldPropagate);
        CVPixelBufferLockBaseAddress(picture, 0);
        auto* base = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(picture));
        for (int y = 0; y < kProbe; ++y)
            for (int x = 0; x < kProbe; ++x)
                paint(x, y, base + y * CVPixelBufferGetBytesPerRow(picture) + x * 4);
        CVPixelBufferUnlockBaseAddress(picture, 0);
        ok = VTPixelTransferSessionTransferImage(transfer, picture, out) == noErr;
    }
    if (picture)
        CFRelease(picture);
    if (out && !ok) {
        CFRelease(out);
        out = nullptr;
    }
    return out;
}

// Sample `index` of row `row` of a plane, in the format's codes.
float sampleOf(CVPixelBufferRef buffer, size_t plane, size_t row, size_t index)
{
    const YCbCrFormat layout = layoutOf(CVPixelBufferGetPixelFormatType(buffer));
    const bool planar = CVPixelBufferIsPlanar(buffer);
    CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    const auto* base = static_cast<const uint8_t*>(planar ? CVPixelBufferGetBaseAddressOfPlane(buffer, plane)
                                                          : CVPixelBufferGetBaseAddress(buffer)) +
                       row * (planar ? CVPixelBufferGetBytesPerRowOfPlane(buffer, plane)
                                     : CVPixelBufferGetBytesPerRow(buffer));
    const float value = layout.top > 255.0f ? float(base[index * 2] | base[index * 2 + 1] << 8) : float(base[index]);
    CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    // 10-bit codes sit in the high bits.
    return layout.top == 1023.0f ? value / 64.0f : value;
}

// VideoToolbox's conversion differs between Macs, so the converter copies how this one does it:
//   - how it scales 8-bit codes into more bits (the M2 Pro by 1023/255 into 10 bits, the Mac mini by 4), from the
//     code it gives white;
//   - how it subsamples chroma (the M2 Pro takes a 4:2:0 block's top-left pixel, the Mac mini averages the block),
//     from its conversion of a blue dot on each pixel of a block.
YCbCrCalibration calibrate(OSType format)
{
    const YCbCrFormat layout = layoutOf(format);
    const int pixels = layout.planes.size() == 2 ? 4 : 2;
    YCbCrCalibration calibration = averageCalibration(layout);
    const float perCode = calibration.steps;
    VTPixelTransferSessionRef transfer = nullptr;
    if (VTPixelTransferSessionCreate(nullptr, &transfer) != noErr)
        return calibration;

    if (CVPixelBufferRef white = convertedByVideoToolbox(transfer, format, [](int, int, uint8_t* p) {
            p[0] = p[1] = p[2] = p[3] = 255;
        })) {
        const float code = sampleOf(white, 0, 0, pixels == 4 ? 0 : 1);
        const float scaled = layout.top / 255.0f;
        if (std::abs(std::rint(235.0f * scaled) - code) < std::abs(std::rint(235.0f * perCode) - code))
            calibration.steps = scaled;
        CFRelease(white);
    }

    constexpr int kAt = 8;
    std::array<float, 4> weights{};
    float sum = 0.0f;
    for (int i = 0; i < pixels; ++i) {
        CVPixelBufferRef dot = convertedByVideoToolbox(transfer, format, [i](int x, int y, uint8_t* p) {
            p[0] = x == kAt + i % 2 && y == kAt + i / 2 ? 255 : 0;
            p[1] = p[2] = 0;
            p[3] = 255;
        });
        if (!dot)
            continue;
        // The Cb sample of the block holding the dots, against one of black.
        const float cb = pixels == 4 ? sampleOf(dot, 1, kAt / 2, kAt) : sampleOf(dot, 0, kAt, kAt * 2);
        const float black = sampleOf(dot, pixels == 4 ? 1 : 0, 0, 0);
        weights[size_t(i)] = std::max(0.0f, cb - black);
        sum += weights[size_t(i)];
        CFRelease(dot);
    }
    VTPixelTransferSessionInvalidate(transfer);
    CFRelease(transfer);
    if (sum > 0.0f)
        for (size_t i = 0; i < 4; ++i)
            calibration.weights[i] = weights[i] / sum;
    return calibration;
}

} // namespace

bool YCbCrConverter::writes(OSType format)
{
    return !planesOf(format).empty();
}

YCbCrConverter::YCbCrConverter(MTL::Device* device, OSType format) : device_(device), format_(format)
{
    if (!writes(format))
        throw std::runtime_error("no Y'CbCr conversion into this pixel format");
    pass_ = std::make_unique<YCbCrPass>(gpu::Device::retain(device), layoutOf(format), calibrate(format));
}

YCbCrConverter::~YCbCrConverter() = default;

bool YCbCrConverter::prepare(CVPixelBufferRef target)
{
    IOSurfaceRef surface = CVPixelBufferGetIOSurface(target);
    if (!surface || CVPixelBufferGetPixelFormatType(target) != format_ || CVPixelBufferGetWidth(target) % 4 ||
        CVPixelBufferGetHeight(target) % 2)
        return false;
    if (planes_.contains(surface))
        return true;
    const auto width = uint32_t(CVPixelBufferGetWidth(target));
    const auto height = uint32_t(CVPixelBufferGetHeight(target));
    const std::vector<YCbCrPlane> planes = planesOf(format_);
    std::vector<gpu::Texture> textures;
    for (size_t i = 0; i < planes.size(); ++i) {
        const YCbCrPlane& plane = planes[i];
        auto* desc = MTL::TextureDescriptor::texture2DDescriptor(gpu::metalFormat(plane.format), width / plane.across,
                                                                 height / plane.down, false);
        desc->setUsage(MTL::TextureUsageShaderWrite);
        // Shared needs no synchronisation pass; machines that refuse it for a surface take managed.
        desc->setStorageMode(MTL::StorageModeShared);
        MTL::Texture* texture = device_->newTexture(desc, surface, i);
        if (!texture) {
            desc->setStorageMode(MTL::StorageModeManaged);
            texture = device_->newTexture(desc, surface, i);
        }
        if (!texture)
            return false;
        textures.push_back(gpu::Texture::adopt(texture));
    }
    planes_[surface] = std::move(textures);
    return true;
}

bool YCbCrConverter::encode(MTL::CommandBuffer* cmd, MTL::Texture* source, CVPixelBufferRef target)
{
    if (!prepare(target))
        return false;
    const std::vector<gpu::Texture>& textures = planes_[CVPixelBufferGetIOSurface(target)];
    gpu::CommandBuffer converting = gpu::CommandBuffer::wrap(cmd);
    pass_->encode(converting, gpu::Texture::retain(source), textures);
    for (const gpu::Texture& texture : textures)
        converting.synchronize(texture);
    return true;
}

} // namespace azl
