// The shader source in this file is ported to GLSL from AMD FidelityFX Super Resolution 1 (ffx_fsr1.h and ffx_a.h,
// v1.20210629, https://github.com/GPUOpen-Effects/FidelityFX-FSR): the 32-bit EASU and RCAS paths. It is used under
// the MIT licence below.
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "engine/fsr_upscaler.hpp"

#include "engine/metal_util.hpp"
#include "engine/shader_cross.hpp"

#include <cmath>

namespace azl {

namespace {

// Changes from the reference: taps are integer reads with clamped coordinates instead of gathers; alpha is filtered
// with the colour's weights and colour is kept within it; RCAS passes alpha through and its limiters divide by a
// denominator kept off zero, since Metal compiles with fast math.
constexpr const char* kSource = R"(#version 450
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_samplerless_texture_functions : require

layout(set = 1, binding = 0) uniform texture2D t;
layout(location = 0) out vec4 fragColour;

float APrxLoRcpF1(float a) { return uintBitsToFloat(0x7ef07ebbu - floatBitsToUint(a)); }
float APrxMedRcpF1(float a) { float b = uintBitsToFloat(0x7ef19fffu - floatBitsToUint(a)); return b * (-b * a + 2.0); }
float APrxLoRsqF1(float a) { return uintBitsToFloat(0x5f347d74u - (floatBitsToUint(a) >> 1u)); }
float AMax3F1(float x, float y, float z) { return max(x, max(y, z)); }
float AMin3F1(float x, float y, float z) { return min(x, min(y, z)); }

vec4 tap(ivec2 p, ivec2 size) {
    return texelFetch(t, clamp(p, ivec2(0), size - 1), 0);
}

float luma2(vec4 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }

void FsrEasuTapF(inout vec4 aC, inout float aW, vec2 off, vec2 dir, vec2 len, float lob, float clp, vec4 c) {
    vec2 v;
    v.x = (off.x * (dir.x)) + (off.y * dir.y);
    v.y = (off.x * (-dir.y)) + (off.y * dir.x);
    v *= len;
    float d2 = v.x * v.x + v.y * v.y;
    d2 = min(d2, clp);
    float wB = (2.0 / 5.0) * d2 + -1.0;
    float wA = lob * d2 + -1.0;
    wB *= wB;
    wA *= wA;
    wB = (25.0 / 16.0) * wB + (-(25.0 / 16.0 - 1.0));
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

void FsrEasuSetF(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = APrxLoRcpF1(lenX);
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
    lenX *= lenX;
    len += lenX * w;
    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = APrxLoRcpF1(lenY);
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
    lenY *= lenY;
    len += lenY * w;
}

#ifdef ENTRY_easu
layout(set = 0, binding = 0, scalar) uniform OutSize { vec2 outSize; };

// 12-tap kernel around f:
//    b c
//  e f g h
//  i j k l
//    n o
void main() {
    ivec2 size = textureSize(t, 0);
    vec2 pp = gl_FragCoord.xy * (vec2(size) / outSize) - 0.5;
    vec2 fp = floor(pp);
    pp -= fp;
    ivec2 f0 = ivec2(fp);
    vec4 b = tap(f0 + ivec2(0, -1), size);
    vec4 c = tap(f0 + ivec2(1, -1), size);
    vec4 e = tap(f0 + ivec2(-1, 0), size);
    vec4 f = tap(f0, size);
    vec4 g = tap(f0 + ivec2(1, 0), size);
    vec4 h = tap(f0 + ivec2(2, 0), size);
    vec4 i = tap(f0 + ivec2(-1, 1), size);
    vec4 j = tap(f0 + ivec2(0, 1), size);
    vec4 k = tap(f0 + ivec2(1, 1), size);
    vec4 l = tap(f0 + ivec2(2, 1), size);
    vec4 n = tap(f0 + ivec2(0, 2), size);
    vec4 o = tap(f0 + ivec2(1, 2), size);
    float bL = luma2(b), cL = luma2(c), eL = luma2(e), fL = luma2(f), gL = luma2(g), hL = luma2(h);
    float iL = luma2(i), jL = luma2(j), kL = luma2(k), lL = luma2(l), nL = luma2(n), oL = luma2(o);

    vec2 dir = vec2(0.0);
    float len = 0.0;
    FsrEasuSetF(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    FsrEasuSetF(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    FsrEasuSetF(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    FsrEasuSetF(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);

    vec2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < 1.0 / 32768.0;
    dirR = APrxLoRsqF1(dirR);
    dirR = zro ? 1.0 : dirR;
    dir.x = zro ? 1.0 : dir.x;
    dir *= vec2(dirR);
    len = len * 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) * APrxLoRcpF1(max(abs(dir.x), abs(dir.y)));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 + -0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = APrxLoRcpF1(lob);

    vec4 min4 = min(min(f, min(g, j)), k);
    vec4 max4 = max(max(f, max(g, j)), k);
    vec4 aC = vec4(0.0);
    float aW = 0.0;
    FsrEasuTapF(aC, aW, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, b);
    FsrEasuTapF(aC, aW, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
    FsrEasuTapF(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, i);
    FsrEasuTapF(aC, aW, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
    FsrEasuTapF(aC, aW, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
    FsrEasuTapF(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
    FsrEasuTapF(aC, aW, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
    FsrEasuTapF(aC, aW, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
    FsrEasuTapF(aC, aW, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, h);
    FsrEasuTapF(aC, aW, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
    FsrEasuTapF(aC, aW, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, o);
    FsrEasuTapF(aC, aW, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, n);
    vec4 pix = min(max4, max(min4, aC * (1.0 / aW)));
    fragColour = vec4(min(pix.rgb, vec3(pix.a)), pix.a);
}
#endif

#ifdef ENTRY_rcas
layout(set = 0, binding = 0, scalar) uniform Sharpness { float sharpness; };

const float FSR_RCAS_LIMIT = 0.25 - (1.0 / 16.0);

//    b
//  d e f
//    h
void main() {
    ivec2 size = textureSize(t, 0);
    ivec2 sp = ivec2(gl_FragCoord.xy);
    vec3 b = tap(sp + ivec2(0, -1), size).rgb;
    vec3 d = tap(sp + ivec2(-1, 0), size).rgb;
    vec4 ee = tap(sp, size);
    vec3 e = ee.rgb;
    vec3 f = tap(sp + ivec2(1, 0), size).rgb;
    vec3 h = tap(sp + ivec2(0, 1), size).rgb;
    vec3 mn4 = min(min(b, min(d, f)), h);
    vec3 mx4 = max(max(b, max(d, f)), h);
    vec2 peakC = vec2(1.0, -1.0 * 4.0);
    vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1e-6));
    vec3 hitMax = (peakC.x - max(mx4, e)) / min(4.0 * mn4 + peakC.y, vec3(-1e-6));
    vec3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-FSR_RCAS_LIMIT, min(AMax3F1(lobeRGB.r, lobeRGB.g, lobeRGB.b), 0.0)) * sharpness;
    float rcpL = APrxMedRcpF1(4.0 * lobe + 1.0);
    vec3 pix = (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL;
    fragColour = vec4(clamp(pix, vec3(0.0), vec3(ee.a)), ee.a);
}
#endif
)";

} // namespace

FsrUpscaler::FsrUpscaler(MTL::Device* device, MTL::PixelFormat format)
{
    const auto library = makeLibrary(device, mslSource(fullscreenEntries(kSource, {"easu", "rcas"})).c_str());
    easu_ = makePipeline(device, library.get(), mslFunction("fullscreen").c_str(), mslFunction("easu").c_str(), format);
    rcas_ = makePipeline(device, library.get(), mslFunction("fullscreen").c_str(), mslFunction("rcas").c_str(), format);
}

void FsrUpscaler::encodeEasu(MTL::RenderCommandEncoder* enc, MTL::Texture* input, MTL::Texture* target) const
{
    const float size[2] = {float(target->width()), float(target->height())};
    enc->setRenderPipelineState(easu_.get());
    enc->setFragmentTexture(input, 0);
    enc->setFragmentBytes(size, sizeof(size), 0);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
}

void FsrUpscaler::encodeRcas(MTL::RenderCommandEncoder* enc, MTL::Texture* input) const
{
    const float sharpness = std::exp2(-kSharpness);
    enc->setRenderPipelineState(rcas_.get());
    enc->setFragmentTexture(input, 0);
    enc->setFragmentBytes(&sharpness, sizeof(sharpness), 0);
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
}

} // namespace azl
