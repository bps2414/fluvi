// SPDX-License-Identifier: GPL-3.0-only
// Fluvi: FidelityFX FSR 1 RCAS sharpener (32-bit path, noise-aware variant). Algorithm and ffx_fsr1.h are
// AMD's, MIT licensed (Copyright (C) 2026 Advanced Micro Devices, Inc.); included from third_party/fidelityfx.
// Input is width x height, video gamma. lobeScale 0 = exact copy. The image lands in the output at (offX, offY);
// output pixels outside it become opaque black (letterbox/pillarbox bars: the picture is never stretched).
// Dispatch: ceil(outW/8) x ceil(outH/8).
#define FFX_GPU 1
#define FFX_HLSL 1
#include "ffx_core.h"

Texture2D<float4> inTex : register(t0);
RWTexture2D<float4> outTex : register(u0);

cbuffer Constants : register(b0)
{
    uint width;        // input (= image) size
    uint height;
    float lobeScale;   // exp2(-stops) of RCAS: 1 = maximum sharpening, 0 = none
    uint pad0;
    int offX;          // where the image sits in the output
    int offY;
    uint outW;         // output size
    uint outH;
};

#define FSR_RCAS_F 1
#define FSR_RCAS_DENOISE 1
FfxFloat32x4 FsrRcasLoadF(FfxInt32x2 p) { return inTex[clamp(p, int2(0, 0), int2(width - 1, height - 1))]; }
void FsrRcasInputF(inout FfxFloat32 r, inout FfxFloat32 g, inout FfxFloat32 b) {}
#include "ffx_fsr1.h"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= outW || id.y >= outH) return;
    const int2 p = int2(id.xy) - int2(offX, offY);
    if (p.x < 0 || p.y < 0 || p.x >= (int)width || p.y >= (int)height) { outTex[id.xy] = float4(0, 0, 0, 1); return; }
    const uint2 q = uint2(p);
    const float4 src = inTex[q];
    if (lobeScale <= 0.0) { outTex[id.xy] = src; return; }
    FfxUInt32x4 con = FfxUInt32x4(asuint(lobeScale), 0, 0, 0);
    FfxFloat32 r, g, b;
    FsrRcasF(r, g, b, q, con);
    float3 rgb = float3(r, g, b);
    if (any(!isfinite(rgb))) rgb = src.rgb;   // black neighbourhoods divide by zero in the limiter
    outTex[id.xy] = float4(rgb, src.a);
}
