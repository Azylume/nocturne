#include "engine/shader_cross.hpp"
#include "gpu/gpu.hpp"

#include "test_client.hpp"

#include "doctest.h"

#include <array>
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
    return {.msl = azl::mslSource(azl::fullscreenEntries(glsl, names))};
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
        {module, azl::mslFunction("fullscreen"), azl::mslFunction("tint"), gpu::Format::BGRA8}, {}, nullptr, &error);
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
        device.makeShaderModule({.msl = azl::mslSource({{"fill", azl::ShaderStage::Compute, fill}})}, &error);
    REQUIRE_MESSAGE(module, error);
    const gpu::ComputePipeline pipeline = device.makeComputePipeline(module, azl::mslFunction("fill"), &error);
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
        pass.dispatch(16, 16, 8, 8);
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
    const gpu::RenderPipelineDesc desc{module, azl::mslFunction("fullscreen"), azl::mslFunction("tint"),
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
        {device.makeShaderModule(fullscreen(kTint, {"tint"})), azl::mslFunction("fullscreen"), azl::mslFunction("tint"),
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
