#include "engine/screen_painter.hpp"

#include "effects/shader_layout.hpp"
#include "engine/metal_util.hpp"
#include "engine/shader_cross.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <vector>

namespace azl {

namespace {

constexpr const char* kPaintSource = R"(#version 450
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_samplerless_texture_functions : require

struct Slice {
    vec4 abcd;
    vec4 efgh;
    vec4 inputRect;
    // The input rect less half a texel, so filtering never pulls in pixels from outside it.
    vec4 bounds;
    // Widths from the left, right, top and bottom edge.
    vec4 soft;
    // Power, luminance, 1 / gamma, and whether any width is set.
    vec4 curve;
    // RGB offsets and the brightness factor.
    vec4 gain;
    // The contrast factor, and whether any colour control is set.
    vec4 tone;
    // Offset of the warp's control points, its patches across and down; no patches means no warp.
    uvec4 warp;
    // Whether the slice's mask texture and the screen's apply.
    uvec4 masks;
    // Pads each slice to the 256 bytes a constant buffer offset steps in, so a fragment shader can take its own.
    vec4 padding[6];
};

layout(set = 2, binding = 0) uniform sampler lin;

#if defined(ENTRY_slice) || defined(ENTRY_warpedSlice) || defined(ENTRY_guide)
layout(set = 0, binding = 0, scalar) readonly buffer Slices { Slice slices[]; };
#endif

#if defined(ENTRY_slice) || defined(ENTRY_warpedSlice)
layout(location = 0) out vec2 outQ;
layout(location = 1) out vec2 outUv;
layout(location = 2) flat out vec4 outBounds;
layout(location = 3) flat out uint outSlice;

vec4 project(Slice s, vec2 p) {
    return vec4(s.abcd.x * p.x + s.abcd.y * p.y + s.abcd.z, s.abcd.w * p.x + s.efgh.x * p.y + s.efgh.y, 0.0,
                s.efgh.z * p.x + s.efgh.w * p.y + 1.0);
}
#else
layout(location = 0) out vec4 fragColour;
#endif

#ifdef ENTRY_slice
// The homography gives w != 1, so the rasterizer interpolates q perspective-correctly.
void main() {
    uint vid = uint(gl_VertexIndex), iid = uint(gl_InstanceIndex);
    vec2 q = vec2(float(vid & 1u), float(vid >> 1));
    gl_Position = project(slices[iid], q);
    outQ = q;
    outUv = slices[iid].inputRect.xy + q * slices[iid].inputRect.zw;
    outBounds = slices[iid].bounds;
    outSlice = iid;
}
#endif

#ifdef ENTRY_warpedSlice
layout(set = 0, binding = 1, scalar) readonly buffer Points { vec2 points[]; };

const uint kCells = 20u;

vec4 bernstein(float t) {
    float u = 1.0 - t;
    return vec4(u * u * u, 3.0 * t * u * u, 3.0 * t * t * u, t * t * t);
}

// Drawn indexed over a grid of 20x20 cells per patch, so each grid point is evaluated on the bicubic surface once; q
// follows the patch parameters, as in Resolume.
void main() {
    uint vid = uint(gl_VertexIndex), iid = uint(gl_InstanceIndex);
    Slice s = slices[iid];
    uvec2 patches = s.warp.yz;
    uint width = patches.x * kCells + 1u;
    uvec2 at = uvec2(vid % width, vid / width);
    uvec2 cellPatch = min(at / kCells, patches - 1u);
    vec4 wx = bernstein(float(at.x - cellPatch.x * kCells) / float(kCells));
    vec4 wy = bernstein(float(at.y - cellPatch.y * kCells) / float(kCells));
    uint stride = patches.x * 3u + 1u;
    uint p = s.warp.x + cellPatch.y * 3u * stride + cellPatch.x * 3u;
    vec2 xy = vec2(0.0);
    for (uint j = 0u; j < 4u; ++j)
        for (uint i = 0u; i < 4u; ++i)
            xy += wx[i] * wy[j] * points[p + j * stride + i];
    gl_Position = project(s, xy);
    outQ = vec2(at) / vec2(patches * kCells);
    outUv = s.inputRect.xy + outQ * s.inputRect.zw;
    outBounds = s.bounds;
    outSlice = iid;
}
#endif

#if defined(ENTRY_paintPlain) || defined(ENTRY_paint) || defined(ENTRY_guide)
layout(location = 0) in vec2 inQ;
layout(location = 1) in vec2 inUv;
layout(location = 2) flat in vec4 inBounds;
layout(location = 3) flat in uint inSlice;
#endif

#if defined(ENTRY_paintPlain) || defined(ENTRY_paint)
layout(set = 1, binding = 0) uniform texture2D picture;
#endif

#ifdef ENTRY_paintPlain
// A slice with no colour, soft edge or masks: one sample, as cheap as a slice can be.
void main() {
    fragColour = texture(sampler2D(picture, lin), clamp(inUv, inBounds.xy, inBounds.zw));
}
#endif

#ifdef ENTRY_paint
layout(set = 1, binding = 1) uniform texture2D sliceMask;
layout(set = 1, binding = 2) uniform texture2D screenMask;
layout(set = 0, binding = 0, scalar) uniform SliceBlock { Slice s; };

float ramp(float x, float power, float luminance) {
    if (x >= 1.0)
        return 1.0;
    x = max(x, 0.0);
    return x < 0.5 ? luminance * pow(2.0 * x, power) : 1.0 - (1.0 - luminance) * pow(2.0 * (1.0 - x), power);
}

float softEdge(vec2 q) {
    float g = 1.0;
    if (s.soft.x > 0.0) g *= ramp(q.x / s.soft.x, s.curve.x, s.curve.y);
    if (s.soft.y > 0.0) g *= ramp((1.0 - q.x) / s.soft.y, s.curve.x, s.curve.y);
    if (s.soft.z > 0.0) g *= ramp(q.y / s.soft.z, s.curve.x, s.curve.y);
    if (s.soft.w > 0.0) g *= ramp((1.0 - q.y) / s.soft.w, s.curve.x, s.curve.y);
    return g < 1.0 ? pow(g, s.curve.z) : 1.0;
}

// Each draw binds its own slice at its offset, so the uniforms stay uniform. Masks are read at the input pixel and the
// output pixel, exactly. Colour works on the premultiplied picture.
void main() {
    if (s.masks.x != 0u) {
        vec2 size = vec2(textureSize(sliceMask, 0));
        if (texelFetch(sliceMask, ivec2(uvec2(clamp(floor(inUv * size), vec2(0.0), size - 1.0))), 0).r == 0.0) {
            fragColour = vec4(0.0);
            return;
        }
    }
    if (s.masks.y != 0u && texelFetch(screenMask, ivec2(uvec2(gl_FragCoord.xy)), 0).r == 0.0) {
        fragColour = vec4(0.0);
        return;
    }
    vec4 c = texture(sampler2D(picture, lin), clamp(inUv, inBounds.xy, inBounds.zw));
    if (s.tone.y > 0.0) {
        vec3 rgb = max(c.rgb + s.gain.rgb * c.a, 0.0) * s.gain.a;
        c.rgb = clamp(0.5 * c.a + (rgb - 0.5 * c.a) * s.tone.x, 0.0, c.a);
    }
    if (s.curve.w > 0.0)
        c *= softEdge(inQ);
    fragColour = c;
}
#endif

#ifdef ENTRY_maskCoverage
layout(set = 0, binding = 0, scalar) readonly buffer Masks { ivec2 masks[]; };
layout(set = 0, binding = 1, scalar) uniform At { uvec2 at; };

// Even-odd rule for a pixel centre in half pixels against points on whole pixels: integer math only.
bool inside(uint p, int n, ivec2 c) {
    bool in_ = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        ivec2 a = masks[p + uint(i)], b = masks[p + uint(j)];
        if ((a.y > c.y) != (b.y > c.y)) {
            int lhs = (c.x - a.x) * (b.y - a.y);
            int rhs = (c.y - a.y) * (b.x - a.x);
            if ((b.y > a.y) == (lhs < rhs))
                in_ = !in_;
        }
    }
    return in_;
}

// Each mask is a header (point count, invert) followed by its points.
bool masked(uint data, uint count, ivec2 c) {
    for (uint m = 0u; m < count; ++m) {
        ivec2 h = masks[data];
        if (inside(data + 1u, h.x, c) != (h.y != 0))
            return true;
        data += 1u + uint(h.x);
    }
    return false;
}

// Rendered when the masks change: 0 where they hide the pixel. `at` is the offset and count of the masks.
void main() {
    fragColour = vec4(masked(at.x, at.y, ivec2(gl_FragCoord.xy) * 2 + 1) ? 0.0 : 1.0);
}
#endif

#ifdef ENTRY_guide
layout(set = 0, binding = 2, scalar) readonly buffer Guides { vec4 guides[]; };

// guides[0]: count and the composition size; then per guide: vertical (1) or not, and its position.
void main() {
    Slice s = slices[inSlice];
    vec2 px = (s.inputRect.xy + inQ * s.inputRect.zw) * guides[0].yz;
    vec2 fw = max(fwidth(px), vec2(1e-4));
    float d = 1e9;
    for (uint i = 1u; i <= uint(guides[0].x); ++i) {
        vec4 g = guides[i];
        d = min(d, g.x > 0.0 ? abs(px.x - g.y * guides[0].y) / fw.x : abs(px.y - g.y * guides[0].z) / fw.y);
    }
    if (d > 2.5)
        discard;
    fragColour = d < 1.0 ? vec4(0.2, 0.8, 1.0, 1.0) : vec4(0.0, 0.0, 0.0, 1.0);
}
#endif

#ifdef ENTRY_cornerTarget
layout(set = 0, binding = 0, scalar) uniform Target { ivec4 t; };

// A white crosshair with a black outline across the whole output through the point, and a ring around it.
void main() {
    ivec2 d = abs(ivec2(gl_FragCoord.xy) - t.xy);
    int d2 = d.x * d.x + d.y * d.y;
    int r = t.z;
    if (min(d.x, d.y) <= 1 || (d2 >= (r - 1) * (r - 1) && d2 <= (r + 1) * (r + 1))) {
        fragColour = vec4(1.0);
        return;
    }
    if (min(d.x, d.y) <= 3 || (d2 >= (r - 3) * (r - 3) && d2 <= (r + 3) * (r + 3))) {
        fragColour = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    discard;
}
#endif

#ifdef ENTRY_card
struct Card {
    uvec2 size;
    int line;
    int diagonal;
    vec4 tint;
    // Lengths of the two text lines and the text's scale.
    uvec4 text;
    // 5x7 glyph bitmaps, bit row * 5 + column: the first line's, then the second's from 32.
    uvec2 glyphs[64];
};

layout(set = 0, binding = 0, scalar) uniform CardBlock { Card k; };

bool ring(ivec2 c, ivec2 o, int r, int l) {
    ivec2 d = c - o;
    int d2 = d.x * d.x + d.y * d.y;
    return d2 >= (2 * r - l) * (2 * r - l) && d2 < (2 * r + l) * (2 * r + l);
}

bool glyphPixel(uint first, uint count, ivec2 p, ivec2 origin) {
    int s = int(k.text.z);
    ivec2 t = p - origin;
    if (t.x < 0 || t.y < 0 || t.y >= 7 * s || t.x >= int(count) * 6 * s)
        return false;
    uint index = uint(t.x / (6 * s));
    uint column = uint(t.x % (6 * s) / s);
    uint row = uint(t.y / s);
    if (column >= 5u)
        return false;
    uint bit = row * 5u + column;
    uvec2 g = k.glyphs[first + index];
    return ((bit < 32u ? g.x >> bit : g.y >> (bit - 32u)) & 1u) != 0u;
}

// The alignment card in composition pixels, all integer math so every GPU draws the same pixels; p2 is the pixel
// centre in half pixels.
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 p2 = p * 2 + 1;
    int w = int(k.size.x), h = int(k.size.y), l = k.line;
    vec3 col = vec3(0.08);
    if (p.x % 40 == 0 || p.y % 40 == 0)
        col = vec3(0.45);
    if (abs(p2.x * h - p2.y * w) < l * k.diagonal || abs(p2.x * h + p2.y * w - 2 * w * h) < l * k.diagonal)
        col = vec3(0.7);
    bool circle = ring(p2, ivec2(w, h), h * 9 / 20, l);
    for (int i = 0; i < 4; ++i)
        circle = circle || ring(p2, ivec2(w * (1 + 2 * (i & 1)) / 2, h * (1 + (i & 2)) / 2), h / 8, l);
    if (circle || abs(p2.x - w) <= l || abs(p2.y - h) <= l)
        col = vec3(1.0);

    int barTop = h * 5 / 8, barLeft = w / 4;
    if (p.y >= barTop && p.y < barTop + h / 10 && p.x >= barLeft && p.x < w - barLeft) {
        const vec3 bars[8] = vec3[](vec3(1, 1, 1), vec3(1, 1, 0), vec3(0, 1, 1), vec3(0, 1, 0),
                                    vec3(1, 0, 1), vec3(1, 0, 0), vec3(0, 0, 1), vec3(0, 0, 0));
        col = bars[min((p.x - barLeft) * 8 / (w - 2 * barLeft), 7)];
    }

    int s = int(k.text.z);
    int lineGap = 10 * s;
    int top = h / 4;
    int width = int(max(k.text.x, k.text.y)) * 6 * s;
    if (p.y >= top - 2 * s && p.y < top + lineGap + 9 * s && abs(p2.x - w) < width + 4 * s) {
        col = vec3(0.0);
        if (glyphPixel(0u, k.text.x, p, ivec2((w - int(k.text.x) * 6 * s + s) / 2, top)))
            col = k.tint.rgb;
        if (glyphPixel(32u, k.text.y, p, ivec2((w - int(k.text.y) * 6 * s + s) / 2, top + lineGap)))
            col = vec3(1.0);
    }
    if (p.x < 2 * l || p.y < 2 * l || p.x >= w - 2 * l || p.y >= h - 2 * l)
        col = vec3(1.0, 0.0, 0.0);
    fragColour = vec4(col, 1.0);
}
#endif

#ifdef ENTRY_frameBits
layout(set = 0, binding = 0, scalar) uniform Bits { uvec2 bits; };

// x: the frame index, y: the bar width in pixels.
void main() {
    uint x = uint(gl_FragCoord.x);
    uint bit = min(x / bits.y, 15u);
    if (x % bits.y < max(bits.y / 8u, 1u)) {
        fragColour = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    fragColour = ((bits.x >> (15u - bit)) & 1u) != 0u ? vec4(1.0) : vec4(0.15, 0.15, 0.15, 1.0);
}
#endif
)";

constexpr MTL::PixelFormat kCardFormat = MTL::PixelFormatRGBA8Unorm;
constexpr MTL::PixelFormat kMaskFormat = MTL::PixelFormatR8Unorm;
constexpr uint32_t kCells = 20;
constexpr size_t kAlign = 256;
constexpr size_t kCardLine = 32;

struct SliceUniform {
    float abcd[4];
    float efgh[4];
    float input[4];
    float bounds[4];
    float soft[4];
    float curve[4];
    float gain[4];
    float tone[4];
    uint32_t warp[4];
    uint32_t masks[4];
    float padding[24];
};
static_assert(sizeof(SliceUniform) == 256);

struct CardUniform {
    uint32_t size[2];
    int32_t line;
    int32_t diagonal;
    float tint[4];
    uint32_t text[4];
    uint32_t glyphs[2 * kCardLine][2];
};

// Distinct text colours for neighbouring screens, so projectors are told apart while aligning.
constexpr float kTints[6][4] = {{0.9f, 0.2f, 0.2f, 1.0f}, {0.2f, 0.8f, 0.3f, 1.0f}, {0.25f, 0.45f, 1.0f, 1.0f},
                                {0.9f, 0.8f, 0.2f, 1.0f}, {0.8f, 0.3f, 0.9f, 1.0f}, {0.2f, 0.8f, 0.9f, 1.0f}};

// 5x7 capitals, digits and a little punctuation, row by row from the top; anything else draws as '?'.
struct Glyph {
    char c;
    const char* rows[7];
};

constexpr Glyph kFont[] = {
    {' ', {".....", ".....", ".....", ".....", ".....", ".....", "....."}},
    {'?', {".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."}},
    {'A', {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'B', {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}},
    {'C', {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}},
    {'D', {"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."}},
    {'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}},
    {'F', {"#####", "#....", "#....", "####.", "#....", "#....", "#...."}},
    {'G', {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####"}},
    {'H', {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'I', {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'J', {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."}},
    {'K', {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"}},
    {'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}},
    {'M', {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"}},
    {'N', {"#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#"}},
    {'O', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'P', {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
    {'Q', {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"}},
    {'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
    {'S', {".####", "#....", "#....", ".###.", "....#", "....#", "####."}},
    {'T', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
    {'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
    {'W', {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#."}},
    {'X', {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}},
    {'Y', {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}},
    {'Z', {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}},
    {'0', {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}},
    {'1', {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'2', {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}},
    {'3', {"#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."}},
    {'4', {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}},
    {'5', {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}},
    {'6', {"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."}},
    {'7', {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."}},
    {'8', {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}},
    {'9', {".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."}},
    {'-', {".....", ".....", ".....", "#####", ".....", ".....", "....."}},
    {'.', {".....", ".....", ".....", ".....", ".....", ".##..", ".##.."}},
    {':', {".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."}},
    {'/', {".....", "....#", "...#.", "..#..", ".#...", "#....", "....."}},
    {'_', {".....", ".....", ".....", ".....", ".....", ".....", "#####"}},
    {'(', {"...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#."}},
    {')', {".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..."}},
};

std::array<uint32_t, 2> glyphBits(char c)
{
    const char upper = char(std::toupper(static_cast<unsigned char>(c)));
    const Glyph* glyph = &kFont[1];
    for (const Glyph& g : kFont)
        if (g.c == upper)
            glyph = &g;
    uint64_t bits = 0;
    for (int row = 0; row < 7; ++row)
        for (int column = 0; column < 5; ++column)
            if (glyph->rows[row][column] == '#')
                bits |= uint64_t(1) << (row * 5 + column);
    return {uint32_t(bits), uint32_t(bits >> 32)};
}

uint32_t setText(CardUniform& card, size_t first, const std::string& text)
{
    const size_t count = std::min(text.size(), kCardLine);
    for (size_t i = 0; i < count; ++i) {
        const auto bits = glyphBits(text[i]);
        card.glyphs[first + i][0] = bits[0];
        card.glyphs[first + i][1] = bits[1];
    }
    return uint32_t(count);
}

std::string outputLabel(const Screen& screen)
{
    return screen.display.empty() ? "no output" : screen.display;
}

CardUniform cardUniform(const Screen& screen, uint32_t w, uint32_t h, uint32_t textScale)
{
    CardUniform u{};
    u.size[0] = w;
    u.size[1] = h;
    u.line = int32_t(std::max(h / 540, 1u));
    u.diagonal = int32_t(std::lround(std::sqrt(double(w) * w + double(h) * h)));
    std::memcpy(u.tint, kTints[screen.id % 6], sizeof(u.tint));
    u.text[0] = setText(u, 0, screen.name);
    u.text[1] = setText(u, kCardLine, outputLabel(screen));
    u.text[2] = textScale;
    return u;
}

int32_t halfPixels(double v, uint32_t size)
{
    return 2 * int32_t(std::lround(v * size));
}

void appendMasks(std::vector<int32_t>& data, const std::vector<Mask>& masks, uint32_t width, uint32_t height)
{
    for (const Mask& mask : masks) {
        data.push_back(int32_t(mask.points.size()));
        data.push_back(mask.invert ? 1 : 0);
        for (const Point& p : mask.points) {
            data.push_back(halfPixels(p[0], width));
            data.push_back(halfPixels(p[1], height));
        }
    }
}

bool validWarp(const Warp& warp)
{
    const auto size = warpGridSize(warp);
    return !warp.points.empty() && warp.patches[0] >= 1 && warp.patches[0] <= kMaxWarpPatches &&
           warp.patches[1] >= 1 && warp.patches[1] <= kMaxWarpPatches &&
           warp.points.size() == size_t(size[0] * size[1]);
}

float colorSum(double screen, double slice)
{
    return float(std::clamp(screen + slice, -1.0, 1.0));
}

size_t aligned(size_t bytes)
{
    return (bytes + kAlign - 1) / kAlign * kAlign;
}

} // namespace


struct ScreenPainter::Draw {
    uint64_t input = 0;
    uint32_t uniform = 0;
    // Null for a plain quad.
    MTL::Buffer* mesh = nullptr;
    uint32_t meshIndices = 0;
    MTL::Texture* mask = nullptr;
    // No colour, soft edge or masks.
    bool plain = false;
    // The composition picture with no soft edge or masks: opaque wherever it draws.
    bool opaque = false;
};

struct ScreenPainter::Prepared {

    Screen screen;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<Draw> draws;
    MTL::Texture* screenMask = nullptr;
    NS::SharedPtr<MTL::Buffer> buffer;
    size_t pointsOffset = 0;
    size_t masksOffset = 0;
};

ScreenView screenView(const Composition& composition, const Screen& screen, bool showGuides,
                      std::optional<Point> target, std::chrono::steady_clock::time_point identify)
{
    ScreenView view{screen, showGuides ? composition.guides : std::vector<Guide>{}, target, identify};
    view.screen.testCard = screen.testCard || composition.testCard;
    return view;
}

// Heckbert's square-to-quad mapping, with the unit square's (0,0), (1,0), (1,1), (0,1) on TL, TR, BR, BL.
std::optional<std::array<float, 8>> sliceHomography(const std::array<double, 8>& corners)
{
    double x[4], y[4];
    for (int i = 0; i < 4; ++i) {
        x[i] = corners[i * 2] * 2.0 - 1.0;
        y[i] = 1.0 - corners[i * 2 + 1] * 2.0;
    }
    const double sx = x[0] - x[1] + x[2] - x[3];
    const double sy = y[0] - y[1] + y[2] - y[3];
    const double dx1 = x[1] - x[2], dx2 = x[3] - x[2], dy1 = y[1] - y[2], dy2 = y[3] - y[2];
    const double den = dx1 * dy2 - dx2 * dy1;
    if (std::abs(den) < 1e-12)
        return std::nullopt;
    const double g = (sx * dy2 - dx2 * sy) / den;
    const double h = (dx1 * sy - sx * dy1) / den;
    if (1.0 + g <= 0.0 || 1.0 + h <= 0.0 || 1.0 + g + h <= 0.0)
        return std::nullopt;
    return std::array<float, 8>{float(x[1] - x[0] + g * x[1]), float(x[3] - x[0] + h * x[3]), float(x[0]),
                                float(y[1] - y[0] + g * y[1]), float(y[3] - y[0] + h * y[3]), float(y[0]),
                                float(g), float(h)};
}

ScreenPainter::ScreenPainter(MTL::Device* device, MTL::PixelFormat targetFormat, std::array<uint32_t, 2> composition,
                             bool frameBits, bool opaqueSlices)
    : device_(device), composition_(composition)
{
    std::vector<ShaderEntry> entries = fullscreenEntries(
        kPaintSource, {"paintPlain", "paint", "maskCoverage", "guide", "cornerTarget", "card", "frameBits"});
    for (const char* vertex : {"slice", "warpedSlice"})
        entries.push_back({vertex, ShaderStage::Vertex, withEntry(kPaintSource, vertex)});
    auto library = makeLibrary(device, mslSource(entries).c_str());
    slices_[0] = slicePipelines(device, library.get(), "paintPlain", targetFormat, true);
    fullSlices_[0] = slicePipelines(device, library.get(), "paint", targetFormat, true);
    if (opaqueSlices) {
        slices_[1] = slicePipelines(device, library.get(), "paintPlain", targetFormat, false);
        fullSlices_[1] = slicePipelines(device, library.get(), "paint", targetFormat, false);
    }
    auto make = [&](const char* vertex, const char* fragment, MTL::PixelFormat format) {
        return makePipeline(device, library.get(), mslFunction(vertex).c_str(), mslFunction(fragment).c_str(), format);
    };
    guides_ = make("slice", "guide", targetFormat);
    warpedGuides_ = make("warpedSlice", "guide", targetFormat);
    maskCoverage_ = make("fullscreen", "maskCoverage", kMaskFormat);
    target_ = make("fullscreen", "cornerTarget", targetFormat);
    card_ = make("fullscreen", "card", kCardFormat);
    identify_ = make("fullscreen", "card", targetFormat);
    if (frameBits)
        frameBits_ = make("fullscreen", "frameBits", targetFormat);
    noMask_ = texture(kMaskFormat, 1, 1);
}

ScreenPainter::~ScreenPainter() = default;

ScreenPainter::SlicePipelines ScreenPainter::slicePipelines(MTL::Device* device, MTL::Library* library,
                                                            const char* fragment, MTL::PixelFormat format,
                                                            bool blend) const
{
    const Blending blending = blend ? Blending::Normal : Blending::None;
    const std::string name = mslFunction(fragment);
    return {makePipeline(device, library, mslFunction("slice").c_str(), name.c_str(), format, blending),
            makePipeline(device, library, mslFunction("warpedSlice").c_str(), name.c_str(), format, blending)};
}

void ScreenPainter::encode(MTL::CommandBuffer* cmd, const Frame* frame, const ScreenView& view, MTL::Texture* target)
{
    AZL_ZONE("paint screen");
    prepare(cmd, view.screen, uint32_t(target->width()), uint32_t(target->height()));
    MTL::Texture* card = view.screen.testCard ? this->card(cmd, view.screen) : nullptr;

    auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* color = pass->colorAttachments()->object(0);
    color->setTexture(target);
    color->setLoadAction(MTL::LoadActionClear);
    color->setClearColor(MTL::ClearColor(0, 0, 0, 1));
    color->setStoreAction(MTL::StoreActionStore);

    AZL_GPU_ZONE(gpu_, pass, "screen");
    auto* enc = cmd->renderCommandEncoder(pass);
    if (frame || card)
        encodeSlices(enc, frame, card);
    if (!view.guides.empty())
        encodeGuides(enc, view.guides);
    if (view.target)
        encodeTarget(enc, *view.target, target);
    if (view.identify > std::chrono::steady_clock::now())
        encodeIdentify(enc, view.screen, target);
    if (frameBits_ && frame)
        encodeFrameBits(enc, frame->index, target);
    enc->endEncoding();
}

// The card at the output's own size, its text as large as the longest line allows.
void ScreenPainter::encodeIdentify(MTL::RenderCommandEncoder* enc, const Screen& screen, MTL::Texture* target) const
{
    const auto w = uint32_t(target->width()), h = uint32_t(target->height());
    const size_t longest = std::max<size_t>(std::min(std::max(screen.name.size(), outputLabel(screen).size()), kCardLine), 1);
    const CardUniform u = cardUniform(screen, w, h, std::max(std::min(h / 30, uint32_t(w * 9 / 10 / (longest * 6))), 1u));
    enc->setRenderPipelineState(identify_.get());
    enc->setFragmentBytes(&u, sizeof(u), 0);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
}

NS::SharedPtr<MTL::Texture> ScreenPainter::texture(MTL::PixelFormat format, uint32_t width, uint32_t height) const
{
    auto desc = NS::RetainPtr(MTL::TextureDescriptor::texture2DDescriptor(format, width, height, false));
    desc->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
    desc->setStorageMode(MTL::StorageModePrivate);
    return NS::TransferPtr(device_->newTexture(desc.get()));
}

// Two triangles per cell of the warp grid's 20x20 cells per patch, shared by every warp of that many patches.
MTL::Buffer* ScreenPainter::meshIndices(std::array<int, 2> patches)
{
    auto& mesh = meshes_[patches];
    if (mesh)
        return mesh.get();
    const uint32_t columns = uint32_t(patches[0]) * kCells, rows = uint32_t(patches[1]) * kCells;
    std::vector<uint32_t> indices;
    indices.reserve(size_t(columns) * rows * 6);
    for (uint32_t y = 0; y < rows; ++y)
        for (uint32_t x = 0; x < columns; ++x) {
            const uint32_t i = y * (columns + 1) + x, below = i + columns + 1;
            indices.insert(indices.end(), {i, i + 1, below, below, i + 1, below + 1});
        }
    mesh = NS::TransferPtr(
        device_->newBuffer(indices.data(), indices.size() * sizeof(uint32_t), MTL::ResourceStorageModeShared));
    return mesh.get();
}

// Rasterises masks (at `at` among the masks, `count` of them) into a coverage texture: 0 where they hide.
void ScreenPainter::renderMasks(MTL::CommandBuffer* cmd, MTL::Texture* coverage, uint32_t at, uint32_t count) const
{
    auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* color = pass->colorAttachments()->object(0);
    color->setTexture(coverage);
    color->setLoadAction(MTL::LoadActionDontCare);
    color->setStoreAction(MTL::StoreActionStore);
    AZL_GPU_ZONE(gpu_, pass, "screen mask");
    auto* enc = cmd->renderCommandEncoder(pass);
    enc->setRenderPipelineState(maskCoverage_.get());
    enc->setFragmentBuffer(prepared_->buffer.get(), prepared_->masksOffset, 0);
    const uint32_t where[2] = {at, count};
    enc->setFragmentBytes(where, sizeof(where), 1);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    enc->endEncoding();
}

// One buffer holds every slice's uniforms, the warps' control points and the masks' points, and masks are rasterised
// into coverage textures: only when the screen or the target's size changes.
void ScreenPainter::prepare(MTL::CommandBuffer* cmd, const Screen& screen, uint32_t width, uint32_t height)
{
    if (prepared_ && prepared_->width == width && prepared_->height == height && prepared_->screen == screen)
        return;
    auto p = std::make_unique<Prepared>();
    p->screen = screen;
    p->width = width;
    p->height = height;

    std::vector<SliceUniform> uniforms;
    std::vector<float> points;
    std::vector<int32_t> masks;
    // Where each slice's masks start among the masks, and how many.
    std::vector<std::array<uint32_t, 2>> sliceMasks;
    for (const Slice& slice : screen.slices) {
        const auto h = sliceHomography(slice.corners);
        if (!h)
            continue;
        const auto& r = slice.inputRect;
        const float hx = 0.5f / float(composition_[0]), hy = 0.5f / float(composition_[1]);
        const SoftEdge& soft = slice.softEdge;
        const bool softOn = soft.left > 0.0 || soft.right > 0.0 || soft.top > 0.0 || soft.bottom > 0.0;
        const float brightness = colorSum(screen.color.brightness, slice.color.brightness);
        const float contrast = colorSum(screen.color.contrast, slice.color.contrast);
        const float rgb[3] = {colorSum(screen.color.red, slice.color.red),
                              colorSum(screen.color.green, slice.color.green),
                              colorSum(screen.color.blue, slice.color.blue)};
        const bool colorOn = brightness != 0.0f || contrast != 0.0f || rgb[0] != 0.0f || rgb[1] != 0.0f ||
                             rgb[2] != 0.0f;
        SliceUniform u{{(*h)[0], (*h)[1], (*h)[2], (*h)[3]},
                       {(*h)[4], (*h)[5], (*h)[6], (*h)[7]},
                       {float(r[0]), float(r[1]), float(r[2]), float(r[3])},
                       {float(r[0]) + hx, float(r[1]) + hy, float(r[0] + r[2]) - hx, float(r[1] + r[3]) - hy},
                       {float(soft.left), float(soft.right), float(soft.top), float(soft.bottom)},
                       {float(soft.power), float(soft.luminance), float(1.0 / soft.gamma), softOn ? 1.0f : 0.0f},
                       {rgb[0], rgb[1], rgb[2], 1.0f + brightness},
                       {1.0f + contrast, colorOn ? 1.0f : 0.0f, 0.0f, 0.0f},
                       {0, 0, 0, 0},
                       {slice.masks.empty() ? 0u : 1u, screen.masks.empty() ? 0u : 1u, 0, 0},
                       {}};
        Draw draw{slice.input, uint32_t(uniforms.size())};
        draw.plain = !softOn && !colorOn && slice.masks.empty() && screen.masks.empty();
        draw.opaque = slice.input == 0 && !softOn && slice.masks.empty() && screen.masks.empty();
        if (validWarp(slice.warp)) {
            u.warp[0] = uint32_t(points.size() / 2);
            u.warp[1] = uint32_t(slice.warp.patches[0]);
            u.warp[2] = uint32_t(slice.warp.patches[1]);
            for (const Point& point : resolvedWarpGrid(slice.warp)) {
                points.push_back(float(point[0]));
                points.push_back(float(point[1]));
            }
            draw.mesh = meshIndices(slice.warp.patches);
            draw.meshIndices = u.warp[1] * u.warp[2] * kCells * kCells * 6;
        }
        if (!slice.masks.empty()) {
            sliceMasks.push_back({uint32_t(masks.size() / 2), uint32_t(slice.masks.size())});
            appendMasks(masks, slice.masks, composition_[0], composition_[1]);
        }
        uniforms.push_back(u);
        p->draws.push_back(draw);
    }
    const uint32_t screenMasksAt = uint32_t(masks.size() / 2);
    appendMasks(masks, screen.masks, width, height);

    const size_t uniformBytes = aligned(std::max<size_t>(uniforms.size() * sizeof(SliceUniform), 16));
    const size_t pointBytes = aligned(points.size() * sizeof(float));
    p->pointsOffset = uniformBytes;
    p->masksOffset = uniformBytes + pointBytes;
    const size_t total = p->masksOffset + std::max<size_t>(masks.size() * sizeof(int32_t), 16);
    p->buffer = NS::TransferPtr(device_->newBuffer(total, MTL::ResourceStorageModeShared));
    auto* bytes = static_cast<uint8_t*>(p->buffer->contents());
    std::memcpy(bytes, uniforms.data(), uniforms.size() * sizeof(SliceUniform));
    std::memcpy(bytes + p->pointsOffset, points.data(), points.size() * sizeof(float));
    std::memcpy(bytes + p->masksOffset, masks.data(), masks.size() * sizeof(int32_t));
    prepared_ = std::move(p);

    size_t next = 0;
    for (Draw& draw : prepared_->draws) {
        const SliceUniform& u = uniforms[draw.uniform];
        if (!u.masks[0])
            continue;
        if (sliceMaskTextures_.size() <= next)
            sliceMaskTextures_.push_back(texture(kMaskFormat, composition_[0], composition_[1]));
        draw.mask = sliceMaskTextures_[next].get();
        renderMasks(cmd, draw.mask, sliceMasks[next][0], sliceMasks[next][1]);
        ++next;
    }
    if (!screen.masks.empty()) {
        if (!screenMaskTexture_ || screenMaskTexture_->width() != width || screenMaskTexture_->height() != height)
            screenMaskTexture_ = texture(kMaskFormat, width, height);
        prepared_->screenMask = screenMaskTexture_.get();
        renderMasks(cmd, prepared_->screenMask, screenMasksAt, uint32_t(screen.masks.size()));
    }
}

// Rendered again only when what it shows changes.
MTL::Texture* ScreenPainter::card(MTL::CommandBuffer* cmd, const Screen& screen)
{
    const std::string key = std::to_string(screen.id % 6) + "\n" + screen.name + "\n" + outputLabel(screen);
    if (cardTexture_ && key == cardKey_)
        return cardTexture_.get();
    if (!cardTexture_)
        cardTexture_ = texture(kCardFormat, composition_[0], composition_[1]);
    cardKey_ = key;

    const uint32_t h = composition_[1];
    const CardUniform u = cardUniform(screen, composition_[0], h, std::max(h / 120, 1u));

    auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
    auto* color = pass->colorAttachments()->object(0);
    color->setTexture(cardTexture_.get());
    color->setLoadAction(MTL::LoadActionDontCare);
    color->setStoreAction(MTL::StoreActionStore);
    AZL_GPU_ZONE(gpu_, pass, "test card");
    auto* enc = cmd->renderCommandEncoder(pass);
    enc->setRenderPipelineState(card_.get());
    enc->setFragmentBytes(&u, sizeof(u), 0);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    enc->endEncoding();
    return cardTexture_.get();
}

void ScreenPainter::drawSlice(MTL::RenderCommandEncoder* enc, const Draw& draw, MTL::RenderPipelineState* quad,
                              MTL::RenderPipelineState* warped) const
{
    if (draw.mesh) {
        enc->setRenderPipelineState(warped);
        enc->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(draw.meshIndices), MTL::IndexTypeUInt32,
                                   draw.mesh, NS::UInteger(0), NS::UInteger(1), NS::Integer(0),
                                   NS::UInteger(draw.uniform));
        return;
    }
    enc->setRenderPipelineState(quad);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangleStrip, NS::UInteger(0), NS::UInteger(4), NS::UInteger(1),
                        NS::UInteger(draw.uniform));
}

void ScreenPainter::bindSliceBuffers(MTL::RenderCommandEncoder* enc) const
{
    MTL::Buffer* buffer = prepared_->buffer.get();
    enc->setVertexBuffer(buffer, 0, 0);
    enc->setVertexBuffer(buffer, prepared_->pointsOffset, 1);
    enc->setFragmentBuffer(buffer, 0, 0);
}

// Each slice is one draw that picks its own uniforms by instance.
void ScreenPainter::encodeSlices(MTL::RenderCommandEncoder* enc, const Frame* frame, MTL::Texture* card) const
{
    bindSliceBuffers(enc);
    enc->setFragmentTexture(prepared_->screenMask ? prepared_->screenMask : noMask_.get(), 2);
    for (const Draw& draw : prepared_->draws) {
        MTL::Texture* picture = card ? card : frame->picture(draw.input);
        if (!picture)
            continue;
        enc->setFragmentTexture(picture, 0);
        enc->setFragmentTexture(draw.mask ? draw.mask : noMask_.get(), 1);
        const size_t opaque = draw.opaque && !card && slices_[1].quad;
        if (draw.plain) {
            drawSlice(enc, draw, slices_[opaque].quad.get(), slices_[opaque].warped.get());
            continue;
        }
        enc->setFragmentBufferOffset(draw.uniform * sizeof(SliceUniform), 0);
        drawSlice(enc, draw, fullSlices_[opaque].quad.get(), fullSlices_[opaque].warped.get());
    }
}

void ScreenPainter::encodeGuides(MTL::RenderCommandEncoder* enc, const std::vector<Guide>& guides) const
{
    std::vector<float> data{float(guides.size()), float(composition_[0]), float(composition_[1]), 0.0f};
    for (const Guide& g : guides)
        data.insert(data.end(), {g.vertical ? 1.0f : 0.0f, float(g.position), 0.0f, 0.0f});
    bindSliceBuffers(enc);
    enc->setFragmentBytes(data.data(), data.size() * sizeof(float), 2);
    for (const Draw& draw : prepared_->draws)
        drawSlice(enc, draw, guides_.get(), warpedGuides_.get());
}

void ScreenPainter::encodeTarget(MTL::RenderCommandEncoder* enc, const Point& point, MTL::Texture* texture) const
{
    const auto width = uint32_t(texture->width()), height = uint32_t(texture->height());
    const int32_t t[4] = {int32_t(std::lround(point[0] * width)), int32_t(std::lround(point[1] * height)),
                          int32_t(std::max(height / 40, 6u)), 0};
    enc->setRenderPipelineState(target_.get());
    enc->setFragmentBytes(t, sizeof(t), 0);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
}

// Drawn last, over the bottom 1/60 of the output.
void ScreenPainter::encodeFrameBits(MTL::RenderCommandEncoder* enc, uint64_t index, MTL::Texture* target) const
{
    const auto width = NS::UInteger(target->width());
    const auto height = std::max<NS::UInteger>(target->height() / 60, 4);
    const uint32_t bits[2] = {uint32_t(index), std::max(uint32_t(width) / kFrameBits, 1u)};
    enc->setScissorRect(MTL::ScissorRect{0, target->height() - height, width, height});
    enc->setRenderPipelineState(frameBits_.get());
    enc->setFragmentBytes(bits, sizeof(bits), 0);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
}

} // namespace azl
