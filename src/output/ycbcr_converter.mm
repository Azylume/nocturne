#include "output/ycbcr_converter.hpp"

#include "engine/metal_util.hpp"

#include <IOSurface/IOSurface.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <cmath>
#include <functional>

#include <stdexcept>

namespace azl {

namespace {

constexpr const char* kSource = R"(
#include <metal_stdlib>
using namespace metal;

struct Params {
    // How each chroma sample mixes its block's pixels: top-left, top-right, bottom-left, bottom-right.
    float4 weights;
    // Steps of the format per 8-bit code, and the texel value of one step.
    float steps;
    float step;
};

// BT.709 video range, in 8-bit codes, rounded to the format's precision.
static float quantise(float code, constant Params& p) { return rint(code * p.steps) * p.step; }
static float luma(float3 c, constant Params& p)
{
    return quantise(16.0 + 219.0 * dot(c, float3(0.2126, 0.7152, 0.0722)), p);
}
static float2 chroma(float3 c, constant Params& p)
{
    const float y = dot(c, float3(0.2126, 0.7152, 0.0722));
    const float2 code = 128.0 + 224.0 * float2((c.b - y) / 1.8556, (c.r - y) / 1.5748);
    return float2(quantise(code.x, p), quantise(code.y, p));
}

// Each thread converts a 4x2 block and writes whole RGBA texels: four luma samples per row, two CbCr pairs.
kernel void to420(texture2d<float, access::read> source [[texture(0)]],
                  texture2d<float, access::write> lumaPlane [[texture(1)]],
                  texture2d<float, access::write> chromaPlane [[texture(2)]],
                  constant Params& p [[buffer(0)]],
                  uint2 at [[thread_position_in_grid]])
{
    if (at.x >= chromaPlane.get_width() || at.y >= chromaPlane.get_height())
        return;
    const uint2 o = uint2(at.x * 4, at.y * 2);
    float3 top[4], bottom[4];
    float4 upper, lower;
    for (uint i = 0; i < 4; ++i) {
        top[i] = source.read(o + uint2(i, 0)).rgb;
        bottom[i] = source.read(o + uint2(i, 1)).rgb;
        upper[i] = luma(top[i], p);
        lower[i] = luma(bottom[i], p);
    }
    lumaPlane.write(upper, uint2(at.x, o.y));
    lumaPlane.write(lower, uint2(at.x, o.y + 1));
    const float4 w = p.weights;
    const float3 left = w.x * top[0] + w.y * top[1] + w.z * bottom[0] + w.w * bottom[1];
    const float3 right = w.x * top[2] + w.y * top[3] + w.z * bottom[2] + w.w * bottom[3];
    chromaPlane.write(float4(chroma(left, p), chroma(right, p)), at);
}

// '2vuy' and 'v216' are Cb Y0 Cr Y1 per pixel pair, written as one RGBA texel.
kernel void to422(texture2d<float, access::read> source [[texture(0)]],
                  texture2d<float, access::write> pairs [[texture(1)]],
                  constant Params& p [[buffer(0)]],
                  uint2 at [[thread_position_in_grid]])
{
    if (at.x >= pairs.get_width() || at.y >= pairs.get_height())
        return;
    const float3 a = source.read(uint2(at.x * 2, at.y)).rgb;
    const float3 b = source.read(uint2(at.x * 2 + 1, at.y)).rgb;
    const float2 cbcr = chroma(p.weights.x * a + p.weights.y * b, p);
    pairs.write(float4(cbcr.x, luma(a, p), cbcr.y, luma(b, p)), at);
}
)";

struct Plane {
    MTL::PixelFormat format;
    // Picture pixels per texel across and down.
    uint32_t across;
    uint32_t down;
};

struct Layout {
    std::vector<Plane> planes;
    // The largest code, and the texel value of one code.
    float top;
    float step;
};

Layout layoutOf(OSType format)
{
    switch (format) {
    case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
        return {{{MTL::PixelFormatRGBA8Unorm, 4, 1}, {MTL::PixelFormatRGBA8Unorm, 4, 2}}, 255.0f, 1.0f / 255.0f};
    // 10 bits in the high bits of 16.
    case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange:
        return {{{MTL::PixelFormatRGBA16Unorm, 4, 1}, {MTL::PixelFormatRGBA16Unorm, 4, 2}}, 1023.0f, 64.0f / 65535.0f};
    case kCVPixelFormatType_422YpCbCr8:
        return {{{MTL::PixelFormatRGBA8Unorm, 2, 1}}, 255.0f, 1.0f / 255.0f};
    case kCVPixelFormatType_422YpCbCr16:
        return {{{MTL::PixelFormatRGBA16Unorm, 2, 1}}, 65535.0f, 1.0f / 65535.0f};
    default:
        return {};
    }
}

std::vector<Plane> planesOf(OSType format)
{
    return layoutOf(format).planes;
}

struct Calibration {
    std::array<float, 4> weights;
    float steps;
};

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
    const Layout layout = layoutOf(CVPixelBufferGetPixelFormatType(buffer));
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
Calibration calibrate(OSType format)
{
    const Layout layout = layoutOf(format);
    const int pixels = layout.planes.size() == 2 ? 4 : 2;
    const float perCode = layout.top == 255.0f ? 1.0f : layout.top == 1023.0f ? 4.0f : 256.0f;
    Calibration calibration{pixels == 4 ? std::array<float, 4>{0.25f, 0.25f, 0.25f, 0.25f}
                                        : std::array<float, 4>{0.5f, 0.5f, 0.0f, 0.0f},
                            perCode};
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
    auto library = makeLibrary(device, kSource);
    auto function = NS::TransferPtr(library->newFunction(
        NS::String::string(planesOf(format).size() == 2 ? "to420" : "to422", NS::UTF8StringEncoding)));
    NS::Error* error = nullptr;
    pipeline_ = NS::TransferPtr(device->newComputePipelineState(function.get(), &error));
    if (!pipeline_)
        throw std::runtime_error("cannot build the Y'CbCr conversion");
    const Calibration calibration = calibrate(format);
    params_ = {calibration.weights, calibration.steps, layoutOf(format).step};
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
    const std::vector<Plane> planes = planesOf(format_);
    std::vector<NS::SharedPtr<MTL::Texture>> textures;
    for (size_t i = 0; i < planes.size(); ++i) {
        const Plane& plane = planes[i];
        auto* desc = MTL::TextureDescriptor::texture2DDescriptor(plane.format, width / plane.across,
                                                                 height / plane.down, false);
        desc->setUsage(MTL::TextureUsageShaderWrite);
        // Shared needs no synchronisation pass; machines that refuse it for a surface take managed.
        desc->setStorageMode(MTL::StorageModeShared);
        auto texture = NS::TransferPtr(device_->newTexture(desc, surface, i));
        if (!texture) {
            desc->setStorageMode(MTL::StorageModeManaged);
            texture = NS::TransferPtr(device_->newTexture(desc, surface, i));
        }
        if (!texture)
            return false;
        textures.push_back(texture);
    }
    planes_[surface] = std::move(textures);
    return true;
}

bool YCbCrConverter::encode(MTL::CommandBuffer* cmd, MTL::Texture* source, CVPixelBufferRef target)
{
    if (!prepare(target))
        return false;
    const auto& textures = planes_[CVPixelBufferGetIOSurface(target)];
    MTL::Texture* grid = textures.back().get();
    auto* pass = MTL::ComputePassDescriptor::computePassDescriptor();
    AZL_GPU_ZONE(gpu_, pass, "record convert");
    MTL::ComputeCommandEncoder* enc = cmd->computeCommandEncoder(pass);
    enc->setComputePipelineState(pipeline_.get());
    enc->setTexture(source, 0);
    for (size_t i = 0; i < textures.size(); ++i)
        enc->setTexture(textures[i].get(), i + 1);
    enc->setBytes(&params_, sizeof params_, 0);
    enc->dispatchThreadgroups(MTL::Size((grid->width() + 15) / 16, (grid->height() + 15) / 16, 1),
                              MTL::Size(16, 16, 1));
    enc->endEncoding();
    if (textures[0]->storageMode() == MTL::StorageModeManaged) {
        auto* syncPass = MTL::BlitPassDescriptor::blitPassDescriptor();
        AZL_GPU_ZONE(gpu_, syncPass, "record sync");
        MTL::BlitCommandEncoder* sync = cmd->blitCommandEncoder(syncPass);
        for (const auto& texture : textures)
            sync->synchronizeResource(texture.get());
        sync->endEncoding();
    }
    return true;
}

} // namespace azl
