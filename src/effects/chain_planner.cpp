#include "effects/chain_planner.hpp"

#include "effects/isf.hpp"

#include <algorithm>
#include <set>

namespace azl {

namespace {

constexpr const char* kPrelude = R"(#include <metal_stdlib>
using namespace metal;

struct VOut { float4 position [[position]]; };

vertex VOut fullscreen(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    VOut o;
    o.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

struct Header {
    float4 yuv[3]; float2 size; float time; float opacity; uint blend; uint turns; float beat; uint blank; float dt;
};

constexpr sampler lin(filter::linear, address::clamp_to_edge);
constexpr sampler pix(coord::pixel, filter::linear, address::clamp_to_edge);

// 60 Hz frames since the previous rendered frame, in 1/256 steps so a steady 60 frames per second is exactly 1.
float frames60(constant Header& h) { return round(h.dt * 15360.0) / 256.0; }

// A factor per 60 Hz frame over this frame's elapsed time.
float perFrame(float x, constant Header& h) {
    float n = frames60(h);
    return n == 1.0 ? x : pow(x, n);
}

uint pcg(uint n) {
    n = n * 747796405u + 2891336453u;
    n = ((n >> ((n >> 28u) + 4u)) ^ n) * 277803737u;
    return (n >> 22u) ^ n;
}
)";

// Alignment test card: a square grid, colour bars, a grey ramp, a circle for aspect, a centre cross, a white edge,
// and a marker that sweeps the bottom row every two seconds to make dropped or repeated frames visible.
constexpr const char* kTestCardSource = R"(
constant float3 kBars[8] = {
    float3(0.75), float3(0.75, 0.75, 0.0), float3(0.0, 0.75, 0.75), float3(0.0, 0.75, 0.0),
    float3(0.75, 0.0, 0.75), float3(0.75, 0.0, 0.0), float3(0.0, 0.0, 0.75), float3(0.0)};

float4 s0(float2 px, ARGS) {
    // Integer pixel math: float division at exact cell edges rounds differently on Intel and Apple GPUs.
    uint2 ipx = uint2(px);
    px = float2(ipx);
    float2 size = h.size;
    uint cell = uint(size.y) / 9u;
    float line = max(1.0, floor(size.y / 540.0));
    uint2 c = ipx / cell;
    float3 col = float3(0.08);

    if (float(ipx.x % cell) < line || float(ipx.y % cell) < line)
        col = float3(0.5);
    if (c.y == 1u && c.x >= 2u && c.x < 14u)
        col = kBars[(ipx.x - 2u * cell) * 2u / (3u * cell)];
    if (c.y == 7u && c.x >= 2u && c.x < 14u)
        col = float3(float(c.x - 2u) / 11.0);

    float2 center = size * 0.5;
    if (abs(length(px + 0.5 - center) - 0.45 * size.y) < line)
        col = float3(1.0);
    if (abs(px.x + 0.5 - center.x) < line || abs(px.y + 0.5 - center.y) < line)
        col = float3(1.0);
    if (any(px < 2.0 * line) || any(px >= size - 2.0 * line))
        col = float3(1.0);

    float markerX = fract(h.time * 0.5) * size.x;
    if (c.y == 8u && px.x >= markerX && px.x < markerX + cell)
        col = float3(1.0);
    return float4(col, 1.0);
}
)";

// `turns` carries the picture's quarter turns in bits 0-1 and its format (PictureFormat) above them: 0 is Y'CbCr
// planes, the others a HAP frame's compressed textures (scaled YCoCg after van Waveren and Castano).
constexpr const char* kYuvAt = R"(
float4 hapAt(texture2d<float> t0, texture2d<float> t1, float2 uv, uint format) {
    float4 c = t0.sample(lin, uv);
    if (format == 1u) return float4(c.rgb, 1.0);
    if (format == 2u) return c;
    float scale = c.b * (255.0 / 8.0) + 1.0;
    float co = (c.r - 128.0 / 255.0) / scale;
    float cg = (c.g - 128.0 / 255.0) / scale;
    float3 rgb = saturate(float3(c.a + co - cg, c.a + cg, c.a - co - cg));
    return float4(rgb, format == 4u ? t1.sample(lin, uv).r : 1.0);
}

float4 yuvAt(texture2d<float> t0, texture2d<float> t1, float2 px, constant Header& h) {
    float2 uv = px / h.size;
    uint turns = h.turns & 3u;
    if (turns == 1u) uv = float2(uv.y, 1.0 - uv.x);
    else if (turns == 2u) uv = 1.0 - uv;
    else if (turns == 3u) uv = float2(1.0 - uv.y, uv.x);
    if (h.turns > 3u) return hapAt(t0, t1, uv, h.turns >> 2);
    float3 yuv = float3(t0.sample(lin, uv).r, t1.sample(lin, uv).rg);
    float3 rgb = float3(dot(h.yuv[0].xyz, yuv), dot(h.yuv[1].xyz, yuv), dot(h.yuv[2].xyz, yuv));
    rgb += float3(h.yuv[0].w, h.yuv[1].w, h.yuv[2].w);
    return float4(saturate(rgb), 1.0);
}
)";

constexpr const char* kYuvSource = R"(
float4 s0(float2 px, ARGS) { return yuvAt(t0, t1, px, h); }
)";

constexpr const char* kYuvFadeSource = R"(
float4 s0(float2 px, ARGS) {
    float4 c = yuvAt(t0, t1, px, h);
    if (fadeMix.weight >= 256u)
        return c;
    float4 from = yuvAt(fadeY, fadeUv, px, fadeHeader);
    return fadeMix.weight == 0u ? from : mix(from, c, float(fadeMix.weight) / 256.0);
}
)";

// TransitionMix as the shaders read it.
const std::string kFadeMix = "\nstruct FadeMix { uint type; uint weight; uint edge; uint side; float params[" +
                             std::to_string(kMaxTransitionParams) + "]; };\n";

constexpr const char* kTextureSource = R"(
float4 s0(float2 px, ARGS) { return t0.sample(pix, px); }
)";

// A text drawn smaller than it was rasterised reads the mip level of its scale, so its edges do not alias.
constexpr const char* kTextSource = R"(
constexpr sampler textSampler(filter::linear, mip_filter::linear, address::clamp_to_zero);

float4 s0(float2 px, ARGS) {
    float2 at = (px - h.yuv[2].xy) * h.yuv[2].z;
    float2 coverage = t0.sample(textSampler, at / float2(t0.get_width(), t0.get_height()),
                                level(max(0.0, log2(h.yuv[2].z)))).rg;
    float fill = coverage.r * h.yuv[0].a;
    float line = coverage.g * h.yuv[1].a * (1.0 - fill);
    float a = fill + line;
    return a > 0.0 ? float4((h.yuv[0].rgb * fill + h.yuv[1].rgb * line) / a, a) : float4(0.0);
}
)";

constexpr const char* kGroupSource = R"(
float4 s0(float2 px, ARGS) {
    float4 c = t0.sample(pix, px * float2(t0.get_width(), t0.get_height()) / h.size);
    return c.a > 0.0 ? float4(c.rgb / c.a, c.a) : float4(0.0);
}
)";

// The last pass of a chain has two entry points into the composition: passMain writes premultiplied colour for
// fixed-function blending, passBlend composites over a copy of the composition so far for the other blend modes
// (separable blend modes of the W3C Compositing spec, numbered as BlendMode).
constexpr const char* kIntoComposition = R"(
float3 hardLight(float3 b, float3 s) {
    return select(b + (2.0 * s - 1.0) - b * (2.0 * s - 1.0), b * 2.0 * s, s <= 0.5);
}

float3 softLight(float3 b, float3 s) {
    float3 d = select(sqrt(b), ((16.0 * b - 12.0) * b + 4.0) * b, b <= 0.25);
    return select(b + (2.0 * s - 1.0) * (d - b), b - (1.0 - 2.0 * s) * b * (1.0 - b), s <= 0.5);
}

float3 blendRgb(float3 b, float3 s, uint mode) {
    switch (mode) {
    case 4: return abs(b - s);
    case 5: return max(b - s, float3(0.0));
    case 6: return b + s - 2.0 * b * s;
    case 7: return max(b, s);
    case 8: return min(b, s);
    case 9: return hardLight(s, b);
    case 10: return hardLight(b, s);
    case 11: return softLight(b, s);
    case 12: return select(select(b / max(1.0 - s, float3(1e-6)), float3(1.0), s >= 1.0), float3(0.0), b <= 0.0);
    case 13: return select(select(1.0 - (1.0 - b) / max(s, float3(1e-6)), float3(0.0), s <= 0.0), float3(1.0),
                           b >= 1.0);
    }
    return s;
}

float4 composite(float4 below, float4 c, uint mode) {
    float3 b = below.a > 0.0 ? below.rgb / below.a : float3(0.0);
    float3 s = mix(c.rgb, saturate(blendRgb(b, c.rgb, mode)), below.a);
    return float4(s * c.a + below.rgb * (1.0 - c.a), c.a + below.a * (1.0 - c.a));
}
)";

// Integer pixel math throughout, so a transition frame is the same on Intel and Apple GPUs: textures are read at whole
// pixels and the crossfade weighs 8-bit values in 1/256 steps.
std::string sourceFunction(SourceKind source)
{
    switch (source) {
    case SourceKind::TestCard: return kTestCardSource;
    case SourceKind::Yuv: return std::string(kYuvAt) + kYuvSource;
    case SourceKind::YuvFade: return std::string(kYuvAt) + kYuvFadeSource;
    case SourceKind::Texture: return kTextureSource;
    case SourceKind::Group: return kGroupSource;
    case SourceKind::Text: return kTextSource;
    case SourceKind::Generator: return "\nfloat4 s0(float2 px, ARGS) { return float4(0.0); }\n";
    }
    return "";
}


std::string textureList(const std::string& name, size_t count, const char* separator)
{
    std::string out;
    for (size_t i = 0; i < count; ++i)
        out += "texture2d<float> " + name + std::to_string(i) + separator;
    return out;
}

std::string nameList(const std::string& name, size_t count)
{
    std::string out;
    for (size_t i = 0; i < count; ++i)
        out += name + std::to_string(i) + ", ";
    return out;
}

// ARGS and PASS thread the pass's textures, header and parameters through every stage function, and for a YuvFade
// source the outgoing clip's planes, header and the mix, named so that no stage's locals take their names.
std::string argsMacros(size_t aux, SourceKind source)
{
    const bool fade = source == SourceKind::YuvFade;
    return (fade ? kFadeMix : std::string()) + std::string("\n#define ARGS texture2d<float> t0, texture2d<float> t1, ") +
           textureList("a", aux, ", ") + "constant Header& h, constant float* pb" +
           (fade ? ", texture2d<float> fadeY, texture2d<float> fadeUv, constant Header& fadeHeader, constant FadeMix& fadeMix"
                 : "") +
           "\n#define PASS t0, t1, " + nameList("a", aux) + "h, pb" + (fade ? ", fadeY, fadeUv, fadeHeader, fadeMix" : "") +
           "\n";
}

std::string effectSignature(const EffectDef& effect)
{
    const std::string name = "fx_" + effect.type;
    switch (effect.kind) {
    case StageKind::Pointwise:
        return "float4 " + name + "(float4 c, float2 px, constant Header& h, constant float* p" +
               (effect.mask.empty() ? ")" : ", float m)");
    case StageKind::Remap:
        return "float2 " + name + "(float2 px, constant Header& h, constant float* p" +
               (effect.derived > 0 ? ", constant float* q)" : ")");
    case StageKind::Neighbourhood:
        return "float4 " + name + "(texture2d<float> t, float2 px, constant Header& h, constant float* p" +
               (effect.derived > 0 ? ", constant float* q)" : ")");
    case StageKind::MultiPass:
        return "float4 " + name + "(float4 c, float2 px, constant Header& h, texture2d<float> a, constant float* p)";
    }
    return "";
}

std::string maskFunction(const EffectDef& effect)
{
    if (effect.mask.empty())
        return "";
    return "\nfloat mask_" + effect.type + "(float2 px, constant Header& h, constant float* p, constant float* q) {" +
           effect.mask + "}\n";
}

std::string effectFunctions(const std::vector<PlannedStage>& stages)
{
    std::string out;
    std::set<const EffectDef*> written;
    for (const PlannedStage& stage : stages)
        if (written.insert(stage.effect).second)
            out += stage.effect->globals + maskFunction(*stage.effect) + "\n" + effectSignature(*stage.effect) + " {" +
                   stage.effect->metal + "}\n";
    return out;
}

// A pointwise stage with a mask takes it first: where the last pass's header names it as blank and the mask is 0, the
// pixel gets no colour (the engine checked), so it is discarded before the stages under it are evaluated.
std::string maskedStage(const EffectDef& effect, size_t k, const std::string& prev, const std::string& params,
                        const std::string& wet, size_t derivedAt)
{
    return "    if (" + wet + " == 0.0) return " + prev + "(px, PASS);\n"
           "    float m = mask_" + effect.type + "(px, h, " + params + ", pb + " + std::to_string(derivedAt) + ");\n"
           "    if (h.blank == " + std::to_string(k) + "u && m == 0.0) discard_fragment();\n"
           "    float4 c = " + prev + "(px, PASS);\n"
           "    return mix(c, fx_" + effect.type + "(c, px, h, " + params + ", m), " + wet + ");\n}\n";
}

// Stage k wraps stage k-1 and mixes its result with its input by the effect's dry/wet, its last parameter. At dry/wet
// 0 (or bypassed) it returns its input without evaluating the effect, which is what the mix gives.
std::string stageFunction(const EffectDef& effect, size_t k, size_t offset, size_t aux, size_t derivedAt)
{
    const std::string prev = "s" + std::to_string(k - 1);
    const std::string fx = "fx_" + effect.type;
    const std::string params = "pb + " + std::to_string(offset);
    const std::string wet = "pb[" + std::to_string(offset + effect.params.size() - 1) + "]";
    const std::string dry = "    if (" + wet + " == 0.0) return c;\n";
    std::string out = "\nfloat4 s" + std::to_string(k) + "(float2 px, ARGS) {\n";
    if (!effect.mask.empty())
        return out + maskedStage(effect, k, prev, params, wet, derivedAt);
    switch (effect.kind) {
    case StageKind::Pointwise:
        out += "    float4 c = " + prev + "(px, PASS);\n" + dry +
               "    return mix(c, " + fx + "(c, px, h, " + params + "), " + wet + ");\n";
        break;
    case StageKind::Remap:
        // Outside the input is transparent, with a one-pixel soft edge instead of a hard cut.
        out += "    if (" + wet + " == 0.0) return " + prev + "(px, PASS);\n"
               "    float2 q = " + fx + "(px, h, " + params +
               (effect.derived > 0 ? ", pb + " + std::to_string(derivedAt) : std::string()) + ");\n"
               "    float2 d = min(q, h.size - q);\n"
               "    float4 o = " + prev + "(clamp(q, float2(0.0), h.size - 0.001), PASS);\n"
               "    o.a *= saturate(min(d.x, d.y) + 0.5);\n"
               "    if (" + wet + " >= 1.0) return o;\n"
               "    return mix(" + prev + "(px, PASS), o, " + wet + ");\n";
        break;
    case StageKind::Neighbourhood:
        out += "    float4 c = " + prev + "(px, PASS);\n" + dry + "    return mix(c, " + fx + "(t0, px, h, " + params +
               (effect.derived > 0 ? ", pb + " + std::to_string(derivedAt) : std::string()) + "), " + wet + ");\n";
        break;
    case StageKind::MultiPass:
        out += "    float4 c = " + prev + "(px, PASS);\n" + dry +
               "    float4 o = " + fx + "(c, px, h, a" + std::to_string(aux) + ", " + params + ");\n"
               "    if (" + wet + " >= 1.0) return o;\n"
               "    return mix(c, o, " + wet + ");\n";
        break;
    }
    return out + "}\n";
}

// The stage functions s1..sN over `stages`, the first of the pass's stages, whose parameters fill `total` values before
// the derived ones; returns the parameter offset after them.
size_t stageFunctions(const std::vector<PlannedStage>& stages, std::string& out, size_t total)
{
    size_t offset = 0;
    size_t aux = 0;
    size_t derivedAt = total;
    for (size_t k = 1; k <= stages.size(); ++k) {
        const EffectDef& effect = *stages[k - 1].effect;
        out += stageFunction(effect, k, offset, aux, derivedAt);
        aux += effect.kind == StageKind::MultiPass;
        offset += effect.params.size();
        derivedAt += effect.derived;
    }
    return offset;
}

size_t paramCount(const std::vector<PlannedStage>& stages)
{
    size_t count = 0;
    for (const PlannedStage& stage : stages)
        count += stage.effect->params.size();
    return count;
}

std::string fragmentHead(const char* name, size_t aux, const std::string& extraTextures,
                         SourceKind source = SourceKind::Yuv)
{
    std::string out = "\nfragment float4 " + std::string(name) +
                      "(VOut in [[stage_in]], texture2d<float> t0 [[texture(0)]], texture2d<float> t1 [[texture(1)]],\n";
    for (size_t j = 0; j < aux; ++j)
        out += "    texture2d<float> a" + std::to_string(j) + " [[texture(" + std::to_string(kAuxSlot + j) + ")]],\n";
    if (source != SourceKind::YuvFade)
        return out + extraTextures + "    constant Header& h [[buffer(0)]], constant float* pb [[buffer(1)]]) {\n";
    return out + extraTextures + "    texture2d<float> fadeY [[texture(" + std::to_string(kFadeLumaSlot) +
           ")]], texture2d<float> fadeUv [[texture(" + std::to_string(kFadeChromaSlot) + ")]],\n" +
           "    constant Header& h [[buffer(0)]], constant float* pb [[buffer(1)]],\n"
           "    constant FadeMix& fadeMix [[buffer(2)]], constant Header& fadeHeader [[buffer(3)]]) {\n";
}

// During a transition the incoming clip's last pass mixes in the outgoing picture itself, exactly as a separate mix
// pass over both pictures did: the incoming colour is rounded to 8 bits as that picture's target stored it.
constexpr const char* kFade = R"(
uint fadeRank(uint2 p) {
    uint x = p.x * 0x8da6b343u ^ p.y * 0xd8163841u;
    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    return x & 255u;
}

float4 fadeQuantise(float4 c) {
    return floor(saturate(float4(c.rgb * c.a, c.a)) * 255.0 + 0.5) / 255.0;
}

// For a wipe, dissolve or push, the one picture shown at p, 1 incoming or -1 outgoing, and where each is read; 0 when
// both are mixed. Each picture is then evaluated in one place, so its chain is inlined once.
int fadeSide(uint2 p, uint width, constant FadeMix& m, thread uint2& inAt, thread uint2& outAt) {
    inAt = p;
    outAt = p;
    switch (m.type) {
    case 1: return p.x < m.edge ? 1 : -1;
    case 2: return fadeRank(p) < m.weight ? 1 : -1;
    case 3:
        inAt = uint2(p.x - m.edge, p.y);
        outAt = uint2(p.x + width - m.edge, p.y);
        return p.x < m.edge ? -1 : 1;
    }
    return 0;
}
)";

std::string isfTransitionFunctions(const IsfTransitions& transitions)
{
    std::string out;
    for (const IsfTransition& t : transitions)
        if (!t.function.empty())
            out += t.globals + "\nfloat4 isfTransition" + std::to_string(uint32_t(t.mode)) +
                   "(float4 from, float4 to, uint2 p, constant Header& h, float progress, constant float* tp) {" +
                   t.function + "}\n";
    return out;
}

// A case of the transition switch per ISF transition, over the outgoing and incoming pictures at p.
std::string isfTransitionCases(const IsfTransitions& transitions, const std::string& from, const std::string& to)
{
    std::string out;
    for (const IsfTransition& t : transitions) {
        if (t.function.empty())
            continue;
        const std::string n = std::to_string(uint32_t(t.mode));
        out += "    case " + n + ": return isfTransition" + n + "(" + from + ", " + to +
               ", p, h, float(m.weight) / 256.0, m.params);\n";
    }
    return out;
}

// fadeIncoming is the incoming clip's last pass at q; fadeMixed mixes it with the outgoing picture as the transition
// type says (TransitionMix).
std::string fadeFunction(size_t stages, const IsfTransitions& transitions)
{
    return "\nfloat4 fadeIncoming(float2 q, ARGS) { return fadeQuantise(s" + std::to_string(stages) + "(q, PASS)); }\n" +
           isfTransitionFunctions(transitions) + R"(
#define FADE_IN(q) (m.side == 2u ? float4(0.0) : fadeIncoming(float2(q) + 0.5, PASS))
#define FADE_OUT(q) (m.side == 1u ? float4(0.0) : from.read(q))
float4 fadeMixed(uint2 p, texture2d<float> from, constant FadeMix& m, ARGS) {
    uint2 inAt, outAt;
    int side = fadeSide(p, uint(h.size.x), m, inAt, outAt);
    float4 o = side == 1 ? float4(0.0) : FADE_OUT(outAt);
    float4 i = side == -1 ? float4(0.0) : FADE_IN(inAt);
    if (side != 0)
        return side == 1 ? i : o;
    switch (m.type) {
)" + isfTransitionCases(transitions, "o", "i") + R"(    }
    uint4 a = uint4(o * 255.0 + 0.5);
    uint4 b = uint4(i * 255.0 + 0.5);
    return float4((a * (256u - m.weight) + b * m.weight + 128u) >> 8) / 255.0;
}
)";
}

// A last pass's entry point during a transition: the pass's own arguments, the outgoing picture and the mix.
std::string fadeHead(const char* name, size_t aux, const std::string& below)
{
    const std::string from = "    texture2d<float> from [[texture(" + std::to_string(kFromSlot) + ")]],\n";
    const std::string head = fragmentHead(name, aux, below + from);
    const std::string end = "constant float* pb [[buffer(1)]]) {\n";
    return head.substr(0, head.rfind(end)) + "constant float* pb [[buffer(1)]], constant FadeMix& m [[buffer(2)]]) {\n";
}

// The single-pass fade: the outgoing clip through the same pass from its own textures, header and parameters,
// rounded to 8 bits as the outgoing picture used to store it, then mixed as fadeMixed does.
std::string fadeSingleFunction(size_t stages, const IsfTransitions& transitions)
{
    return "\nfloat4 fadeOutgoing(float2 q, texture2d<float> f0, texture2d<float> f1, constant Header& hf, "
           "constant float* pbf) { return fadeQuantise(s" + std::to_string(stages) + "(q, f0, f1, hf, pbf)); }\n" +
           R"(
#define FROM(q) fadeOutgoing(float2(q) + 0.5, f0, f1, hf, pbf)
float4 fadeMixedSingle(uint2 p, texture2d<float> f0, texture2d<float> f1, constant Header& hf, constant float* pbf,
                       constant FadeMix& m, ARGS) {
    uint2 inAt, outAt;
    int side = fadeSide(p, uint(h.size.x), m, inAt, outAt);
    float4 o = side == 1 ? float4(0.0) : FROM(outAt);
    float4 i = side == -1 ? float4(0.0) : fadeIncoming(float2(inAt) + 0.5, PASS);
    if (side != 0)
        return side == 1 ? i : o;
    switch (m.type) {
)" + isfTransitionCases(transitions, "o", "i") + R"(    }
    uint4 a = uint4(o * 255.0 + 0.5);
    uint4 b = uint4(i * 255.0 + 0.5);
    return float4((a * (256u - m.weight) + b * m.weight + 128u) >> 8) / 255.0;
}
)";
}

std::string fadeSingleHead(const char* name, const std::string& below);

std::string fadeSingle(size_t stages, const std::string& below, const IsfTransitions& transitions)
{
    const std::string faded = "    float4 c = fadeMixedSingle(uint2(in.position.xy), f0, f1, hf, pbf, m, PASS);\n";
    return fadeSingleFunction(stages, transitions) + fadeSingleHead("fadeSingleMain", "") + faded +
           "    return c * h.opacity;\n}\n" + fadeSingleHead("fadeSingleBlend", below) + faded +
           "    float4 straight = c.a > 0.0 ? float4(c.rgb / c.a, c.a * h.opacity) : float4(0.0);\n"
           "    return composite(below.read(uint2(in.position.xy)), straight, h.blend);\n"
           "}\n";
}

std::string fadeSingleHead(const char* name, const std::string& below)
{
    const std::string from = "    texture2d<float> f0 [[texture(" + std::to_string(kFromLumaSlot) + ")]],\n" +
                             "    texture2d<float> f1 [[texture(" + std::to_string(kFromChromaSlot) + ")]],\n";
    const std::string head = fragmentHead(name, 0, below + from);
    const std::string end = "constant float* pb [[buffer(1)]]) {\n";
    return head.substr(0, head.rfind(end)) +
           "constant float* pb [[buffer(1)]], constant FadeMix& m [[buffer(2)]],\n"
           "    constant Header& hf [[buffer(3)]], constant float* pbf [[buffer(4)]]) {\n";
}

} // namespace

bool singlePassFade(const PlannedPass& pass)
{
    return pass.intoComposition && pass.source == SourceKind::Yuv && combinedStages(pass).empty();
}

namespace {

std::string generateMain(const PlannedPass& pass, size_t aux)
{
    std::string out =
        kPrelude + argsMacros(aux, pass.source) + sourceFunction(pass.source) + effectFunctions(pass.stages);
    stageFunctions(pass.stages, out, paramCount(pass.stages));
    const std::string last = "    float4 c = s" + std::to_string(pass.stages.size()) + "(in.position.xy, PASS);\n";
    if (!pass.intoComposition)
        return out + fragmentHead("passMain", aux, "", pass.source) + last +
               "    return float4(c.rgb, c.a * h.opacity);\n}\n";
    const std::string below = "    texture2d<float> below [[texture(" + std::to_string(kBelowSlot) + ")]],\n";
    const std::string into = kIntoComposition + fragmentHead("passMain", aux, "", pass.source) + last +
                             "    float a = c.a * h.opacity;\n"
                             "    return float4(c.rgb * a, a);\n"
                             "}\n" +
                             fragmentHead("passBlend", aux, below, pass.source) + last +
                             "    return composite(below.read(uint2(in.position.xy)), float4(c.rgb, c.a * h.opacity), "
                             "h.blend);\n"
                             "}\n";
    if (pass.source == SourceKind::YuvFade)
        return out + into;
    const std::string faded = "    float4 c = fadeMixed(uint2(in.position.xy), from, m, PASS);\n";
    const auto transitions = isfTransitions();
    return out + kFadeMix + kFade + fadeFunction(pass.stages.size(), *transitions) + into +
           fadeHead("fadeMain", aux, "") + faded + "    return c * h.opacity;\n}\n" +
           fadeHead("fadeBlend", aux, below) + faded +
           "    float4 straight = c.a > 0.0 ? float4(c.rgb / c.a, c.a * h.opacity) : float4(0.0);\n"
           "    return composite(below.read(uint2(in.position.xy)), straight, h.blend);\n"
           "}\n" +
           (singlePassFade(pass) ? fadeSingle(pass.stages.size(), below, *transitions) : std::string());
}

// A side pass evaluates the stages before its multi-pass stage as INPUT, then that stage's side function.
std::string generateSide(const PlannedPass& pass, size_t aux)
{
    const std::vector<PlannedStage> prefix(pass.stages.begin(), pass.stages.end() - 1);
    const EffectDef& effect = *pass.stages.back().effect;
    const size_t side = *pass.side;
    const std::string fxs = "fxs_" + effect.type + "_" + std::to_string(side);

    std::string out = kPrelude + argsMacros(aux, pass.source) + sourceFunction(pass.source) + effectFunctions(prefix);
    const size_t offset = stageFunctions(prefix, out, paramCount(pass.stages));
    out += "\n#define INPUT(q) s" + std::to_string(prefix.size()) + "(q, PASS)\n";
    out += "float4 " + fxs + "(float2 px, texture2d<float> prev, " + textureList("side", side, ", ") +
           "ARGS, constant float* p) {" + effect.sidePasses[side].metal + "}\n";

    std::string extra = "    texture2d<float> prev [[texture(" + std::to_string(kPrevSlot) + ")]],\n";
    for (size_t i = 0; i < side; ++i)
        extra += "    texture2d<float> side" + std::to_string(i) + " [[texture(" + std::to_string(kSideSlot + i) +
                 ")]],\n";
    out += fragmentHead("passMain", aux, extra, pass.source);
    out += "    return " + fxs + "(in.position.xy, prev, " + nameList("side", side) + "PASS, pb + " +
           std::to_string(offset) + ");\n}\n";
    return out;
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
        p.metal = p.side ? generateSide(p, aux) : generateMain(p, aux);
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
