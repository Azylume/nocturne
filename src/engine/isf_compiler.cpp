#include "engine/isf_compiler.hpp"

#include "engine/shader_cross.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>

namespace azl {

namespace {

// Changes whenever the GLSL or the translation changes meaning, so older cache entries are not read.
constexpr const char* kCacheVersion = "isf-2";
constexpr int kFirstInput = 5;

const char* glslType(IsfInputType type)
{
    switch (type) {
    case IsfInputType::Float: return "float";
    case IsfInputType::Bool: return "bool";
    case IsfInputType::Long: return "int";
    case IsfInputType::Colour: return "vec4";
    case IsfInputType::Point: return "vec2";
    }
    return "float";
}

size_t width(IsfInputType type)
{
    return type == IsfInputType::Colour ? 4 : type == IsfInputType::Point ? 2 : 1;
}

const char* carrier(IsfInputType type)
{
    return width(type) == 4 ? "vec4" : width(type) == 2 ? "vec2" : "float";
}

std::string inputValue(const IsfInput& input, size_t i)
{
    const std::string p = "isf_p" + std::to_string(i);
    switch (input.type) {
    case IsfInputType::Bool: return p + " >= 0.5";
    case IsfInputType::Long:
        return "isf_values" + std::to_string(i) + "[clamp(int(" + p + " + 0.5), 0, " +
               std::to_string(input.values.size() - 1) + ")]";
    default: return p;
    }
}

std::string longTable(const IsfInput& input, size_t i)
{
    std::string out = "const int isf_values" + std::to_string(i) + "[" + std::to_string(input.values.size()) + "] = int[](";
    for (size_t k = 0; k < input.values.size(); ++k)
        out += (k ? ", " : "") + std::to_string(input.values[k]);
    return out + ");\n";
}

std::string pictureMacros(const IsfShader& shader)
{
    if (shader.kind == IsfKind::Transition)
        return "#define IMG_NORM_PIXEL(i, c) isf_##i\n#define IMG_PIXEL(i, c) isf_##i\n"
               "#define IMG_THIS_PIXEL(i) isf_##i\n#define IMG_THIS_NORM_PIXEL(i) isf_##i\n";
    if (shader.pointwise)
        return "#define IMG_NORM_PIXEL(i, c) isf_colour\n#define IMG_THIS_PIXEL(i) isf_colour\n"
               "#define IMG_THIS_NORM_PIXEL(i) isf_colour\n";
    if (shader.kind == IsfKind::Generator)
        return "";
    return "layout(binding = 0) uniform sampler2D inputImage;\n"
           "vec4 isf_sample(sampler2D s, vec2 c) { return texture(s, vec2(c.x, 1.0 - c.y)); }\n"
           "#define IMG_NORM_PIXEL(i, c) isf_sample(i, c)\n#define IMG_PIXEL(i, c) isf_sample(i, (c) / RENDERSIZE)\n"
           "#define IMG_THIS_PIXEL(i) isf_sample(i, isf_FragNormCoord)\n"
           "#define IMG_THIS_NORM_PIXEL(i) isf_sample(i, isf_FragNormCoord)\n";
}

// No #version line, and GL_ES taken as defined: the desktop-only fallbacks in ISF shaders redefine built-ins, which
// GLSL 4.50 refuses, and GL_ names cannot be defined.
std::string prepared(std::string glsl)
{
    static const std::regex version(R"((^|\n)[ \t]*#version[^\n]*)");
    static const std::regex ifdef(R"(#ifdef[ \t]+GL_ES\b)");
    static const std::regex ifndef(R"(#ifndef[ \t]+GL_ES\b)");
    static const std::regex defined(R"(defined\s*\(?\s*GL_ES\s*\)?)");
    glsl = std::regex_replace(glsl, version, "$1");
    glsl = std::regex_replace(glsl, ifdef, "#if 1");
    glsl = std::regex_replace(glsl, ifndef, "#if 0");
    return std::regex_replace(glsl, defined, "1");
}

uint64_t fnv(const std::string& text)
{
    uint64_t hash = 14695981039346656037ull;
    for (const char c : text)
        hash = (hash ^ uint8_t(c)) * 1099511628211ull;
    return hash;
}

std::string cached(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return in ? text.str() : std::string();
}

void store(const std::filesystem::path& dir, const std::filesystem::path& file, const std::string& msl)
{
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path written = file.string() + ".new";
    {
        std::ofstream out(written, std::ios::binary | std::ios::trunc);
        out << msl;
        if (!out)
            return;
    }
    std::filesystem::rename(written, file, ec);
}

// The translated shader as plain functions in a namespace: no includes, no entry point, no attributes.
std::string namespaced(const std::string& msl, const std::string& space)
{
    static const std::regex includes(R"((^|\n)[ \t]*(#include[^\n]*|using namespace metal;))");
    static const std::regex attributes(R"(\s*\[\[[^\]]*\]\])");
    std::string out = std::regex_replace(msl, includes, "$1");
    out = std::regex_replace(out, attributes, "");
    out = std::regex_replace(out, std::regex(R"(\bfragment\s+main0_out\s+main0\s*\()"), "main0_out main0(");
    return "\nnamespace " + space + " {\n" + out + "\n}\n";
}

std::string inputAssignments(const IsfShader& shader, const std::string& values)
{
    std::string out;
    size_t offset = 0;
    for (size_t i = 0; i < shader.inputs.size(); ++i) {
        const size_t n = width(shader.inputs[i].type);
        std::string value = n == 1 ? "" : std::string(n == 4 ? "float4(" : "float2(");
        for (size_t k = 0; k < n; ++k)
            value += (k ? ", " : "") + values + "[" + std::to_string(offset + k) + "]";
        out += "    i.isf_p" + std::to_string(i) + " = " + value + (n == 1 ? "" : ")") + ";\n";
        offset += n;
    }
    return out;
}

std::string callBody(const IsfShader& shader, const std::string& space)
{
    const bool transition = shader.kind == IsfKind::Transition;
    std::string out = "\n    " + space + "::main0_in i = {};\n";
    if (transition)
        out += "    float2 px = float2(p) + 0.5;\n";
    out += "    i.isf_frame = float4(px.x, h.size.y - px.y, h.time, h.dt);\n";
    out += std::string("    i.isf_view = float4(h.size, floor(h.time * 60.0), ") + (transition ? "progress" : "0.0") +
           ");\n";
    if (shader.kind == IsfKind::Effect && shader.pointwise)
        out += "    i.isf_colour = c;\n";
    if (transition)
        out += "    i.isf_from = from;\n    i.isf_to = to;\n";
    out += inputAssignments(shader, transition ? "tp" : "p");
    const bool sampled = shader.kind == IsfKind::Effect && !shader.pointwise;
    return out + "    return " + space + "::main0(i" + (sampled ? ", t, lin" : "") + ").isf_out;\n";
}

std::string assembled(const IsfShader& shader, const std::string& glsl)
{
    std::string out = "#version 450\n"
                      "layout(location = 0) in vec4 isf_frame;\n"
                      "layout(location = 1) in vec4 isf_view;\n"
                      "layout(location = 0) out vec4 isf_out;\n"
                      "float TIME; float TIMEDELTA; int FRAMEINDEX; int PASSINDEX; vec2 RENDERSIZE; vec4 DATE;\n"
                      "vec2 isf_FragNormCoord; vec4 isf_FragCoord;\n";
    if (shader.kind == IsfKind::Effect && shader.pointwise)
        out += "layout(location = 2) in vec4 isf_colour;\n";
    if (shader.kind == IsfKind::Transition)
        out += "layout(location = 3) in vec4 isf_from;\nlayout(location = 4) in vec4 isf_to;\n"
               "vec4 isf_startImage; vec4 isf_endImage; float progress;\n";
    int location = kFirstInput;
    for (size_t i = 0; i < shader.inputs.size(); ++i) {
        const IsfInput& input = shader.inputs[i];
        out += "layout(location = " + std::to_string(location) + ") in " + carrier(input.type) + " isf_p" +
               std::to_string(i) + ";\n";
        location += 1;
        out += std::string(glslType(input.type)) + " " + input.name + ";\n";
        if (input.type == IsfInputType::Long)
            out += longTable(input, i);
    }
    out += pictureMacros(shader);
    out += "#define gl_FragColor isf_out\n#define gl_FragCoord isf_FragCoord\n"
           "#define vv_FragNormCoord isf_FragNormCoord\n#define texture2D texture\n#define IMG_SIZE(i) RENDERSIZE\n"
           // ISF v1 shaders read the picture's rect in pixels; ours is never a part of a larger image.
           "#define _inputImage_imgRect vec4(vec2(0.0), IMG_SIZE(inputImage))\n"

           "#define main isf_main\n#line 1\n";
    out += glsl;
    out += "\n#undef main\nvoid main() {\n"
           "    isf_FragCoord = vec4(isf_frame.xy, 0.0, 1.0);\n"
           "    TIME = isf_frame.z; TIMEDELTA = isf_frame.w;\n"
           "    RENDERSIZE = isf_view.xy; FRAMEINDEX = int(isf_view.z); PASSINDEX = 0; DATE = vec4(0.0);\n"
           "    isf_FragNormCoord = isf_frame.xy / RENDERSIZE;\n";
    if (shader.kind == IsfKind::Transition)
        out += "    progress = isf_view.w; isf_startImage = isf_from; isf_endImage = isf_to;\n";
    for (size_t i = 0; i < shader.inputs.size(); ++i)
        out += "    " + shader.inputs[i].name + " = " + inputValue(shader.inputs[i], i) + ";\n";
    return out + "    isf_main();\n}\n";
}

// The namespaced translation, cached by the shader as written, so a start with the cache runs no regular expression.
std::string translated(const IsfShader& shader, const std::string& space, const std::filesystem::path& cacheDir,
                       std::string& error)
{
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.metal",
                  static_cast<unsigned long long>(
                      fnv(std::string(kCacheVersion) + "\n" + space + "\n" + assembled(shader, shader.glsl))));
    const std::filesystem::path file = cacheDir.empty() ? std::filesystem::path() : cacheDir / name;
    if (!cacheDir.empty())
        if (std::string msl = cached(file); !msl.empty())
            return msl;
    CrossResult result = fragmentGlslToMsl(assembled(shader, prepared(shader.glsl)));
    error = std::move(result.error);
    if (result.msl.empty())
        return {};
    std::string msl = namespaced(result.msl, space);
    if (!cacheDir.empty())
        store(cacheDir, file, msl);
    return msl;
}

} // namespace

std::string isfGlsl(const IsfShader& shader)
{
    return assembled(shader, prepared(shader.glsl));
}

IsfMetal translateIsf(const IsfShader& shader, const std::filesystem::path& cacheDir)
{
    IsfMetal metal;
    const std::string space = "isf_" + shader.type;
    metal.globals = translated(shader, space, cacheDir, metal.error);
    if (metal.globals.empty())
        return metal;
    metal.body = callBody(shader, space);
    return metal;
}

void applyIsf(const IsfShader& shader, const IsfMetal& metal, const std::string& type)
{
    if (shader.kind == IsfKind::Transition) {
        if (!metal.error.empty())
            return;
        IsfTransition transition = isfTransition(shader);
        transition.globals = metal.globals;
        transition.function = metal.body;
        IsfTransitions transitions = *isfTransitions();
        auto it = std::find_if(transitions.begin(), transitions.end(),
                               [&](const IsfTransition& t) { return t.mode == transition.mode; });
        if (it == transitions.end())
            transitions.push_back(std::move(transition));
        else
            *it = std::move(transition);
        setIsfTransitions(std::move(transitions));
        return;
    }
    EffectDef def = isfEffectDef(shader, metal);
    setEffectShader(type, std::move(def.globals), std::move(def.metal));
}

EffectDef isfEffectDef(const IsfShader& shader, const IsfMetal& metal)
{
    EffectDef def = isfEffect(shader);
    if (metal.error.empty()) {
        def.globals = metal.globals;
        def.metal = metal.body;
        return def;
    }
    std::string line = metal.error.substr(0, metal.error.find('\n'));
    std::replace_if(line.begin(), line.end(), [](char c) { return c == '"' || c == '\'' || c == '\\'; }, ' ');
    def.metal = "\n#error ISF " + shader.name + ": " + line + "\n";
    return def;
}

void prepareIsf(const std::filesystem::path& cacheDir)
{
    static std::once_flag once;
    std::call_once(once, [&] {
        const std::vector<IsfSource>& sources = isfSources();
        for (size_t i = 0; i < sources.size(); ++i) {
            const auto& parsed = parsedIsfSources()[i];
            if (const auto* error = std::get_if<std::string>(&parsed)) {
                std::fprintf(stderr, "effects: ISF %s not loaded: %s\n", sources[i].name.c_str(), error->c_str());
                continue;
            }
            const IsfShader& shader = std::get<IsfShader>(parsed);
            const IsfMetal metal = translateIsf(shader, cacheDir);
            if (!metal.error.empty())
                std::fprintf(stderr, "effects: ISF %s does not translate: %s\n", sources[i].name.c_str(),
                             metal.error.c_str());
            applyIsf(shader, metal, shader.type);
        }
    });
}

} // namespace azl
