#include "engine/shader_cross.hpp"

#include "effects/shader_layout.hpp"

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <spirv_glsl.hpp>
#include <spirv_msl.hpp>
#include <spirv_parser.hpp>

#include <mutex>
#include <regex>
#include <stdexcept>

namespace azl {

namespace {

EShLanguage language(ShaderStage stage)
{
    switch (stage) {
    case ShaderStage::Vertex: return EShLangVertex;
    case ShaderStage::Fragment: return EShLangFragment;
    case ShaderStage::Compute: return EShLangCompute;
    }
    return EShLangFragment;
}

// The name without a function's signature, and never starting with an underscore, which after the prefix's would make
// a reserved name.
std::string ownName(const std::string& name, uint32_t id)
{
    const std::string own = name.substr(0, name.find('('));
    return own.empty() ? "id" + std::to_string(id) : own.front() == '_' ? "u" + own : own;
}

// Every global name the pasted GLSL would declare: functions but the entry point, variables outside functions,
// structures, constants and undefined values (which SPIRV-Cross declares as globals).
void prefixNames(spirv_cross::ParsedIR& ir, const std::string& prefix)
{
    using namespace spirv_cross;
    auto name = [&](ID id) { ir.set_name(id, prefix + ownName(ir.get_name(id), uint32_t(id))); };
    const ID entry = ir.default_entry_point;
    for (ID id : ir.ids_for_type[TypeFunction])
        if (id != entry)
            name(id);
    for (ID id : ir.ids_for_type[TypeVariable])
        if (ir.ids[id].get<SPIRVariable>().storage != spv::StorageClassFunction)
            name(id);
    for (ID id : ir.ids_for_type[TypeType])
        if (ir.ids[id].get<SPIRType>().basetype == SPIRType::Struct)
            name(id);
    for (Types type : {TypeConstant, TypeConstantOp, TypeUndef})
        for (ID id : ir.ids_for_type[type])
            name(id);
}

// SPIRV-Cross's GLSL less what only a whole shader has.
std::string pasteable(const std::string& glsl)
{
    static const std::regex directives(R"((^|\n)#(version|extension)[^\n]*)");
    static const std::regex interface(R"((^|\n)layout\([^\n]*\) (in|out|uniform) [^\n]*;)");
    std::string out = std::regex_replace(glsl, directives, "$1");
    out = std::regex_replace(out, interface, "$1");
    const size_t main = out.find("\nvoid main()");
    return main == std::string::npos ? out : out.substr(0, main + 1);
}

spirv_cross::MSLConstexprSampler constexprSampler(SamplerKind kind)
{
    using namespace spirv_cross;
    MSLConstexprSampler s;
    s.min_filter = s.mag_filter = MSL_SAMPLER_FILTER_LINEAR;
    if (kind == SamplerKind::Pixel)
        s.coord = MSL_SAMPLER_COORD_PIXEL;
    if (kind == SamplerKind::Text) {
        s.mip_filter = MSL_SAMPLER_MIP_FILTER_LINEAR;
        s.s_address = s.t_address = s.r_address = MSL_SAMPLER_ADDRESS_CLAMP_TO_ZERO;
    }
    return s;
}

// One entry's MSL inside its namespace, without the lines every source starts with.
std::string namespaced(const std::string& msl, const std::string& name)
{
    static const std::regex header(R"((^|\n)(#include|#pragma|using namespace metal;)[^\n]*)");
    return "\nnamespace " + name + " {\n" + std::regex_replace(msl, header, "$1") + "\n}\n";
}

std::string entryMsl(const ShaderEntry& entry, std::string& error)
{
    std::vector<uint32_t> spirv = glslToSpirv(entry.stage, entry.glsl, error);
    if (spirv.empty())
        return {};
    try {
        spirv_cross::CompilerMSL msl(std::move(spirv));
        spirv_cross::CompilerMSL::Options options;
        options.set_msl_version(2, 1);
        options.enable_decoration_binding = true;
        options.force_native_arrays = true;
        msl.set_msl_options(options);
        for (uint32_t kind = 0; kind < kSamplerKinds; ++kind)
            msl.remap_constexpr_sampler_by_binding(kSamplerSet, kind, constexprSampler(SamplerKind(kind)));
        return namespaced(msl.compile(), entry.name);
    } catch (const std::exception& e) {
        error = entry.name + ": " + e.what();
        return {};
    }
}

constexpr const char* kMslHeader = R"(#pragma clang diagnostic ignored "-Wmissing-prototypes"
#pragma clang diagnostic ignored "-Wmissing-braces"
#pragma clang diagnostic ignored "-Wunused-variable"
#include <metal_stdlib>
#include <simd/simd.h>
using namespace metal;
)";

} // namespace

std::vector<uint32_t> glslToSpirv(ShaderStage stage, const std::string& glsl, std::string& error)
{
    static std::once_flag initialised;
    std::call_once(initialised, [] { glslang::InitializeProcess(); });

    const EShLanguage lang = language(stage);
    glslang::TShader shader(lang);
    const char* text = glsl.c_str();
    shader.setStrings(&text, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    shader.setAutoMapLocations(true);
    const auto messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 450, false, messages)) {
        error = shader.getInfoLog();
        return {};
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        error = program.getInfoLog();
        return {};
    }
    std::vector<uint32_t> spirv;
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv);
    return spirv;
}

CrossResult spirvToGlsl(std::vector<uint32_t> spirv, const std::string& prefix)
{
    CrossResult result;
    try {
        spirv_cross::Parser parser(std::move(spirv));
        parser.parse();
        prefixNames(parser.get_parsed_ir(), prefix);
        spirv_cross::CompilerGLSL glsl(std::move(parser.get_parsed_ir()));
        spirv_cross::CompilerGLSL::Options options;
        options.version = 450;
        options.vulkan_semantics = true;
        glsl.set_common_options(options);
        result.text = pasteable(glsl.compile());
    } catch (const std::exception& e) {
        result.error = e.what();
    }
    return result;
}

CrossResult glslToMsl(const std::vector<ShaderEntry>& entries)
{
    CrossResult result;
    std::string msl = kMslHeader;
    for (const ShaderEntry& entry : entries) {
        const std::string one = entryMsl(entry, result.error);
        if (one.empty())
            return result;
        msl += one;
    }
    result.text = std::move(msl);
    return result;
}

std::string mslSource(const std::vector<ShaderEntry>& entries)
{
    CrossResult msl = glslToMsl(entries);
    if (msl.text.empty())
        throw std::runtime_error("shader compile failed: " + msl.error);
    return std::move(msl.text);
}

std::vector<ShaderEntry> fullscreenEntries(const std::string& glsl, const std::vector<std::string>& names)
{
    std::vector<ShaderEntry> entries{{"fullscreen", ShaderStage::Vertex, kFullscreenVertex}};
    for (const std::string& name : names)
        entries.push_back({name, ShaderStage::Fragment, withEntry(glsl, name)});
    return entries;
}

std::string mslFunction(const std::string& entry)
{
    return entry + "::main0";
}

} // namespace azl
