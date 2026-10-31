#include "effects/chain_planner.hpp"

#include "effects/isf.hpp"
#include "effects/shader_layout.hpp"

#include <algorithm>
#include <set>

namespace azl {

namespace {

constexpr const char* kVersion = "#version 450\n";

constexpr const char* kExtensions = R"(#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_samplerless_texture_functions : require
)";

constexpr const char* kPrelude = R"(

struct Header {
    vec4 yuv[3]; vec2 size; float time; float opacity; uint blend; uint turns; float beat; uint blank; float dt;
};

layout(set = 0, binding = 0, scalar) uniform HeaderBlock { Header h; };

layout(location = 0) out vec4 fragColour;

const float M_PI_F = 3.14159265358979323846;

float saturate(float x) { return clamp(x, 0.0, 1.0); }
vec2 saturate(vec2 x) { return clamp(x, 0.0, 1.0); }
vec3 saturate(vec3 x) { return clamp(x, 0.0, 1.0); }
vec4 saturate(vec4 x) { return clamp(x, 0.0, 1.0); }

float distanceSquared(vec2 a, vec2 b)
{
    vec2 d = a - b;
    return dot(d, d);
}

// A texture read at pixel coordinates, filtered between pixels.
vec4 samplePixel(texture2D t, vec2 px) { return textureLod(sampler2D(t, pix), px, 0.0); }

// 60 Hz frames since the previous rendered frame, in 1/256 steps so a steady 60 frames per second is exactly 1.
float frames60(Header h) { return round(h.dt * 15360.0) / 256.0; }

// A factor per 60 Hz frame over this frame's elapsed time.
float perFrame(float x, Header h)
{
    float n = frames60(h);
    return n == 1.0 ? x : pow(x, n);
}

uint pcg(uint n)
{
    n = n * 747796405u + 2891336453u;
    n = ((n >> ((n >> 28u) + 4u)) ^ n) * 277803737u;
    return (n >> 22u) ^ n;
}
)";

// Alignment test card: a square grid, colour bars, a grey ramp, a circle for aspect, a centre cross, a white edge,
// and a marker that sweeps the bottom row every two seconds to make dropped or repeated frames visible.
constexpr const char* kTestCardGlobals = R"(
const vec3 kBars[8] = vec3[](
    vec3(0.75), vec3(0.75, 0.75, 0.0), vec3(0.0, 0.75, 0.75), vec3(0.0, 0.75, 0.0),
    vec3(0.75, 0.0, 0.75), vec3(0.75, 0.0, 0.0), vec3(0.0, 0.0, 0.75), vec3(0.0));
)";

constexpr const char* kTestCardSource = R"(
    // Integer pixel math: float division at exact cell edges rounds differently on Intel and Apple GPUs.
    uvec2 ipx = uvec2(px);
    px = vec2(ipx);
    vec2 size = h.size;
    uint cell = uint(size.y) / 9u;
    float line = max(1.0, floor(size.y / 540.0));
    uvec2 c = ipx / cell;
    vec3 col = vec3(0.08);

    if (float(ipx.x % cell) < line || float(ipx.y % cell) < line)
        col = vec3(0.5);
    if (c.y == 1u && c.x >= 2u && c.x < 14u)
        col = kBars[(ipx.x - 2u * cell) * 2u / (3u * cell)];
    if (c.y == 7u && c.x >= 2u && c.x < 14u)
        col = vec3(float(c.x - 2u) / 11.0);

    vec2 center = size * 0.5;
    if (abs(length(px + 0.5 - center) - 0.45 * size.y) < line)
        col = vec3(1.0);
    if (abs(px.x + 0.5 - center.x) < line || abs(px.y + 0.5 - center.y) < line)
        col = vec3(1.0);
    if (any(lessThan(px, vec2(2.0 * line))) || any(greaterThanEqual(px, size - 2.0 * line)))
        col = vec3(1.0);

    float markerX = fract(h.time * 0.5) * size.x;
    if (c.y == 8u && px.x >= markerX && px.x < markerX + cell)
        col = vec3(1.0);
    return vec4(col, 1.0);
)";

// `turns` carries the picture's quarter turns in bits 0-1 and its format (PictureFormat) above them: 0 is Y'CbCr
// planes, the others a HAP frame's compressed textures (scaled YCoCg after van Waveren and Castano).
constexpr const char* kYuvAt = R"(
vec4 hapAt(texture2D t0, texture2D t1, vec2 uv, uint format)
{
    vec4 c = texture(sampler2D(t0, lin), uv);
    if (format == 1u) return vec4(c.rgb, 1.0);
    if (format == 2u) return c;
    float scale = c.b * (255.0 / 8.0) + 1.0;
    float co = (c.r - 128.0 / 255.0) / scale;
    float cg = (c.g - 128.0 / 255.0) / scale;
    vec3 rgb = saturate(vec3(c.a + co - cg, c.a + cg, c.a - co - cg));
    return vec4(rgb, format == 4u ? texture(sampler2D(t1, lin), uv).r : 1.0);
}

vec4 yuvAt(texture2D t0, texture2D t1, vec2 px, Header h)
{
    vec2 uv = px / h.size;
    uint turns = h.turns & 3u;
    if (turns == 1u) uv = vec2(uv.y, 1.0 - uv.x);
    else if (turns == 2u) uv = 1.0 - uv;
    else if (turns == 3u) uv = vec2(1.0 - uv.y, uv.x);
    if (h.turns > 3u) return hapAt(t0, t1, uv, h.turns >> 2);
    vec3 yuv = vec3(texture(sampler2D(t0, lin), uv).r, texture(sampler2D(t1, lin), uv).rg);
    vec3 rgb = vec3(dot(h.yuv[0].xyz, yuv), dot(h.yuv[1].xyz, yuv), dot(h.yuv[2].xyz, yuv));
    rgb += vec3(h.yuv[0].w, h.yuv[1].w, h.yuv[2].w);
    return vec4(saturate(rgb), 1.0);
}
)";

constexpr const char* kYuvFadeSource = R"(
    vec4 c = yuvAt(t0, t1, px, h);
    if (m.weight >= 256u)
        return c;
    vec4 from = yuvAt(fadeY, fadeUv, px, hf);
    return m.weight == 0u ? from : mix(from, c, float(m.weight) / 256.0);
)";

// A text drawn smaller than it was rasterised reads the mip level of its scale, so its edges do not alias.
constexpr const char* kTextSource = R"(
    vec2 at = (px - h.yuv[2].xy) * h.yuv[2].z;
    vec2 coverage = textureLod(sampler2D(t0, textSampler), at / vec2(textureSize(t0, 0)),
                               max(0.0, log2(h.yuv[2].z))).rg;
    float fill = coverage.r * h.yuv[0].a;
    float line = coverage.g * h.yuv[1].a * (1.0 - fill);
    float a = fill + line;
    return a > 0.0 ? vec4((h.yuv[0].rgb * fill + h.yuv[1].rgb * line) / a, a) : vec4(0.0);
)";

constexpr const char* kGroupSource = R"(
    vec4 c = samplePixel(t0, px * vec2(textureSize(t0, 0)) / h.size);
    return c.a > 0.0 ? vec4(c.rgb / c.a, c.a) : vec4(0.0);
)";

// TransitionMix as the shaders read it, at buffer 2, and the outgoing clip's header at buffer 3.
const std::string kFadeMix =
    "\nstruct FadeMix { uint type; uint weight; uint edge; uint side; float params[" +
    std::to_string(kMaxTransitionParams) +
    "]; };\nlayout(set = 0, binding = 2, scalar) uniform FadeMixBlock { FadeMix m; };\n";
constexpr const char* kFromHeader = "layout(set = 0, binding = 3, scalar) uniform FromHeaderBlock { Header hf; };\n";

// The last pass of a chain has two entry points into the composition: passMain writes premultiplied colour for
// fixed-function blending, passBlend composites over a copy of the composition so far for the other blend modes
// (separable blend modes of the W3C Compositing spec, numbered as BlendMode).
constexpr const char* kIntoComposition = R"(
vec3 hardLight(vec3 b, vec3 s)
{
    return mix(b + (2.0 * s - 1.0) - b * (2.0 * s - 1.0), b * 2.0 * s, lessThanEqual(s, vec3(0.5)));
}

vec3 softLight(vec3 b, vec3 s)
{
    vec3 d = mix(sqrt(b), ((16.0 * b - 12.0) * b + 4.0) * b, lessThanEqual(b, vec3(0.25)));
    return mix(b + (2.0 * s - 1.0) * (d - b), b - (1.0 - 2.0 * s) * b * (1.0 - b), lessThanEqual(s, vec3(0.5)));
}

vec3 blendRgb(vec3 b, vec3 s, uint mode)
{
    switch (mode) {
    case 4u: return abs(b - s);
    case 5u: return max(b - s, vec3(0.0));
    case 6u: return b + s - 2.0 * b * s;
    case 7u: return max(b, s);
    case 8u: return min(b, s);
    case 9u: return hardLight(s, b);
    case 10u: return hardLight(b, s);
    case 11u: return softLight(b, s);
    case 12u:
        return mix(mix(b / max(1.0 - s, vec3(1e-6)), vec3(1.0), greaterThanEqual(s, vec3(1.0))), vec3(0.0),
                   lessThanEqual(b, vec3(0.0)));
    case 13u:
        return mix(mix(1.0 - (1.0 - b) / max(s, vec3(1e-6)), vec3(0.0), lessThanEqual(s, vec3(0.0))), vec3(1.0),
                   greaterThanEqual(b, vec3(1.0)));
    }
    return s;
}

vec4 composite(vec4 below, vec4 c, uint mode)
{
    vec3 b = below.a > 0.0 ? below.rgb / below.a : vec3(0.0);
    vec3 s = mix(c.rgb, saturate(blendRgb(b, c.rgb, mode)), below.a);
    return vec4(s * c.a + below.rgb * (1.0 - c.a), c.a + below.a * (1.0 - c.a));
}

vec4 belowHere() { return texelFetch(below, ivec2(gl_FragCoord.xy), 0); }
)";

// During a transition the incoming clip's last pass mixes in the outgoing picture itself, exactly as a separate mix
// pass over both pictures did: the incoming colour is rounded to 8 bits as that picture's target stored it.
constexpr const char* kFade = R"(
uint fadeRank(uvec2 p)
{
    uint x = p.x * 0x8da6b343u ^ p.y * 0xd8163841u;
    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    return x & 255u;
}

vec4 fadeQuantise(vec4 c)
{
    return floor(saturate(vec4(c.rgb * c.a, c.a)) * 255.0 + 0.5) / 255.0;
}

// For a wipe, dissolve or push, the one picture shown at p, 1 incoming or -1 outgoing, and where each is read; 0 when
// both are mixed. Each picture is then evaluated in one place, so its chain is inlined once.
int fadeSide(uvec2 p, uint width, out uvec2 inAt, out uvec2 outAt)
{
    inAt = p;
    outAt = p;
    switch (m.type) {
    case 1u: return p.x < m.edge ? 1 : -1;
    case 2u: return fadeRank(p) < m.weight ? 1 : -1;
    case 3u:
        inAt = uvec2(p.x - m.edge, p.y);
        outAt = uvec2(p.x + width - m.edge, p.y);
        return p.x < m.edge ? -1 : 1;
    }
    return 0;
}

vec4 fadeWeighed(vec4 o, vec4 i)
{
    uvec4 a = uvec4(o * 255.0 + 0.5);
    uvec4 b = uvec4(i * 255.0 + 0.5);
    return vec4((a * (256u - m.weight) + b * m.weight + 128u) >> 8) / 255.0;
}
)";

const char* sourceGlobals(SourceKind source)
{
    switch (source) {
    case SourceKind::TestCard: return kTestCardGlobals;
    case SourceKind::Yuv:
    case SourceKind::YuvFade: return kYuvAt;
    default: return "";
    }
}

std::string sourceBody(SourceKind source)
{
    switch (source) {
    case SourceKind::TestCard: return kTestCardSource;
    case SourceKind::Yuv: return "\n    return yuvAt(t0, t1, px, h);\n";
    case SourceKind::YuvFade: return kYuvFadeSource;
    case SourceKind::Texture: return "\n    return samplePixel(t0, px);\n";
    case SourceKind::Group: return kGroupSource;
    case SourceKind::Text: return kTextSource;
    case SourceKind::Generator: return "\n    return vec4(0.0);\n";
    }
    return "";
}

std::string texture(const std::string& name, size_t slot)
{
    return "layout(set = 1, binding = " + std::to_string(slot) + ") uniform texture2D " + name + ";\n";
}

std::string params(const char* name, size_t binding, size_t count)
{
    return "layout(set = 0, binding = " + std::to_string(binding) + ", scalar) uniform " + name + "Block { float " +
           name + "[" + std::to_string(std::max<size_t>(count, 1)) + "]; };\n";
}

size_t paramCount(const std::vector<PlannedStage>& stages)
{
    size_t count = 0;
    for (const PlannedStage& stage : stages)
        count += stage.effect->params.size();
    return count;
}

size_t valueCount(const std::vector<PlannedStage>& stages)
{
    size_t count = paramCount(stages);
    for (const PlannedStage& stage : stages)
        count += stage.effect->derived;
    return count;
}

std::string effectSignature(const EffectDef& effect, const std::string& suffix)
{
    const std::string name = "fx_" + effect.type + suffix;
    const std::string derived = effect.derived > 0 ? ", int q)" : ")";
    switch (effect.kind) {
    case StageKind::Pointwise:
        return "vec4 " + name + "(vec4 c, vec2 px, int p" + (effect.mask.empty() ? ")" : ", float m)");
    case StageKind::Remap: return "vec2 " + name + "(vec2 px, int p" + derived;
    case StageKind::Neighbourhood: return "vec4 " + name + "(texture2D t, vec2 px, int p" + derived;
    case StageKind::MultiPass: return "vec4 " + name + "(vec4 c, vec2 px, texture2D a, int p)";
    }
    return "";
}

std::string maskFunction(const EffectDef& effect, const std::string& suffix)
{
    if (effect.mask.empty())
        return "";
    return "\nfloat mask_" + effect.type + suffix + "(vec2 px, int p, int q)\n{" + effect.mask + "}\n";
}

// Each effect's file-scope GLSL, once.
std::string effectGlobals(const std::vector<PlannedStage>& stages)
{
    std::string out;
    std::set<const EffectDef*> written;
    for (const PlannedStage& stage : stages)
        if (written.insert(stage.effect).second)
            out += stage.effect->globals;
    return out;
}

std::string effectFunctions(const std::vector<PlannedStage>& stages, const std::string& suffix)
{
    std::string out;
    std::set<const EffectDef*> written;
    for (const PlannedStage& stage : stages)
        if (written.insert(stage.effect).second)
            out += maskFunction(*stage.effect, suffix) + "\n" + effectSignature(*stage.effect, suffix) + "\n{" +
                   stage.effect->glsl + "}\n";
    return out;
}

std::string at(size_t index)
{
    return "pb[" + std::to_string(index) + "]";
}

// A pointwise stage with a mask takes it first: where the last pass's header names it as blank and the mask is 0, the
// pixel gets no colour (the engine checked), so it is discarded before the stages under it are evaluated.
std::string maskedStage(const EffectDef& effect, size_t k, const std::string& prev, size_t offset,
                        const std::string& wet, size_t derivedAt, const std::string& suffix)
{
    return "    if (" + wet + " == 0.0) return " + prev + "(px);\n"
           "    float m = mask_" + effect.type + suffix + "(px, " + std::to_string(offset) + ", " +
           std::to_string(derivedAt) + ");\n"
           "    if (h.blank == " + std::to_string(k) + "u && m == 0.0) discard;\n"
           "    vec4 c = " + prev + "(px);\n"
           "    return mix(c, fx_" + effect.type + suffix + "(c, px, " + std::to_string(offset) + ", m), " + wet +
           ");\n}\n";
}

// Stage k wraps stage k-1 and mixes its result with its input by the effect's dry/wet, its last parameter. At dry/wet
// 0 (or bypassed) it returns its input without evaluating the effect, which is what the mix gives.
std::string stageFunction(const EffectDef& effect, size_t k, size_t offset, size_t aux, size_t derivedAt,
                          const std::string& suffix)
{
    const std::string prev = "s" + std::to_string(k - 1) + suffix;
    const std::string fx = "fx_" + effect.type + suffix;
    const std::string p = std::to_string(offset);
    const std::string q = effect.derived > 0 ? ", " + std::to_string(derivedAt) : std::string();
    // An effect registered without the dry/wet parameter, as a test's may be, is always wet.
    const std::string wet = effect.params.empty() ? "1.0" : at(offset + effect.params.size() - 1);
    const std::string dry = "    if (" + wet + " == 0.0) return c;\n";
    std::string out = "\nvec4 s" + std::to_string(k) + suffix + "(vec2 px)\n{\n";
    if (!effect.mask.empty())
        return out + maskedStage(effect, k, prev, offset, wet, derivedAt, suffix);
    switch (effect.kind) {
    case StageKind::Pointwise:
        out += "    vec4 c = " + prev + "(px);\n" + dry + "    return mix(c, " + fx + "(c, px, " + p + "), " + wet +
               ");\n";
        break;
    case StageKind::Remap:
        // Outside the input is transparent, with a one-pixel soft edge instead of a hard cut.
        out += "    if (" + wet + " == 0.0) return " + prev + "(px);\n"
               "    vec2 q = " + fx + "(px, " + p + q + ");\n"
               "    vec2 d = min(q, h.size - q);\n"
               "    vec4 o = " + prev + "(clamp(q, vec2(0.0), h.size - 0.001));\n"
               "    o.a *= saturate(min(d.x, d.y) + 0.5);\n"
               "    if (" + wet + " >= 1.0) return o;\n"
               "    return mix(" + prev + "(px), o, " + wet + ");\n";
        break;
    case StageKind::Neighbourhood:
        out += "    vec4 c = " + prev + "(px);\n" + dry + "    return mix(c, " + fx + "(t0, px, " + p + q + "), " +
               wet + ");\n";
        break;
    case StageKind::MultiPass:
        out += "    vec4 c = " + prev + "(px);\n" + dry + "    vec4 o = " + fx + "(c, px, a" + std::to_string(aux) +
               ", " + p + ");\n"
               "    if (" + wet + " >= 1.0) return o;\n"
               "    return mix(c, o, " + wet + ");\n";
        break;
    }
    return out + "}\n";
}

// The stage functions s1..sN over `stages`, the first of the pass's stages, whose parameters fill `total` values before
// the derived ones; returns the parameter offset after them.
size_t stageFunctions(const std::vector<PlannedStage>& stages, std::string& out, size_t total,
                      const std::string& suffix)
{
    size_t offset = 0;
    size_t aux = 0;
    size_t derivedAt = total;
    for (size_t k = 1; k <= stages.size(); ++k) {
        const EffectDef& effect = *stages[k - 1].effect;
        out += stageFunction(effect, k, offset, aux, derivedAt, suffix);
        aux += effect.kind == StageKind::MultiPass;
        offset += effect.params.size();
        derivedAt += effect.derived;
    }
    return offset;
}

// The source and stage functions, suffixed by `suffix`: s0 the source, sN the picture after N stages. They read the
// header `h`, the values `pb` and the source textures t0 and t1.
std::string chainFunctions(SourceKind source, const std::vector<PlannedStage>& stages, const std::string& suffix)
{
    std::string out = "\nvec4 s0" + suffix + "(vec2 px)\n{" + sourceBody(source) + "}\n" +
                      effectFunctions(stages, suffix);
    stageFunctions(stages, out, paramCount(stages), suffix);
    return out;
}

// The same functions over the outgoing clip of a single-pass fade, from its own textures, header and values.
std::string outgoingChainFunctions(SourceKind source, const std::vector<PlannedStage>& stages)
{
    return "\n#define t0 f0\n#define t1 f1\n#define h hf\n#define pb pbf\n" + chainFunctions(source, stages, "_out") +
           "#undef t0\n#undef t1\n#undef h\n#undef pb\n";
}

std::string isfTransitionFunctions(const IsfTransitions& transitions)
{
    std::string out;
    for (const IsfTransition& t : transitions)
        if (!t.function.empty())
            out += t.globals + "\nvec4 isfTransition" + std::to_string(uint32_t(t.mode)) +
                   "(vec4 from, vec4 to, uvec2 p, float progress)\n{" + t.function + "}\n";
    return out;
}

// A case of the transition switch per ISF transition, over the outgoing and incoming pictures at p.
std::string isfTransitionCases(const IsfTransitions& transitions)
{
    std::string out;
    for (const IsfTransition& t : transitions) {
        if (t.function.empty())
            continue;
        const std::string n = std::to_string(uint32_t(t.mode));
        out += "    case " + n + "u: return isfTransition" + n + "(o, i, p, float(m.weight) / 256.0);\n";
    }
    return out;
}

// fadeMixed mixes the incoming picture at q, the pass's last stage, with the outgoing one as the transition type says
// (TransitionMix): read from its picture, or for the single-pass fade (fadeMixedSingle) evaluated here.
std::string fadeFunctions(size_t stages, bool single, const IsfTransitions& transitions)
{
    const std::string last = "s" + std::to_string(stages);
    std::string out = "\nvec4 fadeIncoming(vec2 q) { return fadeQuantise(" + last + "(q)); }\n";
    const std::string mixed = R"(
    uvec2 inAt, outAt;
    int side = fadeSide(p, uint(h.size.x), inAt, outAt);
    vec4 o = side == 1 ? vec4(0.0) : OUTGOING(outAt);
    vec4 i = side == -1 ? vec4(0.0) : INCOMING(inAt);
    if (side != 0)
        return side == 1 ? i : o;
    switch (m.type) {
)" + isfTransitionCases(transitions) + R"(    }
    return fadeWeighed(o, i);
}
)";
    out += "\n#define OUTGOING(q) (m.side == 1u ? vec4(0.0) : texelFetch(from, ivec2(q), 0))\n"
           "#define INCOMING(q) (m.side == 2u ? vec4(0.0) : fadeIncoming(vec2(q) + 0.5))\n"
           "vec4 fadeMixed(uvec2 p)\n{" + mixed + "#undef OUTGOING\n#undef INCOMING\n";
    if (!single)
        return out;
    return out + "\nvec4 fadeOutgoing(vec2 q) { return fadeQuantise(" + last + "_out(q)); }\n"
                 "#define OUTGOING(q) fadeOutgoing(vec2(q) + 0.5)\n"
                 "#define INCOMING(q) fadeIncoming(vec2(q) + 0.5)\n"
                 "vec4 fadeMixedSingle(uvec2 p)\n{" + mixed + "#undef OUTGOING\n#undef INCOMING\n";
}

std::string entry(const std::string& name, const std::string& body)
{
    return "\n#ifdef ENTRY_" + name + "\nvoid main()\n{\n" + body + "}\n#endif\n";
}

// A last pass's two entry points for one way of working out its colour `c`: premultiplied for fixed-function blending
// (MAIN) and composited over the target's copy (BLEND). `faded` says `c` is premultiplied already.
std::string intoEntries(const std::string& main, const std::string& blend, const std::string& colour, bool faded)
{
    const std::string c = "    vec4 c = " + colour + ";\n";
    if (!faded)
        return entry(main, c + "    float a = c.a * h.opacity;\n    fragColour = vec4(c.rgb * a, a);\n") +
               entry(blend, c + "    fragColour = composite(belowHere(), vec4(c.rgb, c.a * h.opacity), h.blend);\n");
    return entry(main, c + "    fragColour = c * h.opacity;\n") +
           entry(blend, c + "    vec4 straight = c.a > 0.0 ? vec4(c.rgb / c.a, c.a * h.opacity) : vec4(0.0);\n"
                            "    fragColour = composite(belowHere(), straight, h.blend);\n");
}

// Header, values, textures and helpers every pass of this source with these stages declares.
std::string passHead(const PlannedPass& pass, size_t aux)
{
    std::string out = std::string(kVersion) + kExtensions + kGlslSamplers + kPrelude +
                      params("pb", 1, valueCount(pass.stages)) + texture("t0", 0) + texture("t1", 1);
    for (size_t j = 0; j < aux; ++j)
        out += texture("a" + std::to_string(j), kAuxSlot + j);
    if (pass.source == SourceKind::YuvFade)
        out += kFadeMix + kFromHeader + texture("fadeY", kFadeLumaSlot) + texture("fadeUv", kFadeChromaSlot);
    return out + sourceGlobals(pass.source) + effectGlobals(pass.stages);
}

std::string generateMain(const PlannedPass& pass, size_t aux)
{
    const std::string last = "s" + std::to_string(pass.stages.size()) + "(gl_FragCoord.xy)";
    std::string out = passHead(pass, aux);
    if (!pass.intoComposition)
        return out + chainFunctions(pass.source, pass.stages, "") +
               entry("passMain", "    vec4 c = " + last + ";\n    fragColour = vec4(c.rgb, c.a * h.opacity);\n");
    out += texture("below", kBelowSlot) + kIntoComposition;
    if (pass.source == SourceKind::YuvFade)
        return out + chainFunctions(pass.source, pass.stages, "") + intoEntries("passMain", "passBlend", last, false);

    const bool single = singlePassFade(pass);
    const auto transitions = isfTransitions();
    out += kFadeMix + texture("from", kFromSlot) + kFade + isfTransitionFunctions(*transitions) +
           chainFunctions(pass.source, pass.stages, "");
    if (single)
        out += kFromHeader + params("pbf", 4, valueCount(pass.stages)) + texture("f0", kFromLumaSlot) +
               texture("f1", kFromChromaSlot) + outgoingChainFunctions(pass.source, pass.stages);
    out += fadeFunctions(pass.stages.size(), single, *transitions) + intoEntries("passMain", "passBlend", last, false) +
           intoEntries("fadeMain", "fadeBlend", "fadeMixed(uvec2(gl_FragCoord.xy))", true);
    if (single)
        out += intoEntries("fadeSingleMain", "fadeSingleBlend", "fadeMixedSingle(uvec2(gl_FragCoord.xy))", true);
    return out;
}

// A side pass evaluates the stages before its multi-pass stage as INPUT, then that stage's side function.
std::string generateSide(const PlannedPass& pass, size_t aux)
{
    const std::vector<PlannedStage> prefix(pass.stages.begin(), pass.stages.end() - 1);
    const EffectDef& effect = *pass.stages.back().effect;
    const size_t side = *pass.side;
    const std::string fxs = "fxs_" + effect.type + "_" + std::to_string(side);

    std::string out = passHead(pass, aux) + texture("prev", kPrevSlot);
    for (size_t i = 0; i < side; ++i)
        out += texture("side" + std::to_string(i), kSideSlot + i);
    out += "\nvec4 s0(vec2 px)\n{" + sourceBody(pass.source) + "}\n" + effectFunctions(prefix, "");
    const size_t offset = stageFunctions(prefix, out, paramCount(pass.stages), "");
    out += "\n#define INPUT(q) s" + std::to_string(prefix.size()) + "(q)\n";
    out += "vec4 " + fxs + "(vec2 px, int p)\n{" + effect.sidePasses[side].glsl + "}\n";
    return out + entry("passMain", "    fragColour = " + fxs + "(gl_FragCoord.xy, " + std::to_string(offset) + ");\n");
}

bool needsFloat16(const PlannedPass& pass)
{
    if (pass.side)
        return pass.stages.back().effect->needsFloat16;
    if (pass.intoComposition)
        return false;
    return std::any_of(pass.stages.begin(), pass.stages.end(), [](const PlannedStage& stage) {
        return stage.effect->kind != StageKind::MultiPass && stage.effect->needsFloat16;
    });
}

size_t multiPassStages(const PlannedPass& pass)
{
    return size_t(std::count_if(pass.stages.begin(), pass.stages.end(),
                                [](const PlannedStage& s) { return s.effect->kind == StageKind::MultiPass; }));
}

// A neighbourhood stage reads several input pixels, so it needs a materialised input unless the pass so far is
// already a plain texture read. A multi-pass stage needs a free texture slot.
bool cutsBefore(const EffectDef& effect, const PlannedPass& pass)
{
    if (effect.kind == StageKind::MultiPass)
        return multiPassStages(pass) == kMaxAux;
    if (effect.kind != StageKind::Neighbourhood)
        return false;
    return !(pass.stages.empty() && pass.source == SourceKind::Texture);
}

void addSidePasses(const PlannedPass& pass, PlannedStage stage, std::vector<PlannedPass>& passes)
{
    for (size_t i = 0; i < stage.effect->sidePasses.size(); ++i) {
        PlannedPass side{.source = pass.source, .stages = pass.stages, .side = i,
                         .divisor = stage.effect->sidePasses[i].divisor};
        side.stages.push_back(stage);
        passes.push_back(std::move(side));
    }
}

} // namespace

bool singlePassFade(const PlannedPass& pass)
{
    return pass.intoComposition && pass.source == SourceKind::Yuv && combinedStages(pass).empty();
}

std::vector<std::string> passEntries(const PlannedPass& pass)
{
    if (!pass.intoComposition)
        return {"passMain"};
    std::vector<std::string> entries{"passMain", "passBlend"};
    if (pass.source != SourceKind::YuvFade)
        entries.insert(entries.end(), {"fadeMain", "fadeBlend"});
    if (singlePassFade(pass))
        entries.insert(entries.end(), {"fadeSingleMain", "fadeSingleBlend"});
    return entries;
}


std::vector<PlannedPass> planChain(SourceKind source, const std::vector<const EffectDef*>& chain)
{
    std::vector<PlannedPass> passes;
    PlannedPass pass{.source = source};
    for (size_t i = 0; i < chain.size(); ++i) {
        if (cutsBefore(*chain[i], pass)) {
            passes.push_back(std::move(pass));
            pass = PlannedPass{.source = SourceKind::Texture};
        }
        if (chain[i]->kind == StageKind::MultiPass)
            addSidePasses(pass, {chain[i], i}, passes);
        pass.stages.push_back({chain[i], i});
    }
    pass.intoComposition = true;
    passes.push_back(std::move(pass));

    for (PlannedPass& p : passes) {
        p.float16 = needsFloat16(p);
        const size_t aux = combinedStages(p).size();
        p.glsl = p.side ? generateSide(p, aux) : generateMain(p, aux);
    }
    return passes;
}

std::vector<size_t> combinedStages(const PlannedPass& pass)
{
    std::vector<size_t> indexes;
    const size_t end = pass.side ? pass.stages.size() - 1 : pass.stages.size();
    for (size_t i = 0; i < end; ++i)
        if (pass.stages[i].effect->kind == StageKind::MultiPass)
            indexes.push_back(i);
    return indexes;
}

} // namespace azl
