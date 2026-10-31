#include "effects/effect_registry.hpp"

#include "effects/isf.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <shared_mutex>
#include <utility>
#include <vector>

namespace azl {

namespace {

ParamDef choice(ParamDef param)
{
    param.choice = true;
    return param;
}

ParamDef options(ParamDef param, std::vector<std::string> names)
{
    param.options = std::move(names);
    return choice(std::move(param));
}

const std::vector<std::string> kOffOn{"Off", "On"};

ParamDef in(const ParamUnit& unit, ParamDef param)
{
    param.unit = unit;
    return param;
}

ParamDef wrapping(const ParamUnit& unit, ParamDef param)
{
    param.wraps = true;
    return in(unit, std::move(param));
}

ParamDef percent(ParamDef param)
{
    return in(units::percent, std::move(param));
}

// Position is in composition widths and heights with +y up, rotation in degrees counter-clockwise.
EffectDef transform()
{
    return {"transform",
            StageKind::Remap,
            {percent({"positionX", -1.0, 1.0, 0.0}),
             percent({"positionY", -1.0, 1.0, 0.0}),
             in(units::times, {"scale", 0.0, 8.0, 1.0}),
             wrapping(units::degrees, {"rotation", -180.0, 180.0, 0.0})},
            false,
            R"(
    vec2 centre = h.size * 0.5;
    vec2 q = px - centre - vec2(pb[p + 0], -pb[p + 1]) * h.size;
    if (pb[p + 3] != 0.0) {
        float a = pb[p + 3] * (M_PI_F / 180.0);
        float s = sin(a), k = cos(a);
        q = vec2(k * q.x - s * q.y, s * q.x + k * q.y);
    }
    return q / max(pb[p + 2], 1e-4) + centre;
)",
            {}};
}

// Replaces colour by luminance (inverted on request) times a tint of the given hue, saturation and brightness;
// alpha is how much of the tinted picture replaces the original.
EffectDef colorize()
{
    return {"colorize",
            StageKind::Pointwise,
            {wrapping(units::turns, {"hue", 0.0, 1.0, 0.0}),
             percent({"saturation", 0.0, 1.0, 1.0}),
             percent({"brightness", 0.0, 1.0, 1.0}),
             percent({"invert", 0.0, 1.0, 0.0}),
             percent({"alpha", 0.0, 1.0, 1.0})},
            false,
            R"(
    if (pb[p + 4] == 0.0)
        return c;
    vec3 tint = saturate(abs(fract(pb[p + 0] + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
    tint = mix(vec3(1.0), tint, pb[p + 1]) * pb[p + 2];
    float y = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722));
    return vec4(mix(c.rgb, mix(y, 1.0 - y, pb[p + 3]) * tint, pb[p + 4]), c.a);
)",
            {}};
}

// Sobel gradient magnitude per colour channel, scaled by the multiplier.
EffectDef edgeDetection()
{
    return {"edgeDetection",
            StageKind::Neighbourhood,
            {in(units::times, {"multiplier", 0.0, 2.0, 0.25})},
            false,
            R"(
    if (pb[p + 0] == 0.0)
        return vec4(0.0, 0.0, 0.0, samplePixel(t, px).a);
    vec3 k[9];
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i)
            k[j * 3 + i] = samplePixel(t, px + vec2(i - 1, j - 1)).rgb;
    vec3 gx = k[2] + 2.0 * k[5] + k[8] - k[0] - 2.0 * k[3] - k[6];
    vec3 gy = k[6] + 2.0 * k[7] + k[8] - k[0] - 2.0 * k[1] - k[2];
    return vec4(saturate(sqrt(gx * gx + gy * gy) * pb[p + 0]), samplePixel(t, px).a);
)",
            {}};
}

// Shows the picture through parallel strips and fills the rest with a colour. `number` strips span the composition
// across `direction` (degrees, 0 = vertical strips) and move along it at `speed` strips per second. Each strip's width
// is `width` times a random share between minWidth and maxWidth (relative to maxWidth); `spread` varies it further and
// moves the strip within its slot. `add` mixes the result onto the picture with an Add blend (the picture plus
// brighter strips) instead of replacing it. It reads only its own pixel, so it fuses like any pointwise stage.
//
// The livelier features compile as variants, so the plain Stripper costs what it always did. "live": strips slide,
// swing back and forth within half a strip, or step one strip per period (`movement` 0, 1, 2); `evolve` morphs the
// random strips into the next set over each whole step, and `reshuffle` steps it on every that many beats of the beat
// clock, gliding over the first half of the period; `jitter` shakes each strip within the free space of its slot;
// the level of audio feature `audioBand` scales the strips' width by `audioWidth` and the effect's strength by
// `audioMix`. "cross" adds `directionSpread` and `speedSpread`: the strips fall into four random groups, each tilted by
// its own share of up to 90 degrees and moving at its own speed (from standing still to twice as fast at speedSpread
// 0.5, backwards above), so strips cross.
EffectDef stripper()
{
    const std::string strip = R"(
    vec2 d = vec2(cos(a), sin(a));
    float perPixel = pb[p + 0] / (abs(d.x) * h.size.x + abs(d.y) * h.size.y);
    float moved = movement == 1 ? 0.5 * sin(2.0 * M_PI_F * phase)
                : movement == 2 ? floor(phase) + smoothstep(0.0, 0.25, fract(phase)) : phase;
    float s = dot(px - h.size * 0.5, d) * perPixel - moved;
    float slot = floor(s);
    uint n = pcg(uint(int(slot) + seed));
    uint n1 = pcg(uint(int(slot) + seed + 7919));
    vec2 r = mix(vec2(float(n & 0xffffu), float(n >> 16u)), vec2(float(n1 & 0xffffu), float(n1 >> 16u)), morph) *
                   (2.0 / 65535.0) - 1.0;
    float share = mix(pb[p + 11], pb[p + 12],
                      mix(float((n * 2654435761u) >> 16u), float((n1 * 2654435761u) >> 16u), morph) / 65535.0) /
                  max(pb[p + 12], 1e-4);
    n = morph < 0.5 ? n : n1;
    float width = saturate(pb[p + 4] * share * (1.0 + pb[p + 3] * r.y)) * mix(1.0, pb[p + 19], pb[p + 20]);
    float shift = r.x * pb[p + 3];
    if (pb[p + 18] > 0.0) {
        uint t = uint(tick);
        vec2 j = vec2(pcg(n ^ (t * 0x9e3779b9u)) & 0xffffu, pcg(n ^ ((t + 1u) * 0x9e3779b9u)) & 0xffffu);
        j = j * (2.0 / 65535.0) - 1.0;
        shift = clamp(shift + pb[p + 18] * mix(j.x, j.y, smoothstep(0.0, 1.0, fract(tick))), -1.0, 1.0);
    }
    float v = s - slot - 0.5 - shift * (1.0 - width) * 0.5;
    float e = max(pb[p + 5] * width * 0.5, 0.5 * perPixel);
    float hw = width * (0.5 + 2.0 * e) - e;
)";
    const std::string live = R"(
    int movement = int(pb[p + 16] + 0.5);
    float evolve = pb[p + 22] + max(floor(pb[p + 17]) - 1.0 + smoothstep(0.0, 0.5, fract(pb[p + 17])), 0.0);
    int seed = int(evolve) * 7919;
    float morph = smoothstep(0.0, 1.0, fract(evolve));
    float tick = h.time * 8.0;
)";
    const std::string end = R"(
    vec4 o = mix(vec4(pb[p + 7], pb[p + 8], pb[p + 9], pb[p + 10]), c, mix(m, 1.0 - m, pb[p + 6]));
    float sum = max(c.a, o.a);
    vec4 added = vec4((c.rgb * c.a + o.rgb * o.a) / max(sum, 1e-4), sum);
    return mix(c, mix(o, added, pb[p + 13]), mix(1.0, pb[p + 19], pb[p + 21]));
)";
    return {"stripper",
            StageKind::Pointwise,
            {in(units::strips, {"number", 1.0, 64.0, 8.0}),
             wrapping(units::degrees, {"direction", -180.0, 180.0, 0.0}),
             in(units::stripsPerSecond, {"speed", -4.0, 4.0, 0.0, ParamDrive::Phase}),
             percent({"spread", 0.0, 1.0, 0.0}),
             percent({"width", 0.0, 1.0, 0.5}),
             percent({"feather", 0.0, 1.0, 0.0}),
             percent({"invert", 0.0, 1.0, 0.0}),
             percent({"red", 0.0, 1.0, 0.0}),
             percent({"green", 0.0, 1.0, 0.0}),
             percent({"blue", 0.0, 1.0, 0.0}),
             percent({"alpha", 0.0, 1.0, 1.0}),
             percent({"minWidth", 0.0, 1.0, 1.0}),
             percent({"maxWidth", 0.0, 1.0, 1.0}),
             percent({"add", 0.0, 1.0, 0.0}),
             in({"\xc2\xb0", 90.0, 0}, {"directionSpread", 0.0, 1.0, 0.0}),
             percent({"speedSpread", 0.0, 1.0, 0.0, ParamDrive::Phase, "speed"}),
             in(units::whole, options({"movement", 0.0, 2.0, 0.0}, {"Slide", "Swing", "Step"})),
             in(units::beats, {"reshuffle", 0.0, 16.0, 0.0, ParamDrive::Beats}),
             percent({"jitter", 0.0, 1.0, 0.0}),
             in(units::whole, {"audioBand", 0.0, 9.0, 0.0, ParamDrive::Audio}),
             percent({"audioWidth", 0.0, 1.0, 0.0}),
             percent({"audioMix", 0.0, 1.0, 0.0}),
             {"evolve", 0.0, 4.0, 0.0}},
            false,
            R"(
    float a = pb[p + 1] * (M_PI_F / 180.0);
    vec2 d = vec2(cos(a), sin(a));
    float perPixel = pb[p + 0] / (abs(d.x) * h.size.x + abs(d.y) * h.size.y);
    float s = dot(px - h.size * 0.5, d) * perPixel - pb[p + 2];
    float slot = floor(s);
    uint n = pcg(uint(int(slot)));
    vec2 r = vec2(float(n & 0xffffu), float(n >> 16u)) * (2.0 / 65535.0) - 1.0;
    float share = mix(pb[p + 11], pb[p + 12], float((n * 2654435761u) >> 16u) / 65535.0) / max(pb[p + 12], 1e-4);
    float width = saturate(pb[p + 4] * share * (1.0 + pb[p + 3] * r.y));
    float v = s - slot - 0.5 - r.x * pb[p + 3] * (1.0 - width) * 0.5;
    float e = max(pb[p + 5] * width * 0.5, 0.5 * perPixel);
    float hw = width * (0.5 + 2.0 * e) - e;
    float m = 1.0 - smoothstep(hw - e, hw + e, abs(v));
    vec4 o = mix(vec4(pb[p + 7], pb[p + 8], pb[p + 9], pb[p + 10]), c, mix(m, 1.0 - m, pb[p + 6]));
    float sum = max(c.a, o.a);
    vec4 added = vec4((c.rgb * c.a + o.rgb * o.a) / max(sum, 1e-4), sum);
    return mix(o, added, pb[p + 13]);
)",
            {},
            {{"cross",
              {"directionSpread", "speedSpread"},
              live + R"(
    const float turn[4] = float[](0.0, -0.5, 1.0, -1.0);
    const float pace[4] = float[](0.0, 1.0, -1.0, -0.5);
    float m = 0.0;
    for (int g = 0; g < 4; ++g) {
    float a = (pb[p + 1] + pb[p + 14] * turn[g] * 90.0) * (M_PI_F / 180.0);
    float phase = pb[p + 2] + 2.0 * pace[g] * pb[p + 15];
)" + strip + R"(
    if (int((n * 0x9e3779b9u) >> 30u) == g)
        m = max(m, 1.0 - smoothstep(hw - e, hw + e, abs(v)));
    }
)" + end},
             {"live",
              {"movement", "reshuffle", "jitter", "audioWidth", "audioMix", "evolve"},
              live + R"(
    float a = pb[p + 1] * (M_PI_F / 180.0);
    float phase = pb[p + 2];
)" + strip + R"(
    float m = 1.0 - smoothstep(hw - e, hw + e, abs(v));
)" + end}}};
}

// Hides the picture in a grid of `columns` x `rows` cells that appear in a random order as `progress` goes from 0 to
// 1: a cell shows once its rank, hashed from the cell and the seed, is below progress, fading in over `softness`.
// `gap` hides a border of each cell, as a fraction of the cell's shorter side so it is as wide across as down whatever
// the cell's shape; hidden parts are transparent, or black with `black`. Each seed is its own order;
// `evolve` crossfades each cell from its visibility in one seed's order to the next over each whole step (morphing the
// rank instead would pop a cell whenever its rank crossed progress), and `reshuffle` times per second it steps on,
// gliding over the first half of the period.
EffectDef reveal()
{
    return {"reveal",
            StageKind::Pointwise,
            {percent({"progress", 0.0, 1.0, 0.5}),
             in({"", 1.0, 1}, {"columns", 1.0, 32.0, 8.0}),
             in({"", 1.0, 1}, {"rows", 1.0, 32.0, 8.0}),
             percent({"gap", 0.0, 0.4, 0.05}),
             in(units::whole, choice({"seed", 0.0, 100.0, 0.0})),
             in(units::hertz, {"reshuffle", 0.0, 8.0, 0.0, ParamDrive::Phase}),
             percent({"softness", 0.0, 0.5, 0.05}),
             percent({"black", 0.0, 1.0, 0.0}),
             {"evolve", 0.0, 4.0, 0.0}},
            false,
            R"(
    vec2 g = px / h.size * vec2(pb[p + 1], pb[p + 2]);
    vec2 cell = floor(g);
    vec2 f = g - cell;
    vec2 cellPx = h.size / vec2(pb[p + 1], pb[p + 2]);
    vec2 halfGap = pb[p + 3] * 0.5 * min(cellPx.x, cellPx.y) / cellPx;
    vec4 hidden = vec4(0.0, 0.0, 0.0, pb[p + 7]);
    if (any(lessThan(f, halfGap)) || any(greaterThan(f, 1.0 - halfGap)))
        return hidden;
    float evolve = pb[p + 8] + max(floor(pb[p + 5]) - 1.0 + smoothstep(0.0, 0.5, fract(pb[p + 5])), 0.0);
    float blend = smoothstep(0.0, 1.0, fract(evolve));
    float soft = max(pb[p + 6], 1e-5);
    float shown = 0.0;
    for (int i = 0; i < (blend > 0.0 ? 2 : 1); ++i) {
        float seed = pb[p + 4] + floor(evolve) + float(i);
        uint n = (uint(cell.x) * 73856093u) ^ (uint(cell.y) * 19349663u) ^ (floatBitsToUint(seed) * 83492791u);
        n = (n ^ 61u) ^ (n >> 16u);
        n *= 9u;
        n ^= n >> 4u;
        n *= 0x27d4eb2du;
        n ^= n >> 15u;
        float rank = float(n >> 8u) / 16777215.0;
        shown += smoothstep(rank - soft, rank + soft, pb[p + 0]) * (i == 0 ? 1.0 - blend : blend);
    }
    return mix(hidden, c, shown);
)",
            {}};
}

// Keeps the brighter of the picture and its fading history: history = max(input, last history * feedback per 60th of a
// second), with premultiplied alpha so a transparent pixel never brightens it. The history is 16F because at 8-bit a
// fade step below half a level rounds back and leaves permanent ghosts.
EffectDef trails()
{
    return {"trails",
            StageKind::MultiPass,
            {percent({"feedback", 0.0, 1.0, 0.9})},
            true,
            R"(
    vec4 o = samplePixel(a, px);
    return vec4(o.rgb / max(o.a, 1e-4), o.a);
)",
            {{1, true, R"(
    vec4 c = INPUT(px);
    if (pb[p + 0] == 0.0)
        return vec4(c.rgb * c.a, c.a);
    return max(vec4(c.rgb * c.a, c.a), samplePixel(prev, px) * perFrame(pb[p + 0], h));
)"}}};
}

// Shader functions the picture styles share: Slicer draws its cells with them, and each style is an effect of its own.
// Every effect that uses them carries them in its globals, and the guard keeps one copy in a pass.
constexpr const char* kStyleGlobals = R"(
#ifndef NOCTURNE_STYLES
#define NOCTURNE_STYLES
const uint kStyleGlyphs[26] = uint[](
    0xe8fe31u, 0x1e8fa3eu, 0xf8420fu, 0x1e8c63eu, 0x1f87a1fu, 0x1f87a10u, 0xf84e2fu, 0x118fe31u, 0x1f2109fu,
    0x710a4cu, 0x12a6292u, 0x108421fu, 0x11dd631u, 0x11cd671u, 0xe8c62eu, 0x1e8fa10u, 0xe8d64du, 0x1e8fa51u,
    0xf8383eu, 0x1f21084u, 0x118c62eu, 0x118c544u, 0x118d771u, 0x1151151u, 0x1151084u, 0x1f1111fu);

const vec3 kStyleThermal[6] = vec3[](vec3(0.0, 0.0, 0.1), vec3(0.15, 0.1, 0.9), vec3(0.85, 0.1, 0.75),
                                     vec3(1.0, 0.15, 0.1), vec3(1.0, 0.85, 0.1), vec3(1.0));

float styleLuma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

vec3 styleAccent(float hue) { return saturate(abs(fract(hue + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0); }

vec3 styleRamp(float y, vec3 accent)
{
    return y < 0.5 ? accent * (y * 2.0) : mix(accent, vec3(1.0), y * 2.0 - 1.0);
}

float styleSobel(texture2D t, vec2 px, float s)
{
    float k[9];
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i)
            k[j * 3 + i] = styleLuma(samplePixel(t, px + 0.5 + vec2(i - 1, j - 1) * s).rgb);
    float gx = k[2] + 2.0 * k[5] + k[8] - k[0] - 2.0 * k[3] - k[6];
    float gy = k[6] + 2.0 * k[7] + k[8] - k[0] - 2.0 * k[1] - k[2];
    return sqrt(gx * gx + gy * gy);
}

vec3 styleThermal(float y, vec3 accent, float colour)
{
    float x = saturate(y) * 5.0;
    int i = min(int(x), 4);
    return mix(styleRamp(y, accent), mix(kStyleThermal[i], kStyleThermal[i + 1], x - float(i)), colour);
}

vec3 styleGrain(vec2 px, float y, float time)
{
    uint n = pcg(uint(px.x) * 1973u + uint(px.y) * 9277u + uint(time * 24.0) * 26699u);
    return vec3(float(n >> 8u) / 16777215.0 < smoothstep(0.05, 0.6, y) ? 1.0 : 0.0);
}

vec3 styleLetters(texture2D t, vec2 px, float rows, float time, vec2 size)
{
    float g = max(6.0, floor(size.y / rows));
    vec2 cell = floor(px / g);
    float yc = styleLuma(samplePixel(t, (cell + 0.5) * g).rgb);
    ivec2 b = ivec2(floor((px - cell * g) / g * 7.0)) - 1;
    if (yc < 0.12 || any(lessThan(b, ivec2(0))) || any(greaterThan(b, ivec2(4))))
        return vec3(0.0);
    uint ch = pcg(uint(cell.x) * 0x8da6b343u ^ uint(cell.y) * 0xd8163841u ^ uint(time * 6.0) * 0xcb1ab31fu) % 26u;
    bool on = ((kStyleGlyphs[ch] >> uint(24 - b.y * 5 - b.x)) & 1u) != 0u;
    return on ? vec3(smoothstep(0.12, 0.5, yc)) : vec3(0.0);
}

vec3 styleInvert(vec4 c, float colour)
{
    return mix(vec3(1.0 - styleLuma(c.rgb)), 1.0 - c.rgb, colour);
}

vec3 styleDots(texture2D t, vec2 px, float rows, vec3 ink, vec2 size)
{
    float g = max(4.0, floor(size.y / rows));
    vec2 centre = (floor(px / g) + 0.5) * g;
    float r = sqrt(saturate(styleLuma(samplePixel(t, centre).rgb) * 1.5)) * 0.5 * g;
    return ink * saturate(r - length(px - centre) + 0.5);
}

vec3 styleEdges(texture2D t, vec2 px, vec3 accent, vec2 size)
{
    return mix(accent, vec3(1.0), 0.35) * saturate((styleSobel(t, px, max(1.0, size.y / 540.0)) - 0.1) * 3.0);
}

vec3 styleDuotone(float y, vec3 accent)
{
    return mix(accent * 0.3, vec3(1.0), smoothstep(0.2, 0.8, y));
}

vec3 styleOutline(texture2D t, vec2 px, vec3 accent, vec2 size)
{
    float r = max(3.0, size.y / 240.0);
    float lit = smoothstep(0.25, 0.35, styleLuma(samplePixel(t, px + 0.5).rgb));
    float hit = 0.0;
    for (int k = 0; k < 8; ++k) {
        float a = float(k) * (M_PI_F / 4.0);
        vec2 q = px + 0.5 + r * vec2(cos(a), sin(a));
        hit = max(hit, abs(smoothstep(0.25, 0.35, styleLuma(samplePixel(t, q).rgb)) - lit));
    }
    return mix(accent, vec3(1.0), 0.15) * hit;
}
#endif
)";

constexpr const char* kSlicerGlobals = R"(
float slicerRand(uint a, uint b) { return float(pcg(a * 0x9e3779b9u ^ pcg(b)) >> 8u) / 16777215.0; }

// The slot of u (0..1) among n slots of random widths, and its distance to the nearest cut between two slots.
vec2 slicerSlot(float u, int n, uint base, uint tick, float spread, float glide)
{
    float w[17];
    float total = 0.0;
    for (int i = 0; i < n; ++i) {
        float r = mix(slicerRand(base ^ pcg(tick), uint(i)), slicerRand(base ^ pcg(tick + 1u), uint(i)), glide);
        w[i] = 1.0 + spread * 1.8 * (r - 0.5);
        total += w[i];
    }
    float at = 0.0;
    for (int i = 0; i < n; ++i) {
        float end = at + w[i] / total;
        if (u < end || i == n - 1)
            return vec2(float(i), min(i > 0 ? u - at : 1e4, i < n - 1 ? end - u : 1e4));
        at = end;
    }
    return vec2(0.0, 1e4);
}

vec3 slicerStyle(int style, texture2D t, vec2 px, vec4 c, vec3 accent, float colour, float pick,
                   float time, vec2 size)
{
    float y = styleLuma(c.rgb);
    switch (style) {
    case 1: return styleThermal(y, accent, colour);
    case 2: return styleGrain(px, y, time);
    case 3: return styleLetters(t, px, 45.0, time, size);
    case 4: return styleInvert(c, colour);
    case 5: return styleDots(t, px, 70.0, pick < 0.5 ? vec3(1.0) : accent, size);
    case 6: return styleEdges(t, px, accent, size);
    case 7: return styleDuotone(y, accent);
    case 8: return styleOutline(t, px, accent, size);
    case 9: return pick < 0.5 ? vec3(1.0) : accent;
    default: return c.rgb;
    }
}
)";

uint32_t pcg(uint32_t n);
float fract(float x);

// What the CPU hands Slicer's shader (q): every cell's style, six 4-bit styles to a value, then each box or seed's
// centre (composition widths and heights) and half size (composition heights), then the two boxes each box links to.
constexpr int kSlicerCells = 288;
constexpr int kSlicerShapes = 32;
constexpr size_t kSlicerShapesAt = kSlicerCells / 6;
constexpr size_t kSlicerLinksAt = kSlicerShapesAt + 4 * kSlicerShapes;
constexpr size_t kSlicerDerived = kSlicerLinksAt + 2 * kSlicerShapes;
// The CPU does not know the composition's size; it lays cells out at 16:9 to find which ones touch.
constexpr float kSlicerAspect = 16.0f / 9.0f;

enum class SlicerLayout { Vertical, Horizontal, Grid, Radial, Boxes, Voronoi };

float slicerRand(uint32_t a, uint32_t b)
{
    return float(pcg(a * 0x9e3779b9u ^ pcg(b)) >> 8u) / 16777215.0f;
}

using SlicerCuts = std::array<float, 18>;

// The cuts between n slots of random widths, as the shader's slicerSlot places them: cuts[0] = 0 and cuts[n] = 1.
SlicerCuts slicerCuts(int n, uint32_t base, uint32_t tick, float spread, float glide)
{
    std::array<float, 17> w{};
    float total = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float a = slicerRand(base ^ pcg(tick), uint32_t(i)), b = slicerRand(base ^ pcg(tick + 1u), uint32_t(i));
        w[size_t(i)] = 1.0f + spread * 1.8f * (a + (b - a) * glide - 0.5f);
        total += w[size_t(i)];
    }
    SlicerCuts cuts{};
    for (int i = 0; i < n; ++i)
        cuts[size_t(i) + 1] = cuts[size_t(i)] + w[size_t(i)] / total;
    return cuts;
}

int slotAt(float u, const SlicerCuts& cuts, int n)
{
    for (int i = 0; i < n - 1; ++i)
        if (u < cuts[size_t(i) + 1])
            return i;
    return n - 1;
}

// One frame's cells, as the shader cuts them, to find out which touch.
struct SlicerCells {
    SlicerLayout layout;
    int n;
    SlicerCuts outer;
    std::array<SlicerCuts, 17> inner;
    std::array<float, 16> offsets;
    const float* shapes;

    // The cell at (x, y), composition widths and heights; -1 outside every box.
    int at(float x, float y) const
    {
        switch (layout) {
        case SlicerLayout::Vertical: return slotAt(x, outer, n + 1) * 16;
        case SlicerLayout::Horizontal: return slotAt(y, outer, n + 1);
        case SlicerLayout::Grid: {
            const int column = slotAt(x, outer, n + 1);
            return column * 16 + slotAt(y, inner[size_t(column)], n);
        }
        case SlicerLayout::Radial: {
            const float dx = (x - 0.5f) * kSlicerAspect, dy = y - 0.5f;
            const int ring = slotAt(std::sqrt(dx * dx + dy * dy) / std::sqrt(0.25f * kSlicerAspect * kSlicerAspect + 0.25f),
                                    outer, n);
            const float turn = fract(std::atan2(dy, dx) / 6.2831853f + 0.5f + offsets[size_t(ring)]);
            return ring * 18 + slotAt(turn, inner[size_t(ring)], n + 2);
        }
        case SlicerLayout::Boxes: {
            int cell = -1;
            for (int i = 0; i < n; ++i) {
                const float* b = shapes + 4 * i;
                if (std::abs(x - b[0]) * kSlicerAspect < b[2] && std::abs(y - b[1]) < b[3])
                    cell = i;
            }
            return cell;
        }
        case SlicerLayout::Voronoi: {
            int cell = 0;
            float best = 1e9f;
            for (int i = 0; i < n; ++i) {
                const float dx = (x - shapes[4 * i]) * kSlicerAspect, dy = y - shapes[4 * i + 1];
                if (dx * dx + dy * dy < best) {
                    best = dx * dx + dy * dy;
                    cell = i;
                }
            }
            return cell;
        }
        }
        return -1;
    }
};

// Where box or seed i is: a random place for the step, glided toward the next step's, then moved by `movement` at the
// phase of `speed`: sliding right to left across the picture, bouncing off its edges or drifting around its place.
void slicerShape(const float* p, int i, uint32_t seed, uint32_t tick, float glide, bool boxes, float* out)
{
    const uint32_t key = seed + uint32_t(i) * 0x9e3779b9u;
    auto random = [&](uint32_t k) {
        const float a = slicerRand(key, tick * 7u + k), b = slicerRand(key, (tick + 1u) * 7u + k);
        return a + (b - a) * glide;
    };
    const float r = slicerRand(key, 101u), r2 = slicerRand(key, 102u);
    const float tall = p[21] * (1.0f + p[4] * (random(3) - 0.5f));
    const float wide = tall * (1.0f + random(4));
    const float margin = boxes ? 0.1f : 0.0f;
    float x = margin + (1.0f - 2.0f * margin) * random(1);
    float y = margin + (1.0f - 2.0f * margin) * random(2);
    const float phase = p[20];
    auto bounce = [](float v) { return 1.0f - std::abs(fract(v * 0.5f) * 2.0f - 1.0f); };
    switch (int(p[19] + 0.5f)) {
    case 1: x = fract(x - phase * (0.6f + 0.8f * r)) * 1.4f - 0.2f; break;
    case 2:
        x = margin + (1.0f - 2.0f * margin) * bounce(x + phase * (0.5f + r));
        y = margin + (1.0f - 2.0f * margin) * bounce(y + phase * (0.3f + r2));
        break;
    case 3:
        x += 0.12f * std::sin((phase * (1.0f + r) + r2) * 6.2831853f);
        y += 0.12f * std::cos((phase * (0.8f + r2) + r) * 6.2831853f);
        break;
    default: break;
    }
    out[0] = x;
    out[1] = y;
    out[2] = boxes ? wide * 0.5f : 0.0f;
    out[3] = boxes ? tall * 0.5f : 0.0f;
}

// Slicer's values from the CPU (kSlicerDerived of them): the boxes or seeds, each box's links to its two nearest, and
// every cell's style, picked among the styles switched on. Which cells touch comes from a coarse grid of samples; a cell
// takes its random style unless a touching cell before it holds that one, then the next style none of them holds, and
// its random style again when every one is held (few styles on, many neighbours).
// How many steps of `rate` the beat clock has counted: none while it is Static or Off.
float slicerClock(const float* p, float beat)
{
    const int rate = int(p[3] + 0.5f);
    if (int(p[2] + 0.5f) == 0 || rate == 0)
        return 0.0f;
    return std::max(beat, 0.0f) / std::exp2(float(rate - 3));
}

void slicerDerive(const float* p, const float*, float, float beat, float* out)
{
    const auto layout = SlicerLayout(std::clamp(int(p[1] + 0.5f), 0, 5));
    const bool shaped = layout == SlicerLayout::Boxes || layout == SlicerLayout::Voronoi;
    const int n = std::clamp(int(p[0]), 1, shaped ? kSlicerShapes : 16);
    const int animation = int(p[2] + 0.5f);
    const float clock = slicerClock(p, beat);
    const auto tick = uint32_t(std::floor(clock));
    const float step = fract(clock);
    const float glide = animation == 2 ? step * step * (3.0f - 2.0f * step) : 0.0f;
    const uint32_t seed = pcg(uint32_t(p[5]) + 1u);
    std::fill(out, out + kSlicerDerived, 0.0f);

    float* shapes = out + kSlicerShapesAt;
    float* links = out + kSlicerLinksAt;
    std::fill(links, links + 2 * kSlicerShapes, -1.0f);
    if (shaped)
        for (int i = 0; i < n; ++i)
            slicerShape(p, i, seed, tick, glide, layout == SlicerLayout::Boxes, shapes + 4 * i);
    if (layout == SlicerLayout::Boxes)
        for (int i = 0; i < n; ++i) {
            std::array<std::pair<float, int>, 2> nearest{{{1e9f, -1}, {1e9f, -1}}};
            for (int j = 0; j < n; ++j) {
                const float dx = (shapes[4 * j] - shapes[4 * i]) * kSlicerAspect, dy = shapes[4 * j + 1] - shapes[4 * i + 1];
                const std::pair<float, int> d{dx * dx + dy * dy, j};
                if (j == i)
                    continue;
                if (d < nearest[0]) {
                    nearest[1] = nearest[0];
                    nearest[0] = d;
                } else if (d < nearest[1])
                    nearest[1] = d;
            }
            links[2 * i] = float(nearest[0].second);
            links[2 * i + 1] = float(nearest[1].second);
        }

    SlicerCells cells{layout, n, {}, {}, {}, shapes};
    switch (layout) {
    case SlicerLayout::Vertical: cells.outer = slicerCuts(n + 1, seed, tick, p[4], glide); break;
    case SlicerLayout::Horizontal: cells.outer = slicerCuts(n + 1, seed ^ 0x2fu, tick, p[4], glide); break;
    case SlicerLayout::Grid:
        cells.outer = slicerCuts(n + 1, seed, tick, p[4], glide);
        for (int column = 0; column <= n; ++column)
            cells.inner[size_t(column)] = slicerCuts(n, seed ^ (uint32_t(column) * 0x632be5abu + 0x2fu), tick, p[4], glide);
        break;
    case SlicerLayout::Radial:
        cells.outer = slicerCuts(n, seed, tick, p[4], glide);
        for (int ring = 0; ring < n; ++ring) {
            const uint32_t r = uint32_t(ring);
            const float a = slicerRand(seed ^ r, tick), b = slicerRand(seed ^ r, tick + 1u);
            cells.offsets[size_t(ring)] = a + (b - a) * glide;
            cells.inner[size_t(ring)] = slicerCuts(n + 2, seed ^ (r * 0x632be5abu + 7u), tick, p[4], glide);
        }
        break;
    default: break;
    }

    constexpr int kWide = 96, kHigh = 54;
    thread_local std::vector<uint8_t> touching(size_t(kSlicerCells) * kSlicerCells);
    std::fill(touching.begin(), touching.end(), uint8_t(0));
    std::array<int, kWide> above{};
    for (int j = 0; j < kHigh; ++j) {
        int left = -1;
        for (int i = 0; i < kWide; ++i) {
            const int cell = cells.at((float(i) + 0.5f) / kWide, (float(j) + 0.5f) / kHigh);
            for (const int other : {left, j > 0 ? above[size_t(i)] : -1})
                if (cell >= 0 && other >= 0 && other != cell) {
                    touching[size_t(cell) * kSlicerCells + size_t(other)] = 1;
                    touching[size_t(other) * kSlicerCells + size_t(cell)] = 1;
                }
            left = above[size_t(i)] = cell;
        }
    }

    std::vector<int> on;
    for (int s = 0; s < 10; ++s)
        if (p[9 + s] > 0.5f)
            on.push_back(s);
    std::array<int, kSlicerCells> styles{};
    for (int cell = 0; cell < kSlicerCells && !on.empty(); ++cell) {
        const uint32_t key = seed + uint32_t(cell) * 0x9e3779b9u;
        const size_t first = std::min(size_t(slicerRand(key, tick * 3u + 2u) * float(on.size())), on.size() - 1);
        styles[size_t(cell)] = on[first];
        for (size_t k = 0; k < on.size(); ++k) {
            const int candidate = on[(first + k) % on.size()];
            bool taken = false;
            for (int other = 0; other < cell && !taken; ++other)
                taken = touching[size_t(cell) * kSlicerCells + size_t(other)] && styles[size_t(other)] == candidate;
            if (!taken) {
                styles[size_t(cell)] = candidate;
                break;
            }
        }
    }
    for (int cell = 0; cell < kSlicerCells; ++cell)
        out[cell / 6] += float(uint32_t(styles[size_t(cell)]) << (4 * (cell % 6)));
}

ParamDef style(std::string name, double on = 1.0)
{
    ParamDef param = options({std::move(name), 0.0, 1.0, on}, kOffOn);
    param.group = "Styles";
    return param;
}

const ParamUnit kRows{" rows", 1.0, 0};

// Cuts the picture into cells and draws each cell in a style of its own, or as it is. `count` cuts along `direction`:
// vertical or horizontal strips, a grid whose columns each split into rows of their own, rings cut by spokes, boxes
// over the picture or the Voronoi cells of as many seeds. `spread` varies the cell sizes. Each cell takes one of the
// styles switched on (None: the picture as it is), unlike the touching cells' styles while one is free.
// Every `rate` beats (Off: never) the cells reshuffle: Pulse cuts anew at once, Slide glides the cuts there and only
// steps the styles. Boxes and seeds also move by `movement` at `speed` cycles a second; boxes are `size` of the
// picture's height, framed whole or at their `corners`, and with `links` each joins its two nearest boxes with a line
// and a dot.
// Lines of `lines` whole pixels mark the cuts, hard-edged along straight cuts and smoothed around rings, spokes and
// links. The accent colour of `hue` tints the edges, dots, duotone and outline, and fills half the solid cells (the
// rest white); `colour` takes the thermal and inverted cells from the accent ramp and grey to full colour.
EffectDef slicer()
{
    const std::string shapes = std::to_string(kSlicerShapesAt), links = std::to_string(kSlicerLinksAt);
    EffectDef def{"slicer",
                  StageKind::Neighbourhood,
                  {in(units::whole, {"count", 1.0, 32.0, 3.0}),
                   in(units::whole, options({"direction", 0.0, 5.0, 2.0},
                                            {"Vertical", "Horizontal", "Grid", "Radial", "Boxes", "Voronoi"})),
                   in(units::whole, options({"animation", 0.0, 2.0, 1.0}, {"Static", "Pulse", "Slide"})),
                   in(units::whole, options({"rate", 0.0, 7.0, 3.0}, {"Off", "1/4", "1/2", "1", "2", "4", "8", "16"})),
                   percent({"spread", 0.0, 1.0, 0.6}),
                   in(units::whole, {"seed", 0.0, 99.0, 0.0}),
                   choice(in({" px", 1.0, 0}, {"lines", 0.0, 8.0, 2.0})),
                   wrapping(units::turns, {"hue", 0.0, 1.0, 0.7}),
                   percent({"colour", 0.0, 1.0, 1.0}),
                   style("none", 0.0),
                   style("thermal"),
                   style("grain"),
                   style("letters"),
                   style("invert"),
                   style("dots"),
                   style("edges"),
                   style("duotone"),
                   style("outline"),
                   style("solid"),
                   in(units::whole, options({"movement", 0.0, 3.0, 3.0}, {"Still", "Slide", "Bounce", "Drift"})),
                   in(units::perSecond, {"speed", -2.0, 2.0, 0.1, ParamDrive::Phase}),
                   percent({"size", 0.05, 0.6, 0.25}),
                   options({"corners", 0.0, 1.0, 0.0}, kOffOn),
                   options({"links", 0.0, 1.0, 1.0}, kOffOn)},
                  false,
                  R"(
    vec4 c = samplePixel(t, px);
    int direction = int(pb[p + 1] + 0.5);
    int animation = int(pb[p + 2] + 0.5);
    int rate = int(pb[p + 3] + 0.5);
    float clock = animation == 0 || rate == 0 ? 0.0 : max(h.beat, 0.0) / exp2(float(rate - 3));
    uint tick = uint(floor(clock));
    float glide = animation == 2 ? smoothstep(0.0, 1.0, fract(clock)) : 0.0;
    uint seed = pcg(uint(pb[p + 5]) + 1u);
    float width = floor(pb[p + 6] + 0.5);
    int cell = -1;
    float line = 0.0;
    if (direction >= 4) {
        int n = clamp(int(pb[p + 0]), 1, 32);
        int shape = q + )" + shapes + R"(;
        if (direction == 5) {
            float d1 = 1e9, d2 = 1e9;
            vec2 s1 = vec2(0.0), s2 = vec2(0.0);
            for (int i = 0; i < n; ++i) {
                vec2 s = vec2(pb[shape + 4 * i], pb[shape + 4 * i + 1]) * h.size;
                float d = distanceSquared(px, s);
                if (d < d1) {
                    d2 = d1;
                    s2 = s1;
                    d1 = d;
                    s1 = s;
                    cell = i;
                } else if (d < d2) {
                    d2 = d;
                    s2 = s;
                }
            }
            float edge = n > 1 ? (d2 - d1) / (2.0 * max(length(s2 - s1), 1e-3)) : 1e4;
            line = float(edge < width * 0.5);
        } else {
            for (int i = 0; i < n; ++i) {
                int b = shape + 4 * i;
                vec2 extent = vec2(pb[b + 2], pb[b + 3]) * h.size.y;
                vec2 inner = extent - abs(px - vec2(pb[b + 0], pb[b + 1]) * h.size);
                if (inner.x <= 0.0 || inner.y <= 0.0)
                    continue;
                cell = i;
                float corner = 0.25 * min(extent.x, extent.y);
                bool frame = pb[p + 22] > 0.5
                                 ? (inner.x < width && inner.y < corner) || (inner.y < width && inner.x < corner)
                                 : min(inner.x, inner.y) < width;
                line = float(frame);
            }
            if (pb[p + 23] > 0.5)
                for (int i = 0; i < n; ++i) {
                    vec2 a = vec2(pb[shape + 4 * i], pb[shape + 4 * i + 1]) * h.size;
                    line = max(line, saturate(width * 1.5 + 2.0 - length(px - a)));
                    for (int k = 0; k < 2; ++k) {
                        int j = int(pb[q + )" + links + R"( + 2 * i + k]);
                        if (j < 0)
                            continue;
                        vec2 e = vec2(pb[shape + 4 * j], pb[shape + 4 * j + 1]) * h.size - a, w = px - a;
                        vec2 x = w - e * saturate(dot(w, e) / max(dot(e, e), 1e-3));
                        line = max(line, saturate(width * 0.5 + 0.5 - length(x)));
                    }
                }
        }
    } else if (direction == 3) {
        int n = clamp(int(pb[p + 0]), 1, 16);
        vec2 d = px - h.size * 0.5;
        float rmax = length(h.size * 0.5);
        vec2 ring = slicerSlot(length(d) / rmax, n, seed, tick, pb[p + 4], glide);
        uint r = uint(ring.x);
        float offset = mix(slicerRand(seed ^ r, tick), slicerRand(seed ^ r, tick + 1u), glide);
        float turn = fract(atan(d.y, d.x) / (2.0 * M_PI_F) + 0.5 + offset);
        vec2 spoke = slicerSlot(turn, n + 2, seed ^ (r * 0x632be5abu + 7u), tick, pb[p + 4], glide);
        cell = int(r) * 18 + int(spoke.x);
        float edge = min(ring.y * rmax, min(spoke.y, min(turn, 1.0 - turn)) * 2.0 * M_PI_F * length(d));
        line = saturate(width * 0.5 + 0.5 - edge);
    } else {
        int n = clamp(int(pb[p + 0]), 1, 16);
        vec2 uv = px / h.size;
        vec2 x = direction != 1 ? slicerSlot(uv.x, n + 1, seed, tick, pb[p + 4], glide) : vec2(0.0, 1e4);
        vec2 y = direction != 0 ? slicerSlot(uv.y, direction == 1 ? n + 1 : n, seed ^ (uint(x.x) * 0x632be5abu + 0x2fu),
                                               tick, pb[p + 4], glide)
                                  : vec2(0.0, 1e4);
        cell = int(x.x) * 16 + int(y.x);
        line = float(min(x.y * h.size.x, y.y * h.size.y) < width * 0.5);
    }
    int style = cell < 0 ? 0 : int((uint(pb[q + cell / 6]) >> (4u * uint(cell % 6))) & 15u);
    uint key = seed + uint(max(cell, 0)) * 0x9e3779b9u;
    vec3 o = slicerStyle(style, t, px, c, styleAccent(pb[p + 7]), pb[p + 8], slicerRand(key, tick * 3u + 3u), h.time,
                         h.size);
    return vec4(mix(o, vec3(1.0), line), max(c.a, line));
)",
                  {}};
    def.globals = std::string(kStyleGlobals) + kSlicerGlobals;
    def.derive = slicerDerive;
    def.derived = kSlicerDerived;
    return def;
}

EffectDef styleEffect(std::string type, StageKind kind, std::vector<ParamDef> params, std::string glsl)
{
    EffectDef def{std::move(type), kind, std::move(params), false, std::move(glsl), {}};
    def.globals = kStyleGlobals;
    return def;
}

// Luminance in a thermal camera's false colours, from black through blue, magenta, red and yellow to white; `colour`
// below 1 takes it toward a ramp through the accent colour of `hue`.
EffectDef thermal()
{
    return styleEffect("thermal", StageKind::Pointwise,
                       {wrapping(units::turns, {"hue", 0.0, 1.0, 0.7}), percent({"colour", 0.0, 1.0, 1.0})}, R"(
    return vec4(styleThermal(styleLuma(c.rgb), styleAccent(pb[p + 0]), pb[p + 1]), c.a);
)");
}

// White specks on black, denser where the picture is brighter, drawn anew 24 times a second.
EffectDef grain()
{
    return styleEffect("grain", StageKind::Pointwise, {}, R"(
    return vec4(styleGrain(px, styleLuma(c.rgb), h.time), c.a);
)");
}

// Random letters on black in a grid of `rows` rows where the picture is lit, as bright as it is there, changing six
// times a second.
EffectDef letters()
{
    return styleEffect("letters", StageKind::Neighbourhood, {in(kRows, {"rows", 10.0, 120.0, 45.0})}, R"(
    return vec4(styleLetters(t, px, pb[p + 0], h.time, h.size), samplePixel(t, px).a);
)");
}

// A halftone: dots on black in a grid of `rows` rows, as large as the picture is bright there, white or, by `tint`, in
// the colour of `hue`.
EffectDef dots()
{
    return styleEffect("dots", StageKind::Neighbourhood,
                       {in(kRows, {"rows", 20.0, 200.0, 70.0}), wrapping(units::turns, {"hue", 0.0, 1.0, 0.7}),
                        percent({"tint", 0.0, 1.0, 0.0})},
                       R"(
    return vec4(styleDots(t, px, pb[p + 0], mix(vec3(1.0), styleAccent(pb[p + 1]), pb[p + 2]), h.size),
                samplePixel(t, px).a);
)");
}

// Shadows in a dark shade of the colour of `hue`, highlights white.
EffectDef duotone()
{
    return styleEffect("duotone", StageKind::Pointwise, {wrapping(units::turns, {"hue", 0.0, 1.0, 0.7})}, R"(
    return vec4(styleDuotone(styleLuma(c.rgb), styleAccent(pb[p + 0])), c.a);
)");
}

// A thick line in the colour of `hue` on black where the picture crosses from dark to lit.
EffectDef outline()
{
    return styleEffect("outline", StageKind::Neighbourhood, {wrapping(units::turns, {"hue", 0.0, 1.0, 0.7})}, R"(
    return vec4(styleOutline(t, px, styleAccent(pb[p + 0]), h.size), samplePixel(t, px).a);
)");
}

// Classic effects keep the reference's controls: its labels, order, defaults and stored value domain (0..1 where it
// saves values normalized), so its presets map by identity. docs/research/classic-controls.json lists them.
ParamDef classic(std::string name, std::string label, double defaultValue, ParamDrive drive = ParamDrive::Value,
                 std::string scaledBy = {})
{
    return {std::move(name), 0.0, 1.0, defaultValue, drive, std::move(scaledBy), std::move(label)};
}

// Options that start at these shares of the range rather than halfway between their values.
ParamDef startingAt(std::vector<double> starts, ParamDef param)
{
    param.optionStarts = std::move(starts);
    return param;
}

// Shown as `label` where the reference's label does not fit a row.
ParamDef shortened(std::string label, ParamDef param)
{
    param.fullLabel = std::exchange(param.label, std::move(label));
    return param;
}

// The wrapper's Add mixer crossfades additively: the picture fades out over the upper half of Opacity while the
// effect's result fades in over the lower half, both full at 0.5. The stage then mixes by Opacity, so this returns
// what that mix turns into the crossfade. Expects the effect's result in `o`.
std::string addMixer(int blendMode, int opacity)
{
    const std::string b = std::to_string(blendMode), m = std::to_string(opacity);
    return "    if (pb[p + " + b + "] < 0.5)\n        return o;\n    float w = max(pb[p + " + m + "], 1e-4);\n"
           "    vec3 faded = c.rgb * min(1.0, 2.0 - 2.0 * w) + o.rgb * min(1.0, 2.0 * w);\n"
           "    return vec4(c.rgb + (faded - c.rgb) / w, o.a);\n";
}

constexpr int kClassicStrips = 10;

// The hash the shaders' pcg() gives.
uint32_t pcg(uint32_t n)
{
    n = n * 747796405u + 2891336453u;
    n = ((n >> ((n >> 28u) + 4u)) ^ n) * 277803737u;
    return (n >> 22u) ^ n;
}

// A GLSL array of f(i) for each strip i.
std::string hashTable(const char* name, uint32_t (*f)(uint32_t))
{
    std::string out = "    const uint " + std::string(name) + "[" + std::to_string(kClassicStrips) + "] = uint[](";
    for (uint32_t i = 0; i < kClassicStrips; ++i)
        out += (i ? ", " : "") + std::to_string(f(i)) + "u";
    return out + ");\n";
}

uint32_t stripHash(uint32_t i)
{
    return pcg(i * 2654435761u + 1u);
}

uint32_t stripWidthHash(uint32_t i)
{
    return pcg(stripHash(i)) & 0xffffu;
}

// The Classic Stripper's derived values: the strip count, the span the strips wrap over (2, or 0 for none), the random
// draw of MovementType 2, then per strip its direction, its centre's offset across the picture and its half width.
constexpr size_t kStripHead = 3;
constexpr size_t kStripValues = 4;

// MovementType 2 (a random place) starts at 0.8, measured; below, the nearest of slide (0) and bounce (0.5).
int classicMovement(float value)
{
    return value >= 0.8f ? 2 : std::min(int(value * 2.0f + 0.5f), 1);
}

// The resampling Classic Stripper's strips on the GPU: `m` is how much of the strips a pixel is in. `perStrip` runs for
// every strip with its offset across it from its centre (s), its direction (d), its random number (n) and how much
// the pixel is in it (k). Each strip's hashes are constants and the four directions are taken before the loop. The
// half-pixel edge (whatever Feather says) is worked out only near the edge: clearly inside or outside, smoothstep
// clamps to what k takes there (outside with a margin, so its division cannot fall short of 1). The random draw and the
// wrap span come from the CPU (q).
std::string classicStrips(const std::string& perStrip)
{
    return std::string(R"(
    int mode = min(int(pb[p + 1] * 6.0 + 0.5), 6);
    int movement = pb[p + 4] >= 0.8 ? 2 : min(int(pb[p + 4] * 2.0 + 0.5), 1);
    int count = min(int(pb[p + 0] * 10.0 + 1e-4), 10);
    const float turn[4] = float[](0.0, -0.5, 1.0, -1.0);
    const float pace[4] = float[](0.0, 1.0, -1.0, -0.5);
)") + hashTable("stripHash", stripHash) + hashTable("stripWidth", stripWidthHash) + R"(
    vec2 uv = vec2(px.x / h.size.x - 0.5, 0.5 - px.y / h.size.y) * 2.0;
    uint draw = uint(pb[q + 2]);
    float wrap = pb[q + 1];
    float e = 0.5 / h.size.x;
    int turns = pb[p + 3] == 0.0 ? 1 : 4;
    vec2 dirs[4];
    for (int g = 0; g < turns; ++g) {
        float a = (pb[p + 2] + pb[p + 3] * turn[g] * 0.25) * 2.0 * M_PI_F;
        dirs[g] = vec2(cos(a), sin(a));
    }
    float m = 0.0;
    for (int i = 0; i < count; ++i) {
        uint n = stripHash[i];
        vec2 d = dirs[i & (turns - 1)];
        float centre = float(n & 0xffffu) / 32767.5 - 1.0 + (pb[p + 5] + 2.0 * pace[i & 3] * pb[p + 6]) * 17.5;
        if (movement == 1)
            centre = 1.0 - abs(fract(centre * 0.25 + 0.25) * 4.0 - 2.0);
        centre = movement == 2 ? float(pcg(n ^ (draw * 0x9e3779b9u)) & 0xffffu) / 65535.0 * 3.0 - 1.5
                               : fract(centre * 0.5 + 0.5) * 2.0 - 1.0;
        float s = dot(uv, d) - centre;
        s -= wrap * round(s * wrap * 0.25);
        float width = 6.0 * pb[p + 7] * mix(pb[p + 8], pb[p + 9], float(stripWidth[i]) / 65535.0);
        float off = abs(s);
        float k = off <= width * 0.5 - e ? 1.0
                : off >= width * 0.5 + 2.0 * e ? 0.0
                : 1.0 - smoothstep(width * 0.5 - e, width * 0.5 + e, off);
)" + perStrip + "        m = max(m, k);\n    }\n";
}

float fract(float x)
{
    return x - std::floor(x);
}

// The Classic Stripper's derived values, worked out once a frame with the shader's formulas in single precision. The
// CPU's cosine, sine and division round differently from the GPU's, which can move an antialiased strip edge pixel by
// a level. MovementType 2 puts every strip at a new random place over 1.5 picture widths (-1.5..1.5) every 200 x Speed
// of the reference's frames, which move strips at 60 frames a second as ours do: Speed x 10/3 s, at least a frame.
void classicStripperStrips(const float* p, const float* rates, float time, float, float* out)
{
    const float turn[4] = {0.0f, -0.5f, 1.0f, -1.0f};
    const float pace[4] = {0.0f, 1.0f, -1.0f, -0.5f};
    const int count = std::min(int(p[0] * 10.0f + 1e-4f), kClassicStrips);
    const int movement = classicMovement(p[4]);
    const float hold = std::max(std::abs(rates[5]) * 10.0f / 3.0f, 1.0f / 60.0f);
    const auto draw = uint32_t(std::max(time, 0.0f) / hold);
    out[0] = float(count);
    out[1] = movement == 2 ? 0.0f : 2.0f;
    out[2] = float(draw);
    for (int i = 0; i < count; ++i) {
        const uint32_t n = stripHash(uint32_t(i));
        const float a = (p[2] + p[3] * turn[i & 3] * 0.25f) * 2.0f * 3.14159265f;
        float centre = float(n & 0xffffu) / 32767.5f - 1.0f + (p[5] + 2.0f * pace[i & 3] * p[6]) * 17.5f;
        if (movement == 1)
            centre = 1.0f - std::abs(fract(centre * 0.25f + 0.25f) * 4.0f - 2.0f);
        const float share = float(stripWidthHash(uint32_t(i))) / 65535.0f;
        float* strip = out + kStripHead + size_t(i) * kStripValues;
        strip[0] = std::cos(a);
        strip[1] = std::sin(a);
        strip[2] = movement == 2 ? float(pcg(n ^ (draw * 0x9e3779b9u)) & 0xffffu) / 65535.0f * 3.0f - 1.5f
                                 : fract(centre * 0.5f + 0.5f) * 2.0f - 1.0f;
        strip[3] = 6.0f * p[7] * (p[8] + (p[9] - p[8]) * share) * 0.5f;
    }
}

// The plain Classic Stripper's mask from the strips the CPU worked out (q).
constexpr const char* kClassicStripMask = R"(
    int count = int(pb[q + 0]);
    float wrap = pb[q + 1];
    vec2 uv = vec2(px.x / h.size.x - 0.5, 0.5 - px.y / h.size.y) * 2.0;
    float e = 0.5 / h.size.x;
    float m = 0.0;
    for (int i = 0; i < count; ++i) {
        int strip = q + 3 + 4 * i;
        float s = dot(uv, vec2(pb[strip + 0], pb[strip + 1])) - pb[strip + 2];
        s -= wrap * round(s * wrap * 0.25);
        float off = abs(s);
        float k = off <= pb[strip + 3] - e ? 1.0
                : off >= pb[strip + 3] + 2.0 * e ? 0.0
                : 1.0 - smoothstep(pb[strip + 3] - e, pb[strip + 3] + e, off);
        m = max(m, k);
        if (m >= 1.0)
            break;
    }
    return m;
)";

// Transparent black comes out black unless the fill brings colour: mode 5 fills with the inverted picture (white), mode 6
// with the picture itself, the others with the fill colour. A mode on a rounding edge must hold either way.
bool classicStripperKeepsBlack(const float* p)
{
    for (const float edge : {-1e-4f, 1e-4f}) {
        const int mode = std::min(int(p[1] * 6.0f + 0.5f + edge), 6);
        if (mode == 5 || (mode != 6 && (p[11] != 0.0f || p[12] != 0.0f || p[13] != 0.0f)))
            return false;
    }
    return true;
}

// Outside the strips (m 0) modes 0 and 1 show the fill colour, as it is unless the Add mixer is on.
bool classicStripperBlackOutside(const float* p)
{
    for (const float edge : {-1e-4f, 1e-4f}) {
        const int mode = std::min(int(p[1] * 6.0f + 0.5f + edge), 6);
        if (mode > 1)
            return false;
    }
    return p[11] == 0.0f && p[12] == 0.0f && p[13] == 0.0f && p[15] < 0.5f;
}

// Measured against the reference: floor(number * 10) strips, each at its own random centre in [-1, 1] across the
// picture (-1..1 from edge to edge, turned anticlockwise by `direction` in whole turns, so turns follow the aspect) and
// 6 x globalWidth x a random share between minWidth and maxWidth wide, with hard edges (Feather shows no change); they
// slide at 17.5 x speed per second, wrapping over [-1, 1]. Mode is round(mode * 6): 0 and 1 show the picture inside
// the strips and the fill colour outside, 2 the other way round, 5 the picture colour-inverted inside and as it is
// outside, 6 the picture outside and inside its channels (green, blue, green) laid over it. 3 and 4 resample, so they
// compile as a neighbourhood variant: each strip adds the picture turned with it, its width across the strip and its
// height along it, magnified 3x across around the picture's centre (4: a random place per strip) and 2x along around
// the centre line, over the fill colour; where strips overlap they add up. MovementType slides, bounces between the
// edges, or from 0.8 (measured) holds each strip at a random place for a time that grows with Speed (see
// classicStripperStrips). With directionSpread or speedSpread every fourth strip tilts by its own share of up to a quarter
// turn and moves at its own pace, so strips cross.
EffectDef classicStripper()
{
    const std::string pointwise = R"(
    vec4 fill = mode == 5 ? vec4(1.0 - c.rgb, c.a)
              : mode == 6 ? c
                          : vec4(pb[p + 11], pb[p + 12], pb[p + 13], pb[p + 14]);
    vec4 inside = mode == 6 ? vec4((c.gbg + (1.0 - c.a) * c.rgb) / (2.0 - c.a), c.a * (2.0 - c.a)) : c;
    if (mode == 2 || mode == 5)
        m = 1.0 - m;
    vec4 o = mix(fill, inside, m);
)";
    EffectDef def{"classicStripper",
            StageKind::Pointwise,
            {choice(classic("number", "Number", 0.1)),
             options(classic("mode", "Mode", 0.0),
                     {"Strips", "Strips 2", "Inverted", "Zoom", "Zoom Scattered", "Negative", "Channel Swap"}),
             wrapping(units::plain, classic("direction", "Direction", 0.0)),
             shortened("Dir. Spread", classic("directionSpread", "Direction Spread", 0.0)),
             startingAt({0.25, 0.8}, options(shortened("Movement", classic("movementType", "MovementType", 0.0)),
                                             {"Slide", "Bounce", "Random"})),
             classic("speed", "Speed", 0.0, ParamDrive::Phase),
             classic("speedSpread", "SpeedSpread", 0.0, ParamDrive::Phase, "speed"),
             classic("globalWidth", "GlobalWidth", 1.0),
             classic("minWidth", "MinWidth", 0.1),
             classic("maxWidth", "MaxWidth", 0.4),
             classic("feather", "Feather", 0.0),
             classic("red", "Red", 0.0),
             classic("green", "Green", 0.0),
             classic("blue", "Blue", 0.0),
             classic("alpha", "Alpha", 1.0),
             options(classic("blendMode", "Blend Mode", 0.0), {"Alpha", "Add"})},
            false,
            "\n    int mode = min(int(pb[p + 1] * 6.0 + 0.5), 6);" + pointwise + addMixer(15, 16),
            {},
            {{"resample", {"mode"}, R"(
    vec4 c = samplePixel(t, px);
    vec4 sum = vec4(0.0);
)" + classicStrips(R"(
        if (k > 0.0) {
            float shift = mode == 4 ? float(pcg(n ^ 0x5bd1e995u) & 0xffffu) / 32767.5 - 1.0 : 0.0;
            vec2 at = vec2(s / 3.0 + shift, dot(uv, vec2(-d.y, d.x)) * 0.5);
            vec4 x = samplePixel(t, (at * vec2(0.5, -0.5) + 0.5) * h.size);
            sum += k * vec4(x.rgb * x.a, x.a);
        }
)") + pointwise + R"(
    if (mode == 3 || mode == 4) {
        sum = min(sum, 1.0);
        o.a = sum.a + fill.a * (1.0 - sum.a);
        o.rgb = (sum.rgb * sum.a + fill.rgb * fill.a * (1.0 - sum.a)) / max(o.a, 1e-4);
    }
)" + addMixer(15, 16),
              [](double mode) {
                  const int m = std::min(int(mode * 6.0 + 0.5), 6);
                  return m == 3 || m == 4;
              },
              StageKind::Neighbourhood}},
            "Classic"};
    def.keepsBlack = classicStripperKeepsBlack;
    def.mask = kClassicStripMask;
    def.derive = classicStripperStrips;
    def.derived = kStripHead + kClassicStrips * kStripValues;
    def.blackOutsideMask = classicStripperBlackOutside;
    return def;
}

// Replaces colour by a luminance times the colour: weighted (by measured weights) or the HSL lightness, scaled by
// 5 x contrast (neutral at the default 0.2), inverted on request. Mode 1 maps the luminance onto the default palette
// instead: black, red, green, yellow, 50 % grey, blue, magenta, cyan, white at equal steps. The colour's alpha is how
// much of the result replaces the picture. greysKeep keeps the colour of pixels near grey: a share
// exp(-(chroma / (0.475 x tolerance^1.5))^1.5), measured on a saturation ramp.
EffectDef classicColorize()
{
    EffectDef def{"classicColorize",
            StageKind::Pointwise,
            {classic("colorRed", "Color Red", 1.0),
             classic("colorGreen", "Color Green", 0.0),
             classic("colorBlue", "Color Blue", 0.0),
             classic("colorAlpha", "Color Alpha", 1.0),
             options(classic("mode", "Mode", 0.0), {"Color", "Palette"}),
             options(classic("weighted", "Weighted", 1.0), kOffOn),
             options(classic("greysKeep", "Keep", 0.0), kOffOn),
             classic("greysTolerance", "Tolerance", 0.3),
             options(classic("invert", "Invert", 0.0), kOffOn),
             classic("contrast", "Contrast", 0.2),
             options(classic("blendMode", "Blend Mode", 0.0), {"Alpha", "Add"})},
            false,
            R"(
    vec4 o = c;
    if (pb[p + 3] != 0.0) {
        float hi = max(c.r, max(c.g, c.b)), lo = min(c.r, min(c.g, c.b));
        float y = pb[p + 5] > 0.5 ? dot(c.rgb, vec3(0.3594, 0.4010, 0.2396)) : (hi + lo) * 0.5;
        y = saturate(y * 5.0 * pb[p + 9]);
        y = mix(y, 1.0 - y, pb[p + 8]);
        vec3 tint = y * vec3(pb[p + 0], pb[p + 1], pb[p + 2]);
        if (pb[p + 4] > 0.5) {
            const vec3 stops[9] = vec3[](vec3(0.0), vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(1.0, 1.0, 0.0),
                                     vec3(0.5), vec3(0.0, 0.0, 1.0), vec3(1.0, 0.0, 1.0), vec3(0.0, 1.0, 1.0),
                                     vec3(1.0));
            float s = y * 8.0;
            int i = min(int(s), 7);
            tint = mix(stops[i], stops[i + 1], s - float(i));
        }
        float keep = 0.0;
        if (pb[p + 6] > 0.5)
            keep = exp(-pow((hi - lo) / max(0.475 * pow(pb[p + 7], 1.5), 1e-5), 1.5));
        o = vec4(mix(mix(c.rgb, tint, pb[p + 3]), c.rgb, keep), c.a);
    }
)" + addMixer(10, 11),
            {},
            {},
            "Classic"};
    // Black has luminance 0, so its tint (or palette stop) is black unless inverted; greysKeep is left out, as its
    // exp(-pow(0, 1.5)) is only as exact as the GPU's pow.
    def.keepsBlack = [](const float* p) { return p[8] == 0.0f && p[6] <= 0.5f; };
    return def;
}

// A choice the reference saves as the option's index.
ParamDef classicChoice(std::string name, std::string label, double defaultValue, std::vector<std::string> names)
{
    ParamDef param = in(units::whole, options(classic(std::move(name), std::move(label), defaultValue), std::move(names)));
    param.max = double(param.options.size()) - 1.0;
    return param;
}

// The wrapper's mixers.
ParamDef classicBlend()
{
    return classicChoice("blendMode", "Blend Mode", 0.0, {"Alpha", "Add"});
}

ParamDef classicFlag(std::string name, std::string label, double defaultValue = 0.0)
{
    return classicChoice(std::move(name), std::move(label), defaultValue, kOffOn);
}

// Fills what the effect leaves transparent with black when blackBG (p[i]) is on. Expects the result in `o`.
std::string blackBackground(int i)
{
    return "    if (pb[p + " + std::to_string(i) + "] > 0.5)\n        o = vec4(o.rgb * o.a, 1.0);\n";
}

// Each colour channel is read from its own place: shifted by amount = R, G or B x distance in picture sizes (y down),
// tilted about the X or Y axis or turned by amount x 180 degrees (a 45 degree vertical field of view for the tilts),
// or zoomed by 1 - 0.95 x amount (1 - 8.5 x amount below 0). A channel read from outside the picture is 0, and the
// alpha is the largest of the three reads. A channel with the same amount as the one before reuses its read.
EffectDef classicShiftRgb()
{
    const ParamUnit fine{"", 1.0, 3};
    return {"classicShiftRgb",
            StageKind::Neighbourhood,
            {in(units::plain, classic("distance", "Distance", 1.0)),
             classicChoice("mode", "Mode", 0.0, {"Shift X", "Shift Y", "Rotate X", "Rotate Y", "Rotate Z", "Zoom"}),
             in(fine, {"r", -1.0, 1.0, 0.0, ParamDrive::Value, {}, "R"}),
             in(fine, {"g", -1.0, 1.0, 0.0, ParamDrive::Value, {}, "G"}),
             in(fine, {"b", -1.0, 1.0, 0.0, ParamDrive::Value, {}, "B"}),
             classicFlag("blackBG", "Black BG"),
             classicBlend()},
            false,
            R"(
    vec4 c = samplePixel(t, px);
    int mode = int(pb[p + 1] + 0.5);
    vec2 x = px - h.size * 0.5;
    float depth = h.size.y * 0.5 / tan(M_PI_F / 8.0);
    vec4 o = vec4(0.0);
    float last = 0.0;
    bool inside = false;
    vec4 v = vec4(0.0);
    for (int i = 0; i < 3; ++i) {
        float a = pb[p + 2 + i] * pb[p + 0];
        if (i == 0 || a != last) {
            last = a;
            float sn = sin(a * M_PI_F), cs = cos(a * M_PI_F);
            vec2 s = x;
            float facing = 1.0;
            if (mode == 0)
                s.x -= a * h.size.x;
            else if (mode == 1)
                s.y -= a * h.size.y;
            else if (mode == 2) {
                facing = depth * cs - x.y * sn;
                s.y = x.y * depth / facing;
                s.x = x.x * (depth + s.y * sn) / depth;
            } else if (mode == 3) {
                facing = depth * cs + x.x * sn;
                s.x = x.x * depth / facing;
                s.y = x.y * (depth - s.x * sn) / depth;
            } else if (mode == 4)
                s = vec2(cs * x.x - sn * x.y, sn * x.x + cs * x.y);
            else
                s = x * (a >= 0.0 ? 1.0 - 0.95 * a : 1.0 - 8.5 * a);
            vec2 q = s + h.size * 0.5;
            inside = facing > 0.0 && all(greaterThanEqual(q, vec2(0.0))) && all(lessThanEqual(q, h.size));
            if (inside)
                v = q == px ? c : samplePixel(t, q);
        }
        if (inside) {
            o[i] = v[i];
            o.a = max(o.a, v.a);
        }
    }
)" + blackBackground(5) + addMixer(6, 7),
            {},
            {},
            "Classic"};
}

// Folds the picture at x and y (in picture sizes; y measured from the bottom, flipX and flipY pick the side that is
// mirrored) and scrolls it so the fold reads inOut x of the way into the picture, repeating it beyond its edges.
// Black BG only shows on transparent input, which this stage keeps as it is; a remap mixes by Opacity, so the Add
// mixer matches only at full Opacity.
EffectDef classicMirror()
{
    return {"classicMirror",
            StageKind::Remap,
            {in(units::plain, classic("x", "X", 0.0)),
             in(units::plain, classic("y", "Y", 0.0)),
             in(units::plain, classic("inOut", "In / Out", 1.0)),
             classicFlag("flipX", "Flip X"),
             classicFlag("flipY", "Flip Y"),
             classicFlag("blackBG", "Black BG"),
             classicBlend()},
            false,
            R"(
    vec2 t = px / h.size;
    vec2 fold = vec2(pb[p + 0], pb[p + 1]);
    bvec2 mirrored = bvec2(pb[p + 3] > 0.5, pb[p + 4] < 0.5);
    vec2 open = pb[p + 2] * fold + abs(t - fold);
    vec2 folded = 1.0 - pb[p + 2] * (1.0 - fold) - abs(t - (1.0 - fold));
    vec2 s = fract(mix(open, folded, mirrored));
    return clamp(s * h.size, vec2(0.5), h.size - 0.5);
)",
            {},
            {},
            "Classic"};
}

// Repeats a wedge of the picture's centre round it: floor(angles x 12) mirrored pairs of wedges (none below 1/12),
// turned by rotation and reading the picture turned by inputRotation (whole turns), at zoom x the distance from the
// centre, all in picture UV with the size rounded up to a multiple of 16 (1080 reads as 1088, as measured: it turns the
// wedges' angles); reads beyond the picture take its edge.
EffectDef classicKaleidoscope()
{
    return {"classicKaleidoscope",
            StageKind::Remap,
            {in(units::plain, classic("angles", "Angles", 0.3)),
             wrapping(units::plain, classic("rotation", "Rotation", 0.0)),
             in(units::plain, classic("zoom", "Zoom", 0.5)),
             wrapping(units::plain, classic("inputRotation", "Input Rotation", 0.0)),
             classicFlag("blackBG", "Black BG"),
             classicBlend()},
            false,
            R"(
    float n = floor(pb[p + 0] * 12.0 + 1e-4);
    if (n < 1.0)
        return vec2(-4.0);
    vec2 span = ceil(h.size / 16.0) * 16.0;
    vec2 uv = (px - h.size * 0.5) / span * vec2(1.0, -1.0);
    float wedge = M_PI_F / n;
    float a = atan(uv.y, uv.x) - 2.0 * M_PI_F * pb[p + 1];
    a -= 2.0 * wedge * floor(a / (2.0 * wedge));
    a = (a < wedge ? a : 2.0 * wedge - a) + 2.0 * M_PI_F * pb[p + 3];
    vec2 s = length(uv) * pb[p + 2] * vec2(cos(a), -sin(a));
    return clamp(h.size * 0.5 + s * span, vec2(0.5), h.size - 0.5);
)",
            {},
            {},
            "Classic"};
}

// Each quadrant shows the whole picture at half size, mirrored towards the centre lines (the top-left as it is);
// flipX and flipY mirror all four in that axis.
EffectDef classicMirrorQuad()
{
    return {"classicMirrorQuad",
            StageKind::Remap,
            {classicFlag("flipX", "Flip X"),
             classicFlag("flipY", "Flip Y"),
             classicBlend()},
            false,
            R"(
    // In whole pixels, without a division: the fold lands exactly on texel edges, the same on every GPU.
    vec2 fold = abs(2.0 * px - h.size);
    vec2 s = mix(h.size - fold, fold, bvec2(pb[p + 0] > 0.5, pb[p + 1] > 0.5));
    return clamp(s, vec2(0.5), h.size - 0.5);
)",
            {},
            {},
            "Classic"};
}

// Wraps the picture round the centre: its width runs round each of `parts` sectors, counter-clockwise from straight
// down and turned back by angle (whole turns), and its height outwards from the centre, (rings + 1) / 2 times over
// half the picture's width, repeating beyond. Rings and Parts take whole values.
EffectDef classicPolarKaleido()
{
    auto count = [](std::string name, std::string label) {
        return in(units::whole, {std::move(name), 1.0, 16.0, 1.0, ParamDrive::Value, {}, std::move(label)});
    };
    return {"classicPolarKaleido",
            StageKind::Remap,
            {count("rings", "Rings"),
             count("parts", "Parts"),
             wrapping(units::plain, classic("angle", "Angle", 0.0)),
             classicBlend()},
            false,
            R"(
    vec2 d = (px - h.size * 0.5) * vec2(1.0, -1.0);
    float turn = atan(d.y, d.x) / (2.0 * M_PI_F) + 0.75;
    vec2 s = fract(vec2(round(pb[p + 1]) * turn - pb[p + 2],
                        length(d) / (h.size.x * 0.5) * (round(pb[p + 0]) + 1.0) * 0.5));
    return clamp(s * h.size, vec2(0.5), h.size - 0.5);
)",
            {},
            {},
            "Classic"};
}

// Each frame (at 60 per second) every kind of twitch fires on its own with a chance of 8 x frequency^4 and then takes
// a random amount up to its maximum, times globalTwitch: a move of up to maxPos picture sizes, a zoom between
// 1 / 2^maxScale and 2^maxScale, an opacity drop, a brightness gain of up to 1 + 4 x maxLight, red and blue pulled
// apart by up to 5 % of the width and a 3 x 3 blur spread up to 40 pixels.
EffectDef classicTwitch()
{
    auto twitch = [](std::string name, std::string label) {
        return in(units::plain, classic(std::move(name), std::move(label), 0.0));
    };
    EffectDef def{"classicTwitch",
            StageKind::Neighbourhood,
            {in(units::plain, classic("globalTwitch", "Global Twitch", 1.0)),
             twitch("maxPosX", "Max Pos X"),
             twitch("maxPosY", "Max Pos Y"),
             shortened("Pos Freq.", twitch("posFrequency", "Pos Frequency")),
             twitch("maxScale", "Max Scale"),
             shortened("Scale Freq.", twitch("scaleFrequency", "Scale Frequency")),
             twitch("maxOpacity", "Max Opacity"),
             shortened("Opacity Freq.", twitch("opacityFrequency", "Opacity Frequency")),
             twitch("maxLight", "Max Light"),
             shortened("Light Freq.", twitch("lightFrequency", "Light Frequency")),
             twitch("maxRGBShift", "Max RGB Shift"),
             shortened("RGB Freq.", twitch("rgbFrequency", "RGB Frequency")),
             twitch("maxBlur", "Max Blur"),
             shortened("Blur Freq.", twitch("blurFrequency", "Blur Frequency")),
             classicFlag("blackBG", "Black BG"),
             classicBlend()},
            false,
            R"(
    vec4 c = samplePixel(t, px);
    uint tick = uint(max(h.time, 0.0) * 60.0);
    float amount[6];
    for (uint k = 0; k < 6; ++k) {
        float f = pb[p + 3 + 2 * k];
        bool on = float(pcg(tick * 16u + k) & 0xffffu) / 65535.0 < min(1.0, 8.0 * f * f * f * f);
        amount[k] = on ? float(pcg(tick * 16u + k + 8u) & 0xffffu) / 65535.0 : -1.0;
    }
    float g = pb[p + 0];
    vec2 shift = amount[0] < 0.0 ? vec2(0.0)
                                 : (vec2(amount[0], float(pcg(tick * 16u + 7u) & 0xffffu) / 65535.0) * 2.0 - 1.0) *
                                       vec2(pb[p + 1], pb[p + 2]) * g;
    float zoom = amount[1] < 0.0 ? 1.0 : exp2((amount[1] * 2.0 - 1.0) * pb[p + 4] * g);
    vec2 q = h.size * 0.5 + (px - h.size * 0.5) / zoom - shift * h.size;
    float split = amount[4] < 0.0 ? 0.0 : (amount[4] * 2.0 - 1.0) * pb[p + 10] * g * 0.05 * h.size.x;
    float blur = amount[5] < 0.0 ? 0.0 : amount[5] * pb[p + 12] * g * 40.0;
    vec4 o = vec4(0.0);
    float taps = 0.0;
    int reach = blur > 0.0 ? 1 : 0;
    for (int j = -reach; j <= reach; ++j)
        for (int i = -reach; i <= reach; ++i) {
            vec2 at = q + vec2(i, j) * blur;
            vec4 m = inPicture(at, h.size) ? samplePixel(t, at) : vec4(0.0);
            if (split != 0.0) {
                vec2 ar = at + vec2(split, 0.0), ab = at - vec2(split, 0.0);
                vec4 r = inPicture(ar, h.size) ? samplePixel(t, ar) : vec4(0.0);
                vec4 b = inPicture(ab, h.size) ? samplePixel(t, ab) : vec4(0.0);
                m = vec4(r.r, m.g, b.b, max(m.a, max(r.a, b.a)));
            }
            o += m;
            taps += 1.0;
        }
    o /= taps;
    if (amount[3] >= 0.0)
        o.rgb *= 1.0 + 4.0 * amount[3] * pb[p + 8] * g;
    if (amount[2] >= 0.0)
        o.a *= 1.0 - pb[p + 6] * g * (0.3 + 0.35 * amount[2]);
)" + blackBackground(14) + addMixer(15, 16),
            {},
            {},
            "Classic"};
    def.globals = "\nbool inPicture(vec2 q, vec2 size)\n"
                  "{\n    return all(greaterThanEqual(q, vec2(0.0))) && all(lessThanEqual(q, size));\n}\n";
    return def;
}

// The picture's alpha becomes a mask from its colour, so black turns transparent: Rec. 601 luma, or for Red, Green or
// Blue the sum of the other two channels, times a gain of 2 x contrast up to 0.5 and 1 + 10 x (contrast - 0.5) above;
// inverted, the gain applies to one minus that. The colour stays as it is.
EffectDef classicAutoMask()
{
    EffectDef def{"classicAutoMask",
            StageKind::Pointwise,
            {classicChoice("channel", "Channel", 0.0, {"Luminance", "Red", "Green", "Blue"}),
             in(units::plain, classic("contrast", "Contrast", 0.5)),
             classicFlag("invert", "Invert")},
            false,
            R"(
    int channel = int(pb[p + 0] + 0.5);
    float m = channel == 0 ? dot(c.rgb, vec3(0.299, 0.587, 0.114))
            : channel == 1 ? c.g + c.b : channel == 2 ? c.r + c.b : c.r + c.g;
    float gain = pb[p + 1] <= 0.5 ? 2.0 * pb[p + 1] : 1.0 + 10.0 * (pb[p + 1] - 0.5);
    return vec4(c.rgb, c.a * saturate((pb[p + 2] > 0.5 ? 1.0 - m : m) * gain));
)",
            {},
            {},
            "Classic"};
    // It changes only alpha.
    def.keepsBlack = [](const float*) { return true; };
    return def;
}

// Measured against the reference: adds brightness to the colour, then scales it about black by 1 + contrast below 0
// and 1 + 5 x contrast above.
EffectDef classicBrightnessContrast()
{
    return {"classicBrightnessContrast",
            StageKind::Pointwise,
            {in({" %", 100.0, 0}, {"brightness", -1.0, 1.0, 0.0, ParamDrive::Value, {}, "Brightness"}),
             in(units::plain, {"contrast", -1.0, 1.0, 0.0, ParamDrive::Value, {}, "Contrast"}),
             classicBlend()},
            false,
            R"(
    float gain = 1.0 + (pb[p + 1] < 0.0 ? pb[p + 1] : 5.0 * pb[p + 1]);
    vec4 o = vec4(saturate((c.rgb + pb[p + 0]) * gain), c.a);
)" + addMixer(2, 3),
            {},
            {},
            "Classic"};
}

// Measured against the reference, each control alone: colour^(2 x (1 - midpoint)) (0 stays 0), times (2 x ratio)^2,
// plus 4 x (base - 0.5); all three neutral at 0.5. Their order is assumed.
EffectDef classicLevels()
{
    return {"classicLevels",
            StageKind::Pointwise,
            {in(units::plain, classic("base", "Base", 0.5)),
             in(units::plain, classic("midpoint", "Midpoint", 0.5)),
             in(units::plain, classic("ratio", "Ratio", 0.5)),
             classicBlend()},
            false,
            R"(
    vec3 x = mix(pow(c.rgb, vec3(2.0 * (1.0 - pb[p + 1]))), vec3(0.0), lessThanEqual(c.rgb, vec3(0.0)));
    vec4 o = vec4(saturate(x * (4.0 * pb[p + 2] * pb[p + 2]) + 4.0 * (pb[p + 0] - 0.5)), c.a);
)" + addMixer(3, 4),
            {},
            {},
            "Classic"};
}

// Measured against the reference: colour 1 where the luma (0.30 R + 0.59 G + 0.11 B) is above the threshold, colour 2
// below (each colour multiplied by its own alpha), blended by a smoothstep between luma -+ smoothness / 2 clamped to
// 0..1. Alpha keeps the picture's (Original), multiplies the colour's alpha by it (Combine) or is 1 (Replace).
EffectDef classicThreshold()
{
    auto channel = [](std::string name, std::string label, double defaultValue) {
        return in(units::plain, classic(std::move(name), std::move(label), defaultValue));
    };
    return {"classicThreshold",
            StageKind::Pointwise,
            {channel("threshold", "Threshold", 0.5),
             channel("smoothness", "Smoothness", 0.0),
             channel("color1Red", "Color 1 Red", 1.0),
             channel("color1Green", "Color 1 Green", 1.0),
             channel("color1Blue", "Color 1 Blue", 1.0),
             channel("color1Alpha", "Color 1 Alpha", 1.0),
             channel("color2Red", "Color 2 Red", 0.0),
             channel("color2Green", "Color 2 Green", 0.0),
             channel("color2Blue", "Color 2 Blue", 0.0),
             channel("color2Alpha", "Color 2 Alpha", 1.0),
             classicChoice("alpha", "Alpha", 0.0, {"Original", "Combine", "Replace"}),
             classicBlend()},
            false,
            R"(
    float y = dot(c.rgb, vec3(0.30, 0.59, 0.11));
    float soft = pb[p + 1] * 0.5;
    float below = soft > 0.0 ? smoothstep(max(y - soft, 0.0), min(y + soft, 1.0), pb[p + 0]) : float(y <= pb[p + 0]);
    vec4 one = vec4(pb[p + 2], pb[p + 3], pb[p + 4], pb[p + 5]), two = vec4(pb[p + 6], pb[p + 7], pb[p + 8], pb[p + 9]);
    vec4 o = mix(vec4(one.rgb * one.a, one.a), vec4(two.rgb * two.a, two.a), below);
    int alpha = int(pb[p + 10] + 0.5);
    o.a = alpha == 0 ? c.a : alpha == 1 ? o.a * c.a : 1.0;
)" + addMixer(11, 12),
            {},
            {},
            "Classic"};
}

// Measured against the reference, in HSL: the hue (reds below pure red count as negative, from -1/6 to 5/6) times
// hueScale plus hueRotate in turns, the saturation times 2 x satScale.
EffectDef classicHueRotate()
{
    return {"classicHueRotate",
            StageKind::Pointwise,
            {in(units::plain, classic("hueScale", "Hue Scale", 1.0)),
             wrapping(units::plain, classic("hueRotate", "Hue Rotate", 0.0)),
             in(units::plain, classic("satScale", "Sat. Scale", 0.5)),
             classicBlend()},
            false,
            R"(
    float hi = max(c.r, max(c.g, c.b)), lo = min(c.r, min(c.g, c.b)), d = hi - lo;
    float l = (hi + lo) * 0.5;
    float s = d / max(1.0 - abs(2.0 * l - 1.0), 1e-5);
    float hue = d <= 0.0 ? 0.0 : hi == c.r ? (c.g - c.b) / d : hi == c.g ? 2.0 + (c.b - c.r) / d : 4.0 + (c.r - c.g) / d;
    hue = fract(hue / 6.0 * pb[p + 0] + pb[p + 1]);
    s = saturate(s * 2.0 * pb[p + 2]);
    vec3 pure = saturate(abs(fract(hue + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
    vec4 o = vec4(saturate(l + (pure - 0.5) * s * (1.0 - abs(2.0 * l - 1.0))), c.a);
)" + addMixer(3, 4),
            {},
            {},
            "Classic"};
}

// Measured against the reference, whose history takes the picture 30 times a second, so each 60th of a second mixes by
// the square root of feedback, whatever the frame rate. Alpha mixes the picture into the history, Luma keeps the
// history wherever its Rec. 601 luma is above 1 - feedback and takes the picture elsewhere. Lighten keeps the lighter of
// the picture and the history, whose excess over the picture fades more slowly, by feedback^0.76 per 30 Hz step, and
// loses at least a level 20 times a second, so its trail runs out to the picture as the reference's does. The history
// is 8-bit as in the reference, so an Alpha fade stalls a few levels above the picture and leaves a faint ghost; Alpha
// moves at least a level per 60th of a second while the reference's update would, so the ghost stays as faint as the
// reference's. An empty history starts from the picture, so feedback 1 freezes it.
EffectDef classicTrails()
{
    return {"classicTrails",
            StageKind::MultiPass,
            {in(units::plain, classic("feedback", "Feedback", 0.9)),
             classicChoice("mode", "Mode", 0.0, {"Alpha", "Luma", "Lighten"}),
             classicBlend()},
            false,
            R"(
    vec4 kept = samplePixel(a, px);
    vec4 o = vec4(kept.rgb / max(kept.a, 1e-4), kept.a);
)" + addMixer(2, 3),
            {{1, true, R"(
    vec4 c = INPUT(px);
    c.rgb *= c.a;
    vec4 kept = samplePixel(prev, px);
    if (kept.a <= 0.0)
        return c;
    int mode = int(pb[p + 1] + 0.5);
    if (mode == 1)
        return dot(kept.rgb, vec3(0.299, 0.587, 0.114)) > 1.0 - pb[p + 0] ? kept : c;
    if (mode == 2) {
        float tick = floor(h.time * 20.0 + 1e-3) - floor((h.time - h.dt) * 20.0 + 1e-3);
        vec4 ground = round(c * 255.0);
        vec4 excess = max(kept * 255.0 - ground, 0.0);
        vec4 next = min(round(excess * perFrame(pow(pb[p + 0], 0.38), h)), max(excess - tick, 0.0));
        return max(c, (ground + next) / 255.0);
    }
    vec4 mixed = mix(c, kept, perFrame(sqrt(pb[p + 0]), h));
    vec4 delta = mixed - kept;
    vec4 moving = step(0.5 / 255.0, abs(c - kept) * (1.0 - pb[p + 0]));
    return kept + sign(delta) * max(abs(delta), moving * frames60(h) / 255.0);
)"}},
            {},
            "Classic"};
}

// Measured against the reference: a flash every 2 x (1 - frequency) seconds at any tempo, on for half of it; frequency 0
// never flashes and 1 stays on. Sync to Beat, our own control and on by default, counts that period in beats at
// 120 BPM instead (4 x (1 - frequency) beats), so the flash follows the tempo. Fade turns the flash into a ramp up and
// down over the period. The flash replaces the picture with the colour at its alpha.
EffectDef classicStrobe()
{
    return {"classicStrobe",
            StageKind::Pointwise,
            {in(units::plain, classic("frequency", "Frequency", 0.5)),
             classicFlag("fade", "Fade"),
             classic("colorRed", "Color Red", 1.0),
             classic("colorGreen", "Color Green", 1.0),
             classic("colorBlue", "Color Blue", 1.0),
             classic("colorAlpha", "Color Alpha", 1.0),
             classicFlag("syncToBeat", "Sync to Beat", 1.0),
             classicBlend()},
            false,
            R"(
    float period = 2.0 * (1.0 - pb[p + 0]);
    float clock = pb[p + 6] > 0.5 ? h.beat * 0.5 : h.time;
    float ramp = period > 0.0 ? 1.0 - abs(2.0 * fract(clock / period) - 1.0) : 1.0;
    float flash = pb[p + 0] <= 0.0 ? 0.0 : pb[p + 1] > 0.5 ? ramp : step(0.5, ramp);
    vec4 o = mix(c, vec4(pb[p + 2], pb[p + 3], pb[p + 4], pb[p + 5]), flash);
)" + addMixer(7, 8),
            {},
            {},
            "Classic"};
}

constexpr uint32_t kShiftGlitchBands = 12;

// The pattern the shader should work out this frame as two 16-bit halves and 1, then for each axis and band the 16-bit
// halves of its two hashes, n (height, start) and m (run, its start), all exact in single precision.
void classicShiftGlitchBands(const float* p, const float*, float time, float beat, float* out)
{
    const float period = 1.0f - p[0];
    const float clock = p[5] > 0.5f ? beat * 0.5f : time;
    const uint32_t pattern = period > 0.0f ? uint32_t(std::floor(std::max(clock, 0.0f) / period))
                                           : uint32_t(std::max(time, 0.0f) * 60.0f);
    out[0] = float(pattern & 0xffffu);
    out[1] = float(pattern >> 16u);
    out[2] = 1.0f;
    for (uint32_t axis = 0; axis < 2; ++axis) {
        const uint32_t seed = pcg(pattern * 2u + axis);
        for (uint32_t i = 0; i < kShiftGlitchBands; ++i) {
            const uint32_t n = pcg(seed ^ ((i + 1u) * 0x9e3779b9u));
            const uint32_t m = pcg(n);
            float* band = out + 3 + (axis * kShiftGlitchBands + i) * 4;
            band[0] = float(n & 0xffffu);
            band[1] = float(n >> 16u);
            band[2] = float(m & 0xffffu);
            band[3] = float(m >> 16u);
        }
    }
}

// Measured against the reference on the UV probe: horizontal lays ceil(12 x horizontal) bands across the picture, each
// up to 0.5 x size of the picture's height high (uniform) at a random place, and each shows a random run of the
// picture's rows, up to 11 % of its height, stretched over the band; later bands lie over earlier ones. Vertical does
// the same with columns. Row and column band i are laid in the same step, the column band last, so where they cross
// the later one lies on top and shows the picture unshifted along the other axis. The pattern changes every
// 1 - frequency seconds at any tempo, every frame at 1; with Sync to Beat (ours, on by default) every
// 2 x (1 - frequency) beats, the same at 120 BPM. Black BG only shows on transparent input, which this stage keeps as
// it is; a remap mixes by Opacity, so the Add mixer matches only at full Opacity.
//
// The CPU hashes the bands once a frame (classicShiftGlitchBands) so that a pixel does not; the shader uses them only
// when it works out the same pattern, and hashes them itself otherwise, so the picture is the same either way.
EffectDef classicShiftGlitch()
{
    EffectDef def{"classicShiftGlitch",
            StageKind::Remap,
            {in(units::plain, classic("frequency", "Frequency", 1.0)),
             in(units::plain, classic("vertical", "Vertical", 0.0)),
             in(units::plain, classic("horizontal", "Horizontal", 0.5)),
             in(units::plain, classic("size", "Size", 0.5)),
             classicFlag("blackBG", "Black BG"),
             classicFlag("syncToBeat", "Sync to Beat", 1.0),
             classicBlend()},
            false,
            R"(
    float period = 1.0 - pb[p + 0];
    float clock = pb[p + 5] > 0.5 ? h.beat * 0.5 : h.time;
    uint pattern = period > 0.0 ? uint(floor(max(clock, 0.0) / period)) : uint(max(h.time, 0.0) * 60.0);
    bool cached = pb[q + 2] > 0.0 && uint(pb[q + 0]) + (uint(pb[q + 1]) << 16u) == pattern;
    // The topmost band over this pixel on each axis: rows (horizontal bands) along y, then columns along x. A column
    // band below the row band's index cannot lie on top, so the columns stop there.
    ivec2 count = pb[p + 3] > 0.0 ? ivec2(ceil(12.0 * vec2(pb[p + 2], pb[p + 1]) - 1e-4)) : ivec2(0);
    ivec2 top = ivec2(-1);
    vec2 from = px;
    for (int axis = 0; axis < 2; ++axis) {
        float dim = axis == 0 ? h.size.y : h.size.x, at = axis == 0 ? px.y : px.x;
        uint seed = pcg(pattern * 2u + uint(axis));
        for (int i = count[axis] - 1; i >= (axis == 0 ? 0 : max(top.x, 0)); --i) {
            vec4 b;
            if (cached) {
                int band = q + 3 + (axis * 12 + i) * 4;
                b = vec4(pb[band + 0], pb[band + 1], pb[band + 2], pb[band + 3]);
            } else {
                uint n = pcg(seed ^ (uint(i + 1) * 0x9e3779b9u)), m = pcg(n);
                b = vec4(float(n & 0xffffu), float(n >> 16u), float(m & 0xffffu), float(m >> 16u));
            }
            float high = max(1.0, b.x / 65535.0 * 0.5 * pb[p + 3] * dim);
            float start = b.y / 65536.0 * dim;
            if (at < start || at >= start + high)
                continue;
            float run = b.z / 65535.0 * 0.11 * dim;
            from[1 - axis] = b.w / 65535.0 * (dim - run) + (at - start) * run / high;
            top[axis] = i;
            break;
        }
    }
    vec2 s = px;
    if (top.y >= 0 && top.y >= top.x)
        s.x = from.x;
    else if (top.x >= 0)
        s.y = from.y;
    return clamp(s, vec2(0.5), h.size - 0.5);
)",
            {},
            {},
            "Classic"};
    def.derive = classicShiftGlitchBands;
    def.derived = 3 + 2 * kShiftGlitchBands * 4;
    return def;
}

// Edges of each colour channel, measured on the picture as the reference holds it: premultiplied at 8 bits, with the
// alpha taken out again. Mode 1 is a Sobel gradient (a step of one level reads as one) times 20 x multiplier, mode 0
// how far the pixel stands above the darkest of its 3x3 neighbours times 15 x multiplier. Use Color draws the colour
// (times its alpha) at the Rec. 601 luma of the channel edges. Alpha is the picture's. Amount changes nothing in the
// reference.
EffectDef classicEdgeDetection()
{
    auto channel = [](std::string name, std::string label) {
        return in(units::plain, classic(std::move(name), std::move(label), 1.0));
    };
    return {"classicEdgeDetection",
            StageKind::Neighbourhood,
            {classicChoice("mode", "Mode", 1.0, {"Simple", "Sobel"}),
             in(units::plain, classic("multiplier", "Multiplier", 0.2)),
             classicFlag("useColor", "Use Color"),
             channel("colorRed", "Color Red"),
             channel("colorGreen", "Color Green"),
             channel("colorBlue", "Color Blue"),
             channel("colorAlpha", "Color Alpha"),
             channel("amount", "Amount"),
             classicBlend()},
            false,
            R"(
    vec4 c = samplePixel(t, px);
    vec3 e = vec3(0.0);
    if (pb[p + 1] != 0.0) {
        vec3 k[9];
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 3; ++i) {
                vec4 v = samplePixel(t, px + vec2(i - 1, j - 1));
                k[j * 3 + i] = round(v.rgb * v.a * 255.0) / (255.0 * max(v.a, 1e-4));
            }
        if (pb[p + 0] > 0.5) {
            vec3 gx = k[2] + 2.0 * k[5] + k[8] - k[0] - 2.0 * k[3] - k[6];
            vec3 gy = k[6] + 2.0 * k[7] + k[8] - k[0] - 2.0 * k[1] - k[2];
            e = sqrt(gx * gx + gy * gy) * (5.0 * pb[p + 1]);
        } else {
            vec3 lo = k[0];
            for (int i = 1; i < 9; ++i)
                lo = min(lo, k[i]);
            e = (k[4] - lo) * (15.0 * pb[p + 1]);
        }
    }
    if (pb[p + 2] > 0.5)
        e = saturate(dot(e, vec3(0.299, 0.587, 0.114))) * vec3(pb[p + 3], pb[p + 4], pb[p + 5]) * pb[p + 6];
    vec4 o = vec4(saturate(e), c.a);
)" + addMixer(8, 9),
            {},
            {},
            "Classic"};
}

// Generators draw their own picture in straight colour and ignore their input. Angles are degrees, 0 = vertical lines;
// sizes count across the composition's height, so a picture keeps its look at any aspect.
EffectDef generator(std::string type, std::vector<ParamDef> params, std::string glsl)
{
    EffectDef def{std::move(type), StageKind::Pointwise, std::move(params), false, std::move(glsl), {}};
    def.generator = true;
    return def;
}

// red, green, blue, or with a prefix e.g. toRed, toGreen, toBlue.
std::vector<ParamDef> colour(const std::string& prefix = "", double value = 1.0)
{
    auto name = [&](const char* lower, const char* upper) { return prefix.empty() ? lower : prefix + upper; };
    return {percent({name("red", "Red"), 0.0, 1.0, value}), percent({name("green", "Green"), 0.0, 1.0, value}),
            percent({name("blue", "Blue"), 0.0, 1.0, value})};
}

std::vector<ParamDef> joined(std::vector<ParamDef> a, const std::vector<ParamDef>& b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// A step that shows nothing.
EffectDef blank()
{
    return generator("blank", {}, R"(
    return vec4(0.0);
)");
}

EffectDef solid()
{
    return generator("solid", colour(), R"(
    return vec4(pb[p + 0], pb[p + 1], pb[p + 2], 1.0);
)");
}

// On for `duty` of every `rate` beats of the beat clock, from each period's start; off is transparent.
EffectDef strobe()
{
    return generator("strobe",
                     joined({in(units::beats, {"rate", 0.25, 16.0, 1.0, ParamDrive::Beats}),
                             percent({"duty", 0.0, 1.0, 0.5})},
                            colour()),
                     R"(
    return fract(pb[p + 0]) < pb[p + 1] ? vec4(pb[p + 2], pb[p + 3], pb[p + 4], 1.0) : vec4(0.0);
)");
}

// `count` parallel lines across the height, each `width` of its period wide, moving across at `speed` lines per
// second; `softness` blurs their edges by up to half a period. Between them it is transparent. Edges are box filtered
// over a pixel, so a thin line never flickers.
EffectDef lines()
{
    return generator("lines",
                     joined({in(units::lines, {"count", 1.0, 64.0, 8.0}), percent({"width", 0.0, 1.0, 0.5}),
                             wrapping(units::degrees, {"angle", -180.0, 180.0, 0.0}),
                             in(units::linesPerSecond, {"speed", -8.0, 8.0, 0.0, ParamDrive::Phase}),
                             percent({"softness", 0.0, 1.0, 0.0})},
                            colour()),
                     R"(
    float a = pb[p + 2] * (M_PI_F / 180.0);
    vec2 d = vec2(cos(a), sin(a));
    float perPixel = pb[p + 0] / h.size.y;
    float s = dot(px - h.size * 0.5, d) * perPixel - pb[p + 3];
    float v = abs(fract(s + 0.5) - 0.5);
    float hw = pb[p + 1] * 0.5;
    float w = max(perPixel, pb[p + 4] * 0.5);
    float cover = max(min(v + w * 0.5, hw) - max(v - w * 0.5, -hw), 0.0) +
                  max(min(v + w * 0.5, 1.0 + hw) - max(v - w * 0.5, 1.0 - hw), 0.0);
    return vec4(pb[p + 5], pb[p + 6], pb[p + 7], saturate(cover / w));
)");
}

// `count` concentric rings from the centre to the top edge, each `width` of its period wide, around a centre moved by
// `x` and `y` (shares of the width and height), spreading outward at `speed` rings per second (inward when negative):
// a tunnel on the beat with speed as a phase. `softness` blurs the edges by up to half a period. Between them it is
// transparent. One length per pixel; edges box filtered over a pixel as Lines does, so thin rings never shimmer.
EffectDef rings()
{
    return generator("rings",
                     joined({in(units::rings, {"count", 1.0, 64.0, 8.0}), percent({"width", 0.0, 1.0, 0.5}),
                             in(units::ringsPerSecond, {"speed", -8.0, 8.0, 0.0, ParamDrive::Phase}),
                             percent({"softness", 0.0, 1.0, 0.0}), percent({"x", -0.5, 0.5, 0.0}),
                             percent({"y", -0.5, 0.5, 0.0})},
                            colour()),
                     R"(
    float perPixel = pb[p + 0] / (0.5 * h.size.y);
    vec2 centre = h.size * (0.5 + vec2(pb[p + 4], -pb[p + 5]));
    float s = length(px - centre) * perPixel - pb[p + 2];
    float v = abs(fract(s + 0.5) - 0.5);
    float hw = pb[p + 1] * 0.5;
    float w = max(perPixel, pb[p + 3] * 0.5);
    float cover = max(min(v + w * 0.5, hw) - max(v - w * 0.5, -hw), 0.0) +
                  max(min(v + w * 0.5, 1.0 + hw) - max(v - w * 0.5, 1.0 - hw), 0.0);
    return vec4(pb[p + 6], pb[p + 7], pb[p + 8], saturate(cover / w));
)");
}

// `count` squares across the height, turned by `angle`, scrolling at `speed` squares per second; the others are
// transparent.
EffectDef checker()
{
    return generator("checker",
                     joined({in(units::cells, {"count", 1.0, 64.0, 8.0}),
                             wrapping(units::degrees, {"angle", -180.0, 180.0, 0.0}),
                             in(units::cellsPerSecond, {"speed", -8.0, 8.0, 0.0, ParamDrive::Phase})},
                            colour()),
                     R"(
    float a = pb[p + 1] * (M_PI_F / 180.0);
    float s = sin(a), k = cos(a);
    float perPixel = pb[p + 0] / h.size.y;
    vec2 q = px - h.size * 0.5;
    q = vec2(k * q.x + s * q.y, -s * q.x + k * q.y) * perPixel;
    q.x -= pb[p + 2];
    vec2 lo = q - 0.5 * perPixel, hi = q + 0.5 * perPixel;
    vec2 sq = (abs(fract(lo) - 0.5) - abs(fract(hi) - 0.5)) / perPixel;
    return vec4(pb[p + 3], pb[p + 4], pb[p + 5], saturate(0.5 + 0.5 * sq.x * sq.y));
)");
}

// From the first colour to the second across the picture along `angle`, `scale` times over (mirrored), scrolling at
// `speed` ramps per second.
EffectDef gradient()
{
    return generator("gradient",
                     joined(joined({wrapping(units::degrees, {"angle", -180.0, 180.0, 0.0}),
                                    in(units::times, {"scale", 0.25, 8.0, 1.0}),
                                    in(units::perSecond, {"speed", -4.0, 4.0, 0.0, ParamDrive::Phase})},
                                   colour("", 0.0)),
                            colour("to", 1.0)),
                     R"(
    float a = pb[p + 0] * (M_PI_F / 180.0);
    vec2 d = vec2(cos(a), sin(a));
    float t = dot(px - h.size * 0.5, d) / (abs(d.x) * h.size.x + abs(d.y) * h.size.y);
    t = t * pb[p + 1] + 0.5 - pb[p + 2];
    t = 1.0 - abs(1.0 - 2.0 * fract(t * 0.5));
    return vec4(mix(vec3(pb[p + 3], pb[p + 4], pb[p + 5]), vec3(pb[p + 6], pb[p + 7], pb[p + 8]), t), 1.0);
)");
}

// Smooth value noise, `scale` blobs across the height, evolving at `speed` per second; `contrast` pushes it towards
// black and the colour. One octave with a cheap lattice hash keeps it light on integrated GPUs.
EffectDef noise()
{
    return generator("noise",
                     joined({in(units::times, {"scale", 0.5, 32.0, 4.0}),
                             in(units::perSecond, {"speed", 0.0, 4.0, 0.5, ParamDrive::Phase}),
                             in(units::times, {"contrast", 0.0, 4.0, 1.0})},
                            colour()),
                     R"(
    if (pb[p + 2] == 0.0)
        return vec4(vec3(pb[p + 3], pb[p + 4], pb[p + 5]) * 0.5, 1.0);
    vec2 xy = (px - h.size * 0.5) * (pb[p + 0] / h.size.y);
    vec3 q = vec3(0.8 * xy.x - 0.6 * xy.y, 0.6 * xy.x + 0.8 * xy.y, pb[p + 1]);
    vec3 i = floor(q), f = fract(q);
    f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    uvec3 cell = uvec3(ivec3(i));
    float v[8];
    for (uint k = 0; k < 8; ++k) {
        uvec3 e = cell + uvec3(k & 1u, (k >> 1u) & 1u, k >> 2u);
        uint n = e.x * 0x8da6b343u + e.y * 0xd8163841u + e.z * 0xcb1ab31fu;
        n ^= n >> 15u;
        n *= 0x2c1b3c6du;
        n ^= n >> 12u;
        v[k] = float(n >> 8u) / 16777215.0;
    }
    vec4 z = mix(vec4(v[0], v[2], v[4], v[6]), vec4(v[1], v[3], v[5], v[7]), f.x);
    vec2 y = mix(z.xz, z.yw, f.y);
    float g = saturate((mix(y.x, y.y, f.z) - 0.5) * pb[p + 2] + 0.5);
    return vec4(vec3(pb[p + 3], pb[p + 4], pb[p + 5]) * g, 1.0);
)");
}

EffectDef withMix(EffectDef effect)
{
    effect.params.push_back({"mix", 0.0, 1.0, 1.0, ParamDrive::Value, {}, effect.category.empty() && !effect.generator ? "" : "Opacity",
                             false, units::percent});
    return effect;
}

// Each variant is an effect of its own beside its effect.
std::deque<EffectDef> withVariants(std::deque<EffectDef> effects)
{
    for (size_t i = 0, count = effects.size(); i < count; ++i)
        for (const EffectVariant& variant : effects[i].variants) {
            EffectDef def = effects[i];
            def.type += "_" + variant.name;
            def.glsl = variant.glsl;
            def.kind = variant.kind.value_or(def.kind);
            def.variants.clear();
            def.variant = true;
            def.mask.clear();
            def.blackOutsideMask = nullptr;
            effects.push_back(std::move(def));
        }
    return effects;
}

// A deque keeps pointers to its elements valid as effects are added.
std::deque<EffectDef> withIsf(std::deque<EffectDef> effects)
{
    for (EffectDef& def : isfEffects())
        effects.push_back(withMix(std::move(def)));
    return effects;
}

// Registering appends, and the last entry of a type is the one found, so an effect replaced or retired at run time
// stays valid for whoever holds it.
struct Registry {
    std::shared_mutex mutex;
    std::deque<EffectDef> effects;
};

Registry& registry()
{
    static Registry effects{.effects = withIsf(withVariants(
        {withMix(transform()), withMix(colorize()), withMix(edgeDetection()), withMix(stripper()),
         withMix(trails()), withMix(reveal()), withMix(slicer()), withMix(thermal()), withMix(grain()),
         withMix(letters()), withMix(dots()), withMix(duotone()), withMix(outline()), withMix(classicStripper()),
         withMix(classicColorize()),
         withMix(classicShiftRgb()), withMix(classicMirror()), withMix(classicKaleidoscope()), withMix(classicTwitch()),
         withMix(classicAutoMask()), withMix(classicBrightnessContrast()), withMix(classicLevels()),
         withMix(classicThreshold()), withMix(classicHueRotate()), withMix(classicMirrorQuad()),
         withMix(classicPolarKaleido()), withMix(classicTrails()), withMix(classicStrobe()),
         withMix(classicShiftGlitch()), withMix(classicEdgeDetection()),
         withMix(blank()), withMix(solid()), withMix(strobe()), withMix(lines()),
         withMix(checker()), withMix(rings()), withMix(gradient()), withMix(noise())}))};
    return effects;
}

// The current entry of every type, in the order the types were first registered. Under the registry's lock.
std::vector<const EffectDef*> currentLocked(const std::deque<EffectDef>& effects)
{
    std::map<std::string, const EffectDef*> latest;
    for (const EffectDef& def : effects)
        latest[def.type] = &def;
    std::vector<const EffectDef*> out;
    std::set<std::string> seen;
    for (const EffectDef& def : effects)
        if (seen.insert(def.type).second && !latest[def.type]->retired)
            out.push_back(latest[def.type]);
    return out;
}

const EffectDef* findLocked(const std::deque<EffectDef>& effects, const std::string& type)
{
    auto it = std::find_if(effects.rbegin(), effects.rend(), [&](const EffectDef& e) { return e.type == type; });
    return it == effects.rend() || it->retired ? nullptr : &*it;
}

} // namespace

std::string formatParam(const ParamUnit& unit, double value)
{
    const double shown = value * unit.scale;
    const double step = std::pow(10.0, -unit.decimals);
    const double rounded = std::round(shown / step) * step;
    char text[32];
    std::snprintf(text, sizeof text, "%.*f", unit.decimals, rounded == 0.0 ? 0.0 : rounded);
    return text + unit.suffix;
}

double wrapInto(double value, double min, double max)
{
    const double span = max - min;
    if (!(span > 0.0))
        return min;
    const double wrapped = min + std::fmod(value - min, span);
    return wrapped < min ? wrapped + span : wrapped;
}

double fitParam(const ParamDef& param, double value)
{
    if (param.wraps && (value < param.min || value > param.max))
        return wrapInto(value, param.min, param.max);
    return std::clamp(value, param.min, param.max);
}

int optionIndex(const ParamDef& param, double value)
{
    const int last = int(param.options.size()) - 1;
    if (last <= 0 || param.max <= param.min)
        return 0;
    const double share = (value - param.min) / (param.max - param.min);
    const auto& starts = param.optionStarts;
    if (!starts.empty())
        return int(std::upper_bound(starts.begin(), starts.end(), share) - starts.begin());
    return std::clamp(int(std::floor(share * last + 0.5)), 0, last);
}

double optionValue(const ParamDef& param, int index)
{
    const int last = int(param.options.size()) - 1;
    if (last <= 0)
        return param.min;
    return param.min + std::clamp(index, 0, last) * (param.max - param.min) / last;
}

const EffectDef* findEffect(const std::string& type)
{
    Registry& r = registry();
    std::shared_lock lock(r.mutex);
    return findLocked(r.effects, type);
}

std::vector<std::string> effectTypes()
{
    std::vector<const EffectDef*> defs;
    {
        Registry& r = registry();
        std::shared_lock lock(r.mutex);
        for (const EffectDef* def : currentLocked(r.effects))
            if (!def->generator && !def->variant)
                defs.push_back(def);
    }
    std::stable_partition(defs.begin(), defs.end(), [](const EffectDef* def) { return def->category.empty(); });
    std::vector<std::string> types;
    for (const EffectDef* def : defs)
        types.push_back(def->type);
    return types;
}

std::string effectLabel(const std::string& type)
{
    if (const EffectDef* def = findEffect(type); def && !def->label.empty())
        return def->label;
    std::string label;
    for (size_t i = 0; i < type.size(); ++i) {
        const char c = type[i];
        if (i == 0)
            label += char(std::toupper(static_cast<unsigned char>(c)));
        else if (std::isupper(static_cast<unsigned char>(c)))
            label += std::string(" ") + c;
        else
            label += c;
    }
    for (const auto& [from, to] : {std::pair<std::string, std::string>{"Rgb", "RGB"}})
        for (size_t at = label.find(from); at != std::string::npos; at = label.find(from, at + to.size()))
            label.replace(at, from.size(), to);
    return label;
}

std::vector<std::string> generatorTypes()
{
    Registry& r = registry();
    std::shared_lock lock(r.mutex);
    std::vector<std::string> types;
    for (const EffectDef* def : currentLocked(r.effects))
        if (def->generator)
            types.push_back(def->type);
    return types;
}

bool isGenerator(const std::string& type)
{
    const EffectDef* def = findEffect(type);
    return def && def->generator;
}

void registerEffect(EffectDef effect)
{
    EffectDef def = withMix(std::move(effect));
    Registry& r = registry();
    std::unique_lock lock(r.mutex);
    r.effects.push_back(std::move(def));
}

void retireEffect(const std::string& type)
{
    Registry& r = registry();
    std::unique_lock lock(r.mutex);
    if (findLocked(r.effects, type))
        r.effects.push_back(EffectDef{.type = type, .retired = true});
}

void setEffectShader(const std::string& type, std::string globals, std::string glsl)
{
    Registry& r = registry();
    std::unique_lock lock(r.mutex);
    auto it = std::find_if(r.effects.rbegin(), r.effects.rend(), [&](const EffectDef& e) { return e.type == type; });
    if (it == r.effects.rend())
        return;
    it->globals = std::move(globals);
    it->glsl = std::move(glsl);
}

std::string effectRegistryVersion()
{
    std::set<std::string> lines;
    Registry& r = registry();
    std::shared_lock lock(r.mutex);
    for (const EffectDef* entry : currentLocked(r.effects)) {
        const EffectDef& def = *entry;
        if (def.category == "User")
            continue;
        std::string line = def.type;
        for (const ParamDef& param : def.params)
            line += " " + param.name + ":" + std::to_string(param.min) + ".." + std::to_string(param.max);
        lines.insert(line);
    }
    uint64_t hash = 14695981039346656037ull;
    for (const std::string& line : lines)
        for (const char c : line + "\n")
            hash = (hash ^ uint8_t(c)) * 1099511628211ull;
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(hash));
    return hex;
}

} // namespace azl
