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
constexpr const char* kCacheVersion = "isf-3";

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

bool sampled(const IsfShader& shader)
{
    return shader.kind == IsfKind::Effect && !shader.pointwise;
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
           "vec4 isf_sample(vec2 c) { return texture(inputImage, vec2(c.x, 1.0 - c.y)); }\n"
           "#define IMG_NORM_PIXEL(i, c) isf_sample(c)\n#define IMG_PIXEL(i, c) isf_sample((c) / RENDERSIZE)\n"
           "#define IMG_THIS_PIXEL(i) isf_sample(isf_FragNormCoord)\n"
           "#define IMG_THIS_NORM_PIXEL(i) isf_sample(isf_FragNormCoord)\n";
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

void store(const std::filesystem::path& dir, const std::filesystem::path& file, const std::string& glsl)
{
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path written = file.string() + ".new";
    {
        std::ofstream out(written, std::ios::binary | std::ios::trunc);
        out << glsl;
        if (!out)
            return;
    }
    std::filesystem::rename(written, file, ec);
}

// isf_run's arguments after the frame and view: the input colour, the two pictures, then one per input.
std::vector<std::string> runArguments(const IsfShader& shader)
{
    std::vector<std::string> types{"vec4", "vec4"};
    if (shader.kind == IsfKind::Effect && shader.pointwise)
        types.push_back("vec4");
    if (shader.kind == IsfKind::Transition)
        types.insert(types.end(), {"vec4", "vec4"});
    for (const IsfInput& input : shader.inputs)
        types.push_back(carrier(input.type));
    return types;
}

// isf_run sets ISF's built-ins and the inputs from its arguments, runs the shader's main and returns its colour; the
// shader's own main calls it from stage inputs only so that it compiles as a whole shader.
std::string runFunction(const IsfShader& shader)
{
    const std::vector<std::string> types = runArguments(shader);
    std::string out = "vec4 isf_run(";
    for (size_t i = 0; i < types.size(); ++i)
        out += (i ? ", " : "") + types[i] + " a" + std::to_string(i);
    out += ")\n{\n"
           "    isf_FragCoord = vec4(a0.xy, 0.0, 1.0);\n"
           "    TIME = a0.z; TIMEDELTA = a0.w;\n"
           "    RENDERSIZE = a1.xy; FRAMEINDEX = int(a1.z); PASSINDEX = 0; DATE = vec4(0.0);\n"
           "    isf_FragNormCoord = a0.xy / RENDERSIZE;\n";
    size_t next = 2;
    if (shader.kind == IsfKind::Effect && shader.pointwise)
        out += "    isf_colour = a" + std::to_string(next++) + ";\n";
    if (shader.kind == IsfKind::Transition) {
        out += "    progress = a1.w; isf_startImage = a" + std::to_string(next) + "; isf_endImage = a" +
               std::to_string(next + 1) + ";\n";
        next += 2;
    }
    for (size_t i = 0; i < shader.inputs.size(); ++i)
        out += "    isf_p" + std::to_string(i) + " = a" + std::to_string(next++) + ";\n";
    for (size_t i = 0; i < shader.inputs.size(); ++i)
        out += "    " + shader.inputs[i].name + " = " + inputValue(shader.inputs[i], i) + ";\n";
    out += "    isf_main();\n    return isf_out;\n}\n";

    std::string call;
    for (size_t i = 0; i < types.size(); ++i) {
        out += "layout(location = " + std::to_string(i) + ") in " + types[i] + " isf_in" + std::to_string(i) + ";\n";
        call += (i ? ", isf_in" : "isf_in") + std::to_string(i);
    }
    return out + "layout(location = 0) out vec4 isf_result;\nvoid main() { isf_result = isf_run(" + call + "); }\n";
}

std::string assembled(const IsfShader& shader, const std::string& glsl)
{
    std::string out = "#version 450\n"
                      "vec4 isf_out;\n"
                      "float TIME; float TIMEDELTA; int FRAMEINDEX; int PASSINDEX; vec2 RENDERSIZE; vec4 DATE;\n"
                      "vec2 isf_FragNormCoord; vec4 isf_FragCoord;\n";
    if (shader.kind == IsfKind::Effect && shader.pointwise)
        out += "vec4 isf_colour;\n";
    if (shader.kind == IsfKind::Transition)
        out += "vec4 isf_startImage; vec4 isf_endImage; float progress;\n";
    for (size_t i = 0; i < shader.inputs.size(); ++i) {
        const IsfInput& input = shader.inputs[i];
        out += std::string(carrier(input.type)) + " isf_p" + std::to_string(i) + ";\n";
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
    return out + "\n#undef main\n" + runFunction(shader);
}

std::string prefix(const IsfShader& shader)
{
    return "isf_" + shader.type + "_";
}

// The pasted shader reads its picture from the pass's input texture.
std::string pasted(const IsfShader& shader, const std::string& glsl)
{
    if (!sampled(shader))
        return glsl;
    return "\n#define " + prefix(shader) + "inputImage sampler2D(t0, lin)\n" + glsl;
}

// The prefixed GLSL, cached by the shader as written, so a start with the cache compiles nothing.
std::string translated(const IsfShader& shader, const std::filesystem::path& cacheDir, std::string& error)
{
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.glsl",
                  static_cast<unsigned long long>(
                      fnv(std::string(kCacheVersion) + "\n" + prefix(shader) + "\n" + assembled(shader, shader.glsl))));
    const std::filesystem::path file = cacheDir.empty() ? std::filesystem::path() : cacheDir / name;
    if (!cacheDir.empty())
        if (std::string glsl = cached(file); !glsl.empty())
            return glsl;
    std::vector<uint32_t> spirv = glslToSpirv(ShaderStage::Fragment, isfGlsl(shader), error);
    if (spirv.empty())
        return {};
    CrossResult result = spirvToGlsl(std::move(spirv), prefix(shader));
    error = std::move(result.error);
    if (result.text.empty())
        return {};
    std::string glsl = pasted(shader, result.text);
    if (!cacheDir.empty())
        store(cacheDir, file, glsl);
    return glsl;
}

std::string inputArguments(const IsfShader& shader, const std::string& values)
{
    std::string out;
    size_t offset = 0;
    for (const IsfInput& input : shader.inputs) {
        const size_t n = width(input.type);
        std::string value = n == 1 ? "" : std::string(n == 4 ? "vec4(" : "vec2(");
        for (size_t k = 0; k < n; ++k)
            value += (k ? ", " : "") + values + std::to_string(offset + k) + "]";
        out += ", " + value + (n == 1 ? "" : ")");
        offset += n;
    }
    return out;
}

std::string callBody(const IsfShader& shader)
{
    const bool transition = shader.kind == IsfKind::Transition;
    std::string out = "\n";
    if (transition)
        out += "    vec2 px = vec2(p) + 0.5;\n";
    out += "    return " + prefix(shader) + "isf_run(vec4(px.x, h.size.y - px.y, h.time, h.dt), vec4(h.size, " +
           "floor(h.time * 60.0), " + (transition ? "progress" : "0.0") + ")";
    if (shader.kind == IsfKind::Effect && shader.pointwise)
        out += ", c";
    if (transition)
        out += ", from, to";
    return out + inputArguments(shader, transition ? "m.params[" : "pb[p + ") + ");\n";
}

} // namespace

std::string isfGlsl(const IsfShader& shader)
{
    return assembled(shader, prepared(shader.glsl));
}

IsfGlsl translateIsf(const IsfShader& shader, const std::filesystem::path& cacheDir)
{
    IsfGlsl glsl;
    glsl.globals = translated(shader, cacheDir, glsl.error);
    if (glsl.globals.empty())
        return glsl;
    glsl.body = callBody(shader);
    return glsl;
}

void applyIsf(const IsfShader& shader, const IsfGlsl& glsl, const std::string& type)
{
    if (shader.kind == IsfKind::Transition) {
        if (!glsl.error.empty())
            return;
        IsfTransition transition = isfTransition(shader);
        transition.globals = glsl.globals;
        transition.function = glsl.body;
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
    EffectDef def = isfEffectDef(shader, glsl);
    setEffectShader(type, std::move(def.globals), std::move(def.glsl));
}

EffectDef isfEffectDef(const IsfShader& shader, const IsfGlsl& glsl)
{
    EffectDef def = isfEffect(shader);
    if (glsl.error.empty()) {
        def.globals = glsl.globals;
        def.glsl = glsl.body;
        return def;
    }
    std::string line = glsl.error.substr(0, glsl.error.find('\n'));
    std::replace_if(line.begin(), line.end(), [](char c) { return c == '"' || c == '\'' || c == '\\'; }, ' ');
    def.glsl = "\n#error ISF " + shader.name + ": " + line + "\n";
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
            const IsfGlsl glsl = translateIsf(shader, cacheDir);
            if (!glsl.error.empty())
                std::fprintf(stderr, "effects: ISF %s does not translate: %s\n", sources[i].name.c_str(),
                             glsl.error.c_str());
            applyIsf(shader, glsl, shader.type);
        }
    });
}

} // namespace azl
