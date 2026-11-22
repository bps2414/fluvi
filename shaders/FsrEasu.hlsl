// SPDX-License-Identifier: GPL-3.0-only
// Fluvi: FidelityFX FSR 1 EASU upscaler (32-bit path). Algorithm and ffx_fsr1.h are AMD's, MIT licensed
// (Copyright (C) 2026 Advanced Micro Devices, Inc.); included from third_party/fidelityfx, notice kept there.
// Input and output keep the video's gamma (EASU wants perceptual space). Dispatch: ceil(outW/8) x ceil(outH/8).
#define FFX_GPU 1
#define FFX_HLSL 1
#include "ffx_core.h"

Texture2D<float4> inTex : register(t0);
RWTexture2D<float4> outTex : register(u0);
SamplerState samp : register(s0);   // linear clamp (gather ignores the filter)

cbuffer Constants : register(b0)
{
    uint inW;
    uint inH;
    uint outW;
    uint outH;
};

#define FFX_FSR_EASU_FLOAT 1
FfxFloat32x4 FsrEasuRF(FfxFloat32x2 p) { return inTex.GatherRed(samp, p); }
FfxFloat32x4 FsrEasuGF(FfxFloat32x2 p) { return inTex.GatherGreen(samp, p); }
FfxFloat32x4 FsrEasuBF(FfxFloat32x2 p) { return inTex.GatherBlue(samp, p); }
#include "ffx_fsr1.h"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= outW || id.y >= outH) return;
    FfxUInt32x4 con0, con1, con2, con3;
    ffxFsrPopulateEasuConstants(con0, con1, con2, con3, FfxFloat32(inW), FfxFloat32(inH), FfxFloat32(inW), FfxFloat32(inH),
                                FfxFloat32(outW), FfxFloat32(outH));
    FfxFloat32x3 color;
    ffxFsrEasuFloat(color, id.xy, con0, con1, con2, con3);
    outTex[id.xy] = float4(saturate(color), 1.0);
}
