#include "output/ycbcr_pass.hpp"

#include "effects/shader_layout.hpp"
#include "gpu/glsl.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

namespace azl {

namespace {

constexpr const char* kSource = R"(#version 450
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_samplerless_texture_functions : require

layout(local_size_x = 16, local_size_y = 16) in;

layout(set = 0, binding = 0, scalar) uniform Params {
    // How each chroma sample mixes its block's pixels: top-left, top-right, bottom-left, bottom-right.
    vec4 weights;
    // Steps of the format per 8-bit code, and the texel value of one step.
    float steps;
    float step;
} p;

layout(set = 1, binding = 0) uniform texture2D source;

// BT.709 video range, in 8-bit codes, rounded to the format's precision.
float quantise(float code) { return roundEven(code * p.steps) * p.step; }
float luma(vec3 c)
{
    return quantise(16.0 + 219.0 * dot(c, vec3(0.2126, 0.7152, 0.0722)));
}
vec2 chroma(vec3 c)
{
    float y = dot(c, vec3(0.2126, 0.7152, 0.0722));
    vec2 code = 128.0 + 224.0 * vec2((c.b - y) / 1.8556, (c.r - y) / 1.5748);
    return vec2(quantise(code.x), quantise(code.y));
}

vec3 at(uvec2 q) { return texelFetch(source, ivec2(q), 0).rgb; }

#ifdef ENTRY_to420
layout(set = 1, binding = 1) writeonly uniform image2D lumaPlane;
layout(set = 1, binding = 2) writeonly uniform image2D chromaPlane;

// Each thread converts a 4x2 block and writes whole RGBA texels: four luma samples per row, two CbCr pairs.
void main()
{
    uvec2 id = gl_GlobalInvocationID.xy;
    if (id.x >= uint(imageSize(chromaPlane).x) || id.y >= uint(imageSize(chromaPlane).y))
        return;
    uvec2 o = uvec2(id.x * 4u, id.y * 2u);
    vec3 top[4], bottom[4];
    vec4 upper, lower;
    for (uint i = 0u; i < 4u; ++i) {
        top[i] = at(o + uvec2(i, 0u));
        bottom[i] = at(o + uvec2(i, 1u));
        upper[i] = luma(top[i]);
        lower[i] = luma(bottom[i]);
    }
    imageStore(lumaPlane, ivec2(id.x, o.y), upper);
    imageStore(lumaPlane, ivec2(id.x, o.y + 1u), lower);
    vec4 w = p.weights;
    vec3 left = w.x * top[0] + w.y * top[1] + w.z * bottom[0] + w.w * bottom[1];
    vec3 right = w.x * top[2] + w.y * top[3] + w.z * bottom[2] + w.w * bottom[3];
    imageStore(chromaPlane, ivec2(id), vec4(chroma(left), chroma(right)));
}
#endif

#ifdef ENTRY_to422
layout(set = 1, binding = 1) writeonly uniform image2D pairs;

// '2vuy' and 'v216' are Cb Y0 Cr Y1 per pixel pair, written as one RGBA texel.
void main()
{
    uvec2 id = gl_GlobalInvocationID.xy;
    if (id.x >= uint(imageSize(pairs).x) || id.y >= uint(imageSize(pairs).y))
        return;
    vec3 a = at(uvec2(id.x * 2u, id.y));
    vec3 b = at(uvec2(id.x * 2u + 1u, id.y));
    vec2 cbcr = chroma(p.weights.x * a + p.weights.y * b);
    imageStore(pairs, ivec2(id), vec4(cbcr.x, luma(a), cbcr.y, luma(b)));
}
#endif

#ifdef ENTRY_to422p
layout(set = 1, binding = 1) writeonly uniform image2D lumaPlane;
layout(set = 1, binding = 2) writeonly uniform image2D cbPlane;
layout(set = 1, binding = 3) writeonly uniform image2D crPlane;

// Each thread converts a pixel pair: two luma samples, one Cb and one Cr.
void main()
{
    uvec2 id = gl_GlobalInvocationID.xy;
    if (id.x >= uint(imageSize(crPlane).x) || id.y >= uint(imageSize(crPlane).y))
        return;
    vec3 a = at(uvec2(id.x * 2u, id.y));
    vec3 b = at(uvec2(id.x * 2u + 1u, id.y));
    vec2 cbcr = chroma(p.weights.x * a + p.weights.y * b);
    imageStore(lumaPlane, ivec2(id.x * 2u, id.y), vec4(luma(a)));
    imageStore(lumaPlane, ivec2(id.x * 2u + 1u, id.y), vec4(luma(b)));
    imageStore(cbPlane, ivec2(id), vec4(cbcr.x));
    imageStore(crPlane, ivec2(id), vec4(cbcr.y));
}
#endif
)";

const char* entryOf(YCbCrPacking packing)
{
    switch (packing) {
    case YCbCrPacking::BiPlanar420:
        return "to420";
    case YCbCrPacking::Pairs422:
        return "to422";
    case YCbCrPacking::Planar422:
        return "to422p";
    }
    return "";
}

} // namespace

YCbCrCalibration averageCalibration(const YCbCrFormat& format)
{
    const std::array<float, 4> weights = format.packing == YCbCrPacking::BiPlanar420
                                             ? std::array<float, 4>{0.25f, 0.25f, 0.25f, 0.25f}
                                             : std::array<float, 4>{0.5f, 0.5f, 0.0f, 0.0f};
    return {weights, format.top == 255.0f ? 1.0f : format.top == 1023.0f ? 4.0f : 256.0f};
}

YCbCrFormat ycbcrFormat(YCbCrLayout layout)
{
    using enum gpu::Format;
    switch (layout) {
    case YCbCrLayout::NV12:
        return {YCbCrPacking::BiPlanar420, {{RGBA8, 4, 1}, {RGBA8, 4, 2}}, 255.0f, 1.0f / 255.0f};
    // 10 bits in the high bits of 16.
    case YCbCrLayout::P010:
        return {YCbCrPacking::BiPlanar420, {{RGBA16, 4, 1}, {RGBA16, 4, 2}}, 1023.0f, 64.0f / 65535.0f};
    // 10 bits in the low bits of 16.
    case YCbCrLayout::Planar422P10:
        return {YCbCrPacking::Planar422, {{R16, 1, 1}, {R16, 2, 1}, {R16, 2, 1}}, 1023.0f, 1.0f / 65535.0f};
    }
    return {};
}

void copyPlanes(const YCbCrFormat& format, std::span<const gpu::Readback> planes, YCbCrPicture& picture)
{
    for (size_t i = 0; i < format.planes.size(); ++i) {
        const YCbCrPlane& plane = format.planes[i];
        const gpu::Readback& readback = planes[i];
        const size_t pictureRow =
            (size_t(picture.width) * gpu::unitBytes(plane.format) + plane.across - 1) / plane.across;
        const size_t row = std::min({pictureRow, readback.rowBytes(), picture.strides[i]});
        const uint32_t rows = (picture.height + plane.down - 1) / plane.down;
        for (uint32_t y = 0; y < rows; ++y)
            std::memcpy(picture.planes[i] + y * picture.strides[i], readback.data() + y * readback.rowBytes(), row);
    }
}

YCbCrPass::YCbCrPass(const gpu::Device& device, YCbCrFormat format, YCbCrCalibration calibration)
    : format_(std::move(format)), params_{calibration.weights, calibration.steps, format_.step}
{
    const std::string entry = entryOf(format_.packing);
    std::string error;
    const gpu::ShaderModule module =
        device.makeShaderModule(gpu::compileGlsl({{entry, ShaderStage::Compute, withEntry(kSource, entry)}}), &error);
    if (module)
        pipeline_ = device.makeComputePipeline(module, gpu::entryPoint(entry), &error);
    if (!pipeline_)
        throw std::runtime_error("cannot build the Y'CbCr conversion: " + error);
}

std::vector<gpu::Texture> YCbCrPass::makePlanes(const gpu::Device& device, uint32_t width, uint32_t height) const
{
    std::vector<gpu::Texture> planes;
    for (const YCbCrPlane& plane : format_.planes) {
        gpu::Texture texture = device.makeTexture({.format = plane.format,
                                                   .width = (width + plane.across - 1) / plane.across,
                                                   .height = (height + plane.down - 1) / plane.down,
                                                   .usage = gpu::Usage::Storage});
        if (!texture)
            return {};
        planes.push_back(std::move(texture));
    }
    return planes;
}

void YCbCrPass::encode(gpu::CommandBuffer& cmd, const gpu::Texture& source, std::span<const gpu::Texture> planes) const
{
    const gpu::Texture& grid = planes.back();
    gpu::ComputePass pass = cmd.beginCompute();
    pass.setPipeline(pipeline_);
    pass.setTexture(0, source);
    for (size_t i = 0; i < planes.size(); ++i)
        pass.setTexture(uint32_t(i + 1), planes[i]);
    pass.setBytes(0, &params_, sizeof params_);
    pass.dispatchGroups((grid.width() + 15) / 16, (grid.height() + 15) / 16, 16, 16);
    pass.end();
}

} // namespace azl
