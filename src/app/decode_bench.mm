#include "effects/shader_layout.hpp"
#include "engine/shader_cross.hpp"
#include "media/video_decoder.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

// Decodes a clip through the engine's decoder and draws each picture into a 1080p target the way a layer's source
// stage samples it, to compare codecs: process CPU time and wall time per decoded frame, and GPU time per sampled
// frame. Nothing is shown.
namespace {

constexpr const char* kUsage = R"(usage: nocturne-decode-bench [--frames N] [--repeat R] FILE...

Per file: CPU ms per decoded frame (process user + system time), wall ms per frame, GPU ms per frame drawn into a
1920x1080 RGBA8 target, and the file's read rate at its frame rate. Run with nothing else busy.
)";

constexpr const char* kShader = R"(#version 450
#extension GL_EXT_scalar_block_layout : require

#ifdef ENTRY_fullscreen
layout(location = 0) out vec2 uv;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * vec2(2, -2) + vec2(-1, 1), 0, 1);
    uv = p;
}
#endif

#ifdef ENTRY_draw
layout(set = 0, binding = 0, scalar) uniform Header { vec4 yuv[3]; uint turns; } h;
layout(set = 1, binding = 0) uniform texture2D t0;
layout(set = 1, binding = 1) uniform texture2D t1;
layout(set = 2, binding = 0) uniform sampler lin;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour;
vec4 hapAt(vec2 uv, uint format) {
    vec4 c = texture(sampler2D(t0, lin), uv);
    if (format == 1u) return vec4(c.rgb, 1.0);
    if (format == 2u) return c;
    float scale = c.b * (255.0 / 8.0) + 1.0;
    float co = (c.r - 128.0 / 255.0) / scale;
    float cg = (c.g - 128.0 / 255.0) / scale;
    vec3 rgb = clamp(vec3(c.a + co - cg, c.a + cg, c.a - co - cg), 0.0, 1.0);
    return vec4(rgb, format == 4u ? texture(sampler2D(t1, lin), uv).r : 1.0);
}
void main() {
    if (h.turns > 3u) {
        colour = hapAt(uv, h.turns >> 2);
        return;
    }
    vec3 yuv = vec3(texture(sampler2D(t0, lin), uv).r, texture(sampler2D(t1, lin), uv).rg);
    vec3 rgb = vec3(dot(h.yuv[0].xyz, yuv), dot(h.yuv[1].xyz, yuv), dot(h.yuv[2].xyz, yuv));
    colour = vec4(clamp(rgb + vec3(h.yuv[0].w, h.yuv[1].w, h.yuv[2].w), 0.0, 1.0), 1.0);
}
#endif
)";

struct Header {
    float yuv[12];
    uint32_t turns;
    uint32_t pad[3];
};

double cpuSeconds()
{
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    auto seconds = [](timeval t) { return double(t.tv_sec) + t.tv_usec / 1e6; };
    return seconds(usage.ru_utime) + seconds(usage.ru_stime);
}

double median(std::vector<double> values)
{
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

void bench(id<MTLDevice> device, id<MTLRenderPipelineState> pipeline, const std::string& file, int frames)
{
    azl::VideoDecoder::Options options;
    options.name = std::filesystem::path(file).filename().string();
    options.cacheBytes = size_t(256) << 20;
    const azl::gpu::Device gpuDevice = azl::gpu::Device::retain((__bridge MTL::Device*)device);
    azl::VideoDecoder decoder(&gpuDevice, file, options);
    decoder.hurry(true);
    const azl::ClipLength length = decoder.length();
    frames = std::min(frames, length.frames);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    MTLTextureDescriptor* targetDescriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1920 height:1080
                                                       mipmapped:NO];
    targetDescriptor.usage = MTLTextureUsageRenderTarget;
    targetDescriptor.storageMode = MTLStorageModePrivate;
    id<MTLTexture> target = [device newTextureWithDescriptor:targetDescriptor];

    // The first pictures are kept for the GPU timing: holding every decoded buffer would starve the decoder's pool.
    constexpr size_t kTimed = 30;
    std::vector<azl::VideoFrameRef> pictures;
    int decoded = 0;
    const double cpuBefore = cpuSeconds();
    const auto wallBefore = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i)
        if (azl::VideoFrameRef frame = decoder.waitFrame(i, true)) {
            ++decoded;
            if (pictures.size() < kTimed)
                pictures.push_back(std::move(frame));
        }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallBefore).count();
    const double cpu = cpuSeconds() - cpuBefore;

    std::vector<double> gpu;
    for (const azl::VideoFrameRef& picture : pictures) {
        @autoreleasepool {
            id<MTLCommandBuffer> commands = [queue commandBuffer];
            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = target;
            pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
            Header header{};
            std::copy(picture->yuvToRgb.begin(), picture->yuvToRgb.end(), header.yuv);
            header.turns = picture->headerTurns();
            [encoder setRenderPipelineState:pipeline];
            [encoder setFragmentTexture:(__bridge id<MTLTexture>)picture->luma.native() atIndex:0];
            [encoder setFragmentTexture:(__bridge id<MTLTexture>)picture->chroma.native() atIndex:1];
            [encoder setFragmentBytes:&header length:sizeof(header) atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [encoder endEncoding];
            [commands commit];
            [commands waitUntilCompleted];
            gpu.push_back((commands.GPUEndTime - commands.GPUStartTime) * 1e3);
        }
    }
    const double bytes = double(std::filesystem::file_size(file));
    const double seconds = length.frames / std::max(1.0, length.fps);
    std::printf("%-40s %5d frames  cpu %6.2f ms/frame  wall %6.2f ms/frame  gpu %6.3f ms/frame (median)  %6.1f MB/s  "
                "%s\n",
                options.name.c_str(), decoded, 1e3 * cpu / std::max(1, decoded), 1e3 * wall / std::max(1, decoded),
                median(gpu), bytes / seconds / 1e6,
                pictures.empty() || pictures.front()->format == azl::PictureFormat::YCbCr ? "Y'CbCr" : "HAP");
}

} // namespace

int main(int argc, char** argv)
{
    @autoreleasepool {
        int frames = 200, repeat = 1;
        std::vector<std::string> files;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--frames" && i + 1 < argc)
                frames = std::atoi(argv[++i]);
            else if (arg == "--repeat" && i + 1 < argc)
                repeat = std::atoi(argv[++i]);
            else if (arg.rfind("--", 0) == 0)
                return std::fputs(kUsage, stderr), 2;
            else
                files.push_back(arg);
        }
        if (files.empty())
            return std::fputs(kUsage, stderr), 2;
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        const azl::CrossResult msl =
            azl::glslToMsl({{"fullscreen", azl::ShaderStage::Vertex, azl::withEntry(kShader, "fullscreen")},
                            {"draw", azl::ShaderStage::Fragment, azl::withEntry(kShader, "draw")}});
        if (msl.text.empty())
            return std::fprintf(stderr, "%s\n", msl.error.c_str()), 1;
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:@(msl.text.c_str()) options:nil error:&error];
        if (!library)
            return std::fprintf(stderr, "%s\n", error.localizedDescription.UTF8String), 1;
        MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = [library newFunctionWithName:@(azl::mslFunction("fullscreen").c_str())];
        descriptor.fragmentFunction = [library newFunctionWithName:@(azl::mslFunction("draw").c_str())];
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!pipeline)
            return std::fprintf(stderr, "%s\n", error.localizedDescription.UTF8String), 1;
        std::printf("%s\n", device.name.UTF8String);
        for (int r = 0; r < repeat; ++r)
            for (const std::string& file : files) {
                try {
                    bench(device, pipeline, file, frames);
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "%s: %s\n", file.c_str(), e.what());
                }
            }
    }
    return 0;
}
