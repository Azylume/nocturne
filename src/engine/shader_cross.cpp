#include "engine/shader_cross.hpp"

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <spirv_msl.hpp>

#include <mutex>
#include <vector>

namespace azl {

namespace {

bool toSpirv(const std::string& glsl, std::vector<uint32_t>& spirv, std::string& error)
{
    static std::once_flag initialised;
    std::call_once(initialised, [] { glslang::InitializeProcess(); });

    glslang::TShader shader(EShLangFragment);
    const char* text = glsl.c_str();
    shader.setStrings(&text, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, EShLangFragment, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    shader.setAutoMapLocations(true);
    shader.setAutoMapBindings(true);
    const auto messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 450, false, messages)) {
        error = shader.getInfoLog();
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        error = program.getInfoLog();
        return false;
    }
    glslang::GlslangToSpv(*program.getIntermediate(EShLangFragment), spirv);
    return true;
}

} // namespace

CrossResult fragmentGlslToMsl(const std::string& glsl)
{
    std::vector<uint32_t> spirv;
    CrossResult result;
    if (!toSpirv(glsl, spirv, result.error))
        return result;
    try {
        spirv_cross::CompilerMSL msl(std::move(spirv));
        spirv_cross::CompilerMSL::Options options;
        options.set_msl_version(2, 1);
        msl.set_msl_options(options);
        result.msl = msl.compile();
    } catch (const std::exception& e) {
        result.error = e.what();
    }
    return result;
}

} // namespace azl
