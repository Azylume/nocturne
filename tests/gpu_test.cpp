#include "gpu/glsl.hpp"
#include "gpu/gpu.hpp"

#include "test_files.hpp"

#include "doctest.h"

#include "effects/shader_layout.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace gpu = azl::gpu;

namespace {

std::vector<uint8_t> readBack(const gpu::Device& device, const gpu::Queue& queue, const gpu::Texture& texture)
{
    gpu::Scope scope;
    gpu::Readback readback(device, texture);
    gpu::CommandBuffer cmd = queue.commandBuffer();
    readback.encode(cmd);
    cmd.commit();
    cmd.waitUntilCompleted();
    REQUIRE(cmd.completion().ok);
    return {readback.data(), readback.data() + readback.size()};
}

std::vector<uint8_t> pattern(size_t size)
{
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i)
        bytes[i] = uint8_t(i * 7 + 3);
    return bytes;
}

gpu::ShaderSource fullscreen(const std::string& glsl, const std::vector<std::string>& names)
{
    return gpu::compileGlsl(azl::fullscreenEntries(glsl, names));
}

const char* const kTint = R"(#version 450
layout(set = 0, binding = 0, std140) uniform Tint { vec4 tint; };
layout(location = 0) out vec4 color;
#ifdef ENTRY_tint
void main() { color = tint; }
#endif
)";

} // namespace

TEST_CASE("textures of every format upload and read back byte for byte through the GPU interface")
{
    const gpu::Device device = gpu::Device::systemDefault();
    REQUIRE(device);
    const gpu::Queue queue = device.makeQueue();
    for (const gpu::Format format : {gpu::Format::BGRA8, gpu::Format::RGBA8, gpu::Format::RGBA16F, gpu::Format::RGBA16,
                                     gpu::Format::R8,
                                     gpu::Format::RG8, gpu::Format::R16, gpu::Format::RG16, gpu::Format::BC1,
                                     gpu::Format::BC3, gpu::Format::BC4}) {
        CAPTURE(int(format));
        const gpu::Texture texture =
            device.makeTexture({.format = format, .width = 64, .height = 32, .memory = gpu::Memory::Upload});
        REQUIRE(texture);
        CHECK(texture.format() == format);
        CHECK(texture.width() == 64);
        CHECK(texture.allocatedBytes() > 0);
        const size_t rowBytes = gpu::rowBytes(format, 64);
        const size_t rows = gpu::isCompressed(format) ? 8 : 32;
        const std::vector<uint8_t> bytes = pattern(rowBytes * rows);
        REQUIRE(device.upload(texture, 0, bytes.data(), rowBytes));
        CHECK(readBack(device, queue, texture) == bytes);
    }
}

TEST_CASE("a render pass clears, draws through a pipeline and signals a fence the CPU waits for")
{
    const gpu::Device device = gpu::Device::systemDefault();
    const gpu::Queue queue = device.makeQueue();
    const gpu::Texture target =
        device.makeTexture({.width = 8, .height = 8, .usage = gpu::Usage::RenderTarget | gpu::Usage::Sampled});
    std::string error;
    const gpu::ShaderModule module = device.makeShaderModule(fullscreen(kTint, {"tint"}), &error);
    REQUIRE_MESSAGE(module, error);
    const gpu::RenderPipeline pipeline = device.makeRenderPipeline(
        {module, gpu::entryPoint("fullscreen"), gpu::entryPoint("tint"), gpu::Format::BGRA8}, {}, nullptr, &error);
    REQUIRE_MESSAGE(pipeline, error);
    const gpu::Fence fence = device.makeFence();
    REQUIRE(fence);
    CHECK(fence.value() == 0);

    gpu::Scope scope;
    gpu::CommandBuffer cmd = queue.commandBuffer();
    gpu::RenderPass clear = cmd.begin(gpu::RenderTarget{&target, gpu::Load::Clear, {0.0, 0.0, 1.0, 1.0}});
    clear.end();
    gpu::RenderPass pass = cmd.begin(gpu::RenderTarget{&target, gpu::Load::Keep});
    pass.setPipeline(pipeline);
    const std::array<float, 4> tint{1.0f, 0.0f, 0.0f, 1.0f};
    pass.setBytes(0, tint.data(), sizeof(tint));
    pass.setScissor({0, 0, 4, 8});
    pass.draw(3);
    pass.end();
    cmd.signal(fence, 1);
    bool completed = false;
    cmd.onCompleted([&completed](const gpu::Completion& done) { completed = done.ok; });
    cmd.commit();
    CHECK(fence.wait(1, 5000));
    CHECK(fence.value() >= 1);
    cmd.waitUntilCompleted();
    CHECK(cmd.completion().gpuEnd >= cmd.completion().gpuStart);

    const std::vector<uint8_t> pixels = readBack(device, queue, target);
    // BGRA: the left half drawn red, the right half still the clear's blue.
    CHECK(pixels[0] == 0);
    CHECK(pixels[2] == 255);
    CHECK(pixels[7 * 4] == 255);
    CHECK(pixels[7 * 4 + 2] == 0);
}

TEST_CASE("an indexed draw reads its points and colour from buffers at offsets")
{
    const gpu::Device device = gpu::Device::systemDefault();
    const gpu::Queue queue = device.makeQueue();
    const char* const vertex = R"(#version 450
layout(set = 0, binding = 1, std430) readonly buffer Points { vec2 points[]; };
void main() { gl_Position = vec4(points[gl_VertexIndex], 0.0, 1.0); }
)";
    const char* const fragment = R"(#version 450
layout(set = 0, binding = 0, std430) readonly buffer Colour { vec4 colour; };
layout(location = 0) out vec4 color;
void main() { color = colour; }
)";
    std::string error;
    const std::vector<azl::ShaderEntry> entries{{"indexedVertex", azl::ShaderStage::Vertex, vertex},
                                                {"indexedPaint", azl::ShaderStage::Fragment, fragment}};
    const gpu::ShaderModule module = device.makeShaderModule({.msl = azl::mslSource(entries)}, &error);
    REQUIRE_MESSAGE(module, error);
    const gpu::RenderPipeline pipeline = device.makeRenderPipeline(
        {module, azl::mslFunction("indexedVertex"), azl::mslFunction("indexedPaint"), gpu::Format::BGRA8});
    REQUIRE(pipeline);
    // The left half of the target as two triangles, after 16 bytes the shader never reads.
    const std::array<float, 12> points{9.0f, 9.0f, 9.0f, 9.0f, -1.0f, -1.0f, 0.0f, -1.0f, 0.0f, 1.0f, -1.0f, 1.0f};
    const std::array<uint32_t, 8> indices{7, 7, 0, 1, 2, 0, 2, 3};
    const std::array<float, 8> colours{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f};
    const gpu::Buffer pointBuffer = device.makeBuffer(sizeof(points));
    const gpu::Buffer indexBuffer = device.makeBuffer(sizeof(indices));
    const gpu::Buffer colourBuffer = device.makeBuffer(sizeof(colours));
    std::memcpy(pointBuffer.contents(), points.data(), sizeof(points));
    std::memcpy(indexBuffer.contents(), indices.data(), sizeof(indices));
    std::memcpy(colourBuffer.contents(), colours.data(), sizeof(colours));
    const gpu::Texture target =
        device.makeTexture({.width = 8, .height = 8, .usage = gpu::Usage::RenderTarget | gpu::Usage::Sampled});
    {
        gpu::Scope scope;
        gpu::CommandBuffer cmd = queue.commandBuffer();
        gpu::RenderPass pass = cmd.begin(gpu::RenderTarget{&target, gpu::Load::Clear});
        pass.setPipeline(pipeline);
        pass.setVertexBuffer(1, pointBuffer, 4 * sizeof(float));
        pass.setBuffer(0, colourBuffer, 4 * sizeof(float));
        pass.drawIndexed(indexBuffer, 6, 2 * sizeof(uint32_t));
        pass.end();
        cmd.commit();
        cmd.waitUntilCompleted();
    }
    const std::vector<uint8_t> pixels = readBack(device, queue, target);
    // BGRA: green on the left, the clear's transparent black on the right.
    CHECK(pixels[1] == 255);
    CHECK(pixels[3] == 255);
    CHECK(pixels[7 * 4 + 1] == 0);
    CHECK(pixels[7 * 4 + 3] == 0);
}

TEST_CASE("a compute pass writes a storage texture")
{
    const gpu::Device device = gpu::Device::systemDefault();
    const gpu::Queue queue = device.makeQueue();
    const char* const fill = R"(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 1, binding = 0, rgba8) uniform writeonly image2D target;
void main() { imageStore(target, ivec2(gl_GlobalInvocationID.xy), vec4(0.0, 1.0, 0.0, 1.0)); }
)";
    std::string error;
    const gpu::ShaderModule module =
        device.makeShaderModule(gpu::compileGlsl({{"fill", azl::ShaderStage::Compute, fill}}), &error);
    REQUIRE_MESSAGE(module, error);
    const gpu::ComputePipeline pipeline = device.makeComputePipeline(module, gpu::entryPoint("fill"), &error);
    REQUIRE_MESSAGE(pipeline, error);
    CHECK(pipeline.maxThreads() >= 64);
    const gpu::Texture target = device.makeTexture(
        {.format = gpu::Format::RGBA8, .width = 16, .height = 16, .usage = gpu::Usage::Storage | gpu::Usage::Sampled});
    {
        gpu::Scope scope;
        gpu::CommandBuffer cmd = queue.commandBuffer();
        cmd.transition(target, gpu::Layout::Storage);
        gpu::ComputePass pass = cmd.beginCompute();
        pass.setPipeline(pipeline);
        pass.setTexture(0, target);
        pass.dispatch(16, 8, 8, 8);
        pass.dispatchGroups(2, 2, 8, 8);
        pass.end();
        cmd.commit();
        cmd.waitUntilCompleted();
    }
    const std::vector<uint8_t> pixels = readBack(device, queue, target);
    CHECK(pixels[1] == 255);
    CHECK(pixels[pixels.size() - 3] == 255);
    CHECK(pixels[pixels.size() - 4] == 0);
}

TEST_CASE("a pipeline cache file built once gives the same pipeline without compiling")
{
    const gpu::Device device = gpu::Device::systemDefault();
    if (device.name().find("Paravirtual") != std::string::npos)
        return;
    TempDir root;
    const std::string path = (root.path / "pipelines.bin").string();
    const gpu::ShaderModule module = device.makeShaderModule(fullscreen(kTint, {"tint"}));
    const gpu::RenderPipelineDesc desc{module, gpu::entryPoint("fullscreen"), gpu::entryPoint("tint"),
                                       gpu::Format::RGBA16F, gpu::Blending::Add};
    bool cached = true;
    {
        const gpu::PipelineCache cache = device.openPipelineCache(path);
        REQUIRE(cache);
        CHECK(device.makeRenderPipeline(desc, cache, &cached));
        CHECK(!cached);
        REQUIRE(device.savePipelineCache(cache, path));
    }
    const gpu::PipelineCache reopened = device.openPipelineCache(path);
    CHECK(device.makeRenderPipeline(desc, reopened, &cached));
    CHECK(cached);
}

TEST_CASE("timestamps around a render pass resolve in order where the device takes them")
{
    const gpu::Device device = gpu::Device::systemDefault();
    const gpu::TimestampMode mode = device.timestampMode();
    MESSAGE("timestamp mode " << int(mode));
    if (mode == gpu::TimestampMode::None)
        return;
    const gpu::Queue queue = device.makeQueue();
    const gpu::TimestampQueries queries = device.makeTimestampQueries(2);
    REQUIRE(queries);
    CHECK(queries.count() == 2);
    const gpu::Texture target = device.makeTexture({.width = 64, .height = 64, .usage = gpu::Usage::RenderTarget});
    const gpu::RenderPipeline pipeline = device.makeRenderPipeline(
        {device.makeShaderModule(fullscreen(kTint, {"tint"})), gpu::entryPoint("fullscreen"), gpu::entryPoint("tint"),
         gpu::Format::BGRA8});
    REQUIRE(pipeline);
    gpu::Scope scope;
    gpu::CommandBuffer cmd = queue.commandBuffer();
    gpu::RenderTarget rt{&target, gpu::Load::Clear};
    if (mode == gpu::TimestampMode::StageBoundary)
        rt.timestamps[1] = {&queries, 0, 1};
    // The pass draws: a pass without a fragment stage has no end of fragment to sample.
    gpu::RenderPass pass = cmd.begin(rt);
    if (mode == gpu::TimestampMode::DrawBoundary)
        pass.writeTimestamp(queries, 0);
    pass.setPipeline(pipeline);
    const std::array<float, 4> tint{1.0f, 1.0f, 1.0f, 1.0f};
    pass.setBytes(0, tint.data(), sizeof(tint));
    pass.draw(3);
    if (mode == gpu::TimestampMode::DrawBoundary)
        pass.writeTimestamp(queries, 1);
    pass.end();
    cmd.commit();
    cmd.waitUntilCompleted();
    std::array<uint64_t, 2> ticks{};
    REQUIRE(queries.resolve(0, ticks));
    CHECK(ticks[0] > 0);
    CHECK(ticks[1] >= ticks[0]);
}

namespace {

// Draws three vertices through entries[0] (vertex) and entries[1] (fragment), `source` at texture 0, into a new BGRA8
// target cleared to blue, and reads it back.
std::vector<uint8_t> drawn(const gpu::Device& device, const gpu::Queue& queue, const std::vector<azl::ShaderEntry>& entries,
                           uint32_t width, uint32_t height, const gpu::Texture* source = nullptr)
{
    std::string error;
    const gpu::ShaderModule module = device.makeShaderModule(gpu::compileGlsl(entries), &error);
    REQUIRE_MESSAGE(module, error);
    const gpu::RenderPipeline pipeline = device.makeRenderPipeline(
        {module, gpu::entryPoint(entries[0].name), gpu::entryPoint(entries[1].name), gpu::Format::BGRA8}, {}, nullptr,
        &error);
    REQUIRE_MESSAGE(pipeline, error);
    const gpu::Texture target =
        device.makeTexture({.width = width, .height = height, .usage = gpu::Usage::RenderTarget | gpu::Usage::Sampled});
    {
        gpu::Scope scope;
        gpu::CommandBuffer cmd = queue.commandBuffer();
        gpu::RenderPass pass = cmd.begin(gpu::RenderTarget{&target, gpu::Load::Clear, {0.0, 0.0, 1.0, 1.0}});
        pass.setPipeline(pipeline);
        if (source)
            pass.setTexture(0, *source);
        pass.draw(3);
        pass.end();
        cmd.commit();
    }
    return readBack(device, queue, target);
}

} // namespace

TEST_CASE("clip space points up on every backend: a triangle above y = 0 covers the top rows")
{
    const gpu::Device device = gpu::Device::systemDefault();
    const gpu::Queue queue = device.makeQueue();
    const char* const upper = R"(#version 450
void main()
{
    const vec2 corners[3] = vec2[](vec2(-1.0, 0.0), vec2(3.0, 0.0), vec2(-1.0, 4.0));
    gl_Position = vec4(corners[gl_VertexIndex], 0.0, 1.0);
}
)";
    const char* const red = R"(#version 450
layout(location = 0) out vec4 color;
void main() { color = vec4(1.0, 0.0, 0.0, 1.0); }
)";
    const std::vector<uint8_t> pixels =
        drawn(device, queue, {{"upper", azl::ShaderStage::Vertex, upper}, {"red", azl::ShaderStage::Fragment, red}}, 8,
              8);
    // BGRA: row 0 red, row 7 still the clear's blue.
    CHECK(pixels[2] == 255);
    CHECK(pixels[0] == 0);
    CHECK(pixels[7 * 8 * 4 + 2] == 0);
    CHECK(pixels[7 * 8 * 4] == 255);
}

TEST_CASE("the fixed samplers read texture rows from the top: pixel and normalised coordinates, text clamps to zero")
{
    const gpu::Device device = gpu::Device::systemDefault();
    const gpu::Queue queue = device.makeQueue();
    std::vector<uint8_t> texels(4 * 4 * 4);
    for (size_t i = 0; i < texels.size(); ++i)
        texels[i] = uint8_t(i * 3);
    const gpu::Texture source =
        device.makeTexture({.format = gpu::Format::RGBA8, .width = 4, .height = 4, .memory = gpu::Memory::Upload});
    REQUIRE(device.upload(source, 0, texels.data(), 16));
    const std::string reads = std::string(R"(#version 450
)") + azl::kGlslSamplers + R"(
layout(set = 1, binding = 0) uniform texture2D picture;
layout(location = 0) out vec4 color;
void main()
{
    if (gl_FragCoord.x < 1.0)
        color = textureLod(sampler2D(picture, pix), vec2(2.5, 1.5), 0.0);
    else if (gl_FragCoord.x < 2.0)
        color = texture(sampler2D(picture, lin), vec2(0.625, 0.375));
    else
        color = texture(sampler2D(picture, textSampler), vec2(-0.5, 0.5));
}
)";
    const std::vector<uint8_t> pixels = drawn(
        device, queue,
        {{"fullscreen", azl::ShaderStage::Vertex, azl::kFullscreenVertex}, {"reads", azl::ShaderStage::Fragment, reads}},
        3, 1, &source);
    // Texel (2, 1) of the RGBA upload, read back as BGRA.
    const size_t texel = (1 * 4 + 2) * 4;
    for (size_t x : {0, 1}) {
        CAPTURE(x);
        CHECK(pixels[x * 4 + 2] == texels[texel]);
        CHECK(pixels[x * 4 + 1] == texels[texel + 1]);
        CHECK(pixels[x * 4] == texels[texel + 2]);
    }
    CHECK(pixels[8] == 0);
    CHECK(pixels[9] == 0);
    CHECK(pixels[10] == 0);
    CHECK(pixels[11] == 0);
}

TEST_CASE("each blending mode combines a premultiplied colour with the target as its formula says")
{
    const gpu::Device device = gpu::Device::systemDefault();
    const gpu::Queue queue = device.makeQueue();
    std::string error;
    const gpu::ShaderModule module = device.makeShaderModule(fullscreen(kTint, {"tint"}), &error);
    REQUIRE_MESSAGE(module, error);
    // Target d = (0.2, 0.4, 0.8, 1), source s premultiplied with alpha 0.5.
    const std::array<double, 3> d{0.2, 0.4, 0.8};
    const std::array<float, 4> s{0.4f, 0.2f, 0.1f, 0.5f};
    struct Case {
        gpu::Blending blending;
        double (*formula)(double s, double d, double a);
    };
    const Case cases[] = {
        {gpu::Blending::Normal, [](double s, double d, double a) { return s + d * (1 - a); }},
        {gpu::Blending::Add, [](double s, double d, double) { return std::min(1.0, s + d); }},
        {gpu::Blending::Screen, [](double s, double d, double) { return s * (1 - d) + d; }},
        {gpu::Blending::Multiply, [](double s, double d, double a) { return s * d + d * (1 - a); }},
    };
    for (const Case& c : cases) {
        CAPTURE(int(c.blending));
        const gpu::RenderPipeline pipeline = device.makeRenderPipeline(
            {module, gpu::entryPoint("fullscreen"), gpu::entryPoint("tint"), gpu::Format::RGBA8, c.blending}, {},
            nullptr, &error);
        REQUIRE_MESSAGE(pipeline, error);
        const gpu::Texture target = device.makeTexture(
            {.format = gpu::Format::RGBA8, .width = 2, .height = 2, .usage = gpu::Usage::RenderTarget});
        {
            gpu::Scope scope;
            gpu::CommandBuffer cmd = queue.commandBuffer();
            gpu::RenderPass pass = cmd.begin(gpu::RenderTarget{&target, gpu::Load::Clear, {d[0], d[1], d[2], 1.0}});
            pass.setPipeline(pipeline);
            pass.setBytes(0, s.data(), sizeof(s));
            pass.draw(3);
            pass.end();
            cmd.commit();
        }
        const std::vector<uint8_t> pixels = readBack(device, queue, target);
        for (size_t i = 0; i < 3; ++i)
            CHECK(std::abs(int(pixels[i]) - int(std::lround(c.formula(s[i], d[i], s[3]) * 255.0))) <= 1);
    }
}

#if !defined(__APPLE__)
#include "gpu/gpu_vulkan.hpp"

TEST_CASE("the Vulkan validation layer checks every GPU call the tests make")
{
    REQUIRE(gpu::Device::systemDefault());
    CHECK(gpu::vulkan::validationEnabled());
}
#endif
