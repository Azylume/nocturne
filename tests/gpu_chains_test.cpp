#include "effects/chain_planner.hpp"
#include "effects/effect_registry.hpp"
#include "engine/isf_compiler.hpp"
#include "gpu/glsl.hpp"
#include "gpu/gpu.hpp"

#include "doctest.h"

#include <string>
#include <vector>

namespace gpu = azl::gpu;

namespace {

// Every pipeline the chain compiler makes for the pass: its fragment entries into the pass's target format.
void buildPipelines(const gpu::Device& device, const azl::PlannedPass& pass)
{
    std::string error;
    const std::vector<std::string> names = azl::passEntries(pass);
    const gpu::ShaderModule module =
        device.makeShaderModule(gpu::compileGlsl(azl::fullscreenEntries(pass.glsl, names)), &error);
    REQUIRE_MESSAGE(module, error);
    const gpu::Format format = pass.float16 && !pass.intoComposition ? gpu::Format::RGBA16F : gpu::Format::BGRA8;
    for (const std::string& name : names) {
        CAPTURE(name);
        const gpu::Blending blending = pass.intoComposition && name.ends_with("Main")
                                           ? gpu::Blending::Normal
                                           : gpu::Blending::None;
        CHECK_MESSAGE(device.makeRenderPipeline(
                          {module, gpu::entryPoint("fullscreen"), gpu::entryPoint(name), format, blending}, {},
                          nullptr, &error),
                      error);
    }
}

void buildChain(const gpu::Device& device, azl::SourceKind source, const azl::EffectDef* effect)
{
    for (const azl::PlannedPass& pass : azl::planChain(source, {effect}))
        buildPipelines(device, pass);
}

// The built-in effects and generators, taken before any test case registers its own.
const std::vector<std::string> kEffects = azl::effectTypes();
const std::vector<std::string> kGenerators = azl::generatorTypes();

} // namespace

TEST_CASE("every effect and generator, and every source, builds its passes' pipelines on the GPU interface")
{
    const gpu::Device device = gpu::Device::systemDefault();
    REQUIRE(device);
    azl::prepareIsf({});
    for (const std::string& type : kEffects) {
        CAPTURE(type);
        buildChain(device, azl::SourceKind::TestCard, azl::findEffect(type));
    }
    for (const std::string& type : kGenerators) {
        CAPTURE(type);
        buildChain(device, azl::SourceKind::Generator, azl::findEffect(type));
    }
    const azl::EffectDef* pointwise = azl::findEffect(kEffects.front());
    for (const azl::SourceKind source : {azl::SourceKind::Yuv, azl::SourceKind::Texture, azl::SourceKind::Group,
                                         azl::SourceKind::Text, azl::SourceKind::YuvFade}) {
        CAPTURE(int(source));
        buildChain(device, source, pointwise);
    }
}
