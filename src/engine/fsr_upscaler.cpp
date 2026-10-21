// The shader source in this file is ported to Metal from AMD FidelityFX Super Resolution 1 (ffx_fsr1.h and ffx_a.h,
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

#include <cmath>

namespace azl {

namespace {

// Changes from the reference: taps are integer reads with clamped coordinates instead of gathers; alpha is filtered
// with the colour's weights and colour is kept within it; RCAS passes alpha through and its limiters divide by a
// denominator kept off zero, since Metal compiles with fast math.
constexpr const char* kSource = R"(
#include <metal_stdlib>
using namespace metal;

struct VOut { float4 position [[position]]; };

vertex VOut fullscreen(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    VOut o;
    o.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

float APrxLoRcpF1(float a) { return as_type<float>(0x7ef07ebbu - as_type<uint>(a)); }
float APrxMedRcpF1(float a) { float b = as_type<float>(0x7ef19fffu - as_type<uint>(a)); return b * (-b * a + 2.0); }
float APrxLoRsqF1(float a) { return as_type<float>(0x5f347d74u - (as_type<uint>(a) >> 1u)); }
float AMax3F1(float x, float y, float z) { return max(x, max(y, z)); }
float AMin3F1(float x, float y, float z) { return min(x, min(y, z)); }

float4 tap(texture2d<float> t, int2 p, int2 size) {
    return t.read(uint2(clamp(p, int2(0), size - 1)));
}

float luma2(float4 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }

void FsrEasuTapF(thread float4& aC, thread float& aW, float2 off, float2 dir, float2 len, float lob, float clp,
                 float4 c) {
    float2 v;
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

void FsrEasuSetF(thread float2& dir, thread float& len, float w, float lA, float lB, float lC, float lD, float lE) {
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = APrxLoRcpF1(lenX);
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = saturate(abs(dirX) * lenX);
    lenX *= lenX;
    len += lenX * w;
    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = APrxLoRcpF1(lenY);
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = saturate(abs(dirY) * lenY);
    lenY *= lenY;
    len += lenY * w;
}

// 12-tap kernel around f:
//    b c
//  e f g h
//  i j k l
//    n o
fragment float4 easu(VOut in [[stage_in]], texture2d<float> t [[texture(0)]], constant float2& outSize [[buffer(0)]]) {
    int2 size = int2(t.get_width(), t.get_height());
    float2 pp = in.position.xy * (float2(size) / outSize) - 0.5;
    float2 fp = floor(pp);
    pp -= fp;
    int2 f0 = int2(fp);
    float4 b = tap(t, f0 + int2(0, -1), size);
    float4 c = tap(t, f0 + int2(1, -1), size);
    float4 e = tap(t, f0 + int2(-1, 0), size);
    float4 f = tap(t, f0, size);
    float4 g = tap(t, f0 + int2(1, 0), size);
    float4 h = tap(t, f0 + int2(2, 0), size);
    float4 i = tap(t, f0 + int2(-1, 1), size);
    float4 j = tap(t, f0 + int2(0, 1), size);
    float4 k = tap(t, f0 + int2(1, 1), size);
    float4 l = tap(t, f0 + int2(2, 1), size);
    float4 n = tap(t, f0 + int2(0, 2), size);
    float4 o = tap(t, f0 + int2(1, 2), size);
    float bL = luma2(b), cL = luma2(c), eL = luma2(e), fL = luma2(f), gL = luma2(g), hL = luma2(h);
    float iL = luma2(i), jL = luma2(j), kL = luma2(k), lL = luma2(l), nL = luma2(n), oL = luma2(o);

    float2 dir = float2(0.0);
    float len = 0.0;
    FsrEasuSetF(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    FsrEasuSetF(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    FsrEasuSetF(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    FsrEasuSetF(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);

    float2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < 1.0 / 32768.0;
    dirR = APrxLoRsqF1(dirR);
    dirR = zro ? 1.0 : dirR;
    dir.x = zro ? 1.0 : dir.x;
    dir *= float2(dirR);
    len = len * 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) * APrxLoRcpF1(max(abs(dir.x), abs(dir.y)));
    float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 + -0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = APrxLoRcpF1(lob);

    float4 min4 = min(min(f, min(g, j)), k);
    float4 max4 = max(max(f, max(g, j)), k);
    float4 aC = float4(0.0);
    float aW = 0.0;
    FsrEasuTapF(aC, aW, float2(0.0, -1.0) - pp, dir, len2, lob, clp, b);
    FsrEasuTapF(aC, aW, float2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
    FsrEasuTapF(aC, aW, float2(-1.0, 1.0) - pp, dir, len2, lob, clp, i);
    FsrEasuTapF(aC, aW, float2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
    FsrEasuTapF(aC, aW, float2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
    FsrEasuTapF(aC, aW, float2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
    FsrEasuTapF(aC, aW, float2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
    FsrEasuTapF(aC, aW, float2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
    FsrEasuTapF(aC, aW, float2(2.0, 0.0) - pp, dir, len2, lob, clp, h);
    FsrEasuTapF(aC, aW, float2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
    FsrEasuTapF(aC, aW, float2(1.0, 2.0) - pp, dir, len2, lob, clp, o);
    FsrEasuTapF(aC, aW, float2(0.0, 2.0) - pp, dir, len2, lob, clp, n);
    float4 pix = min(max4, max(min4, aC * (1.0 / aW)));
    return float4(min(pix.rgb, float3(pix.a)), pix.a);
}

constant float FSR_RCAS_LIMIT = 0.25 - (1.0 / 16.0);

//    b
//  d e f
//    h
fragment float4 rcas(VOut in [[stage_in]], texture2d<float> t [[texture(0)]], constant float& sharpness [[buffer(0)]]) {
    int2 size = int2(t.get_width(), t.get_height());
    int2 sp = int2(in.position.xy);
    float3 b = tap(t, sp + int2(0, -1), size).rgb;
    float3 d = tap(t, sp + int2(-1, 0), size).rgb;
    float4 ee = tap(t, sp, size);
    float3 e = ee.rgb;
    float3 f = tap(t, sp + int2(1, 0), size).rgb;
    float3 h = tap(t, sp + int2(0, 1), size).rgb;
    float3 mn4 = min(min(b, min(d, f)), h);
    float3 mx4 = max(max(b, max(d, f)), h);
    float2 peakC = float2(1.0, -1.0 * 4.0);
    float3 hitMin = min(mn4, e) / max(4.0 * mx4, float3(1e-6));
    float3 hitMax = (peakC.x - max(mx4, e)) / min(4.0 * mn4 + peakC.y, float3(-1e-6));
    float3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-FSR_RCAS_LIMIT, min(AMax3F1(lobeRGB.r, lobeRGB.g, lobeRGB.b), 0.0)) * sharpness;
    float rcpL = APrxMedRcpF1(4.0 * lobe + 1.0);
    float3 pix = (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL;
    return float4(clamp(pix, float3(0.0), float3(ee.a)), ee.a);
}
)";

} // namespace

FsrUpscaler::FsrUpscaler(MTL::Device* device, MTL::PixelFormat format)
{
    const auto library = makeLibrary(device, kSource);
    easu_ = makePipeline(device, library.get(), "fullscreen", "easu", format);
    rcas_ = makePipeline(device, library.get(), "fullscreen", "rcas", format);
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
