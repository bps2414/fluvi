// SPDX-License-Identifier: GPL-3.0-only
// Fluvi: NV12 -> RGB (BT.709/601, limited/full) with H.264 chroma siting and bilinear chroma upsampling.
// Output keeps the video's gamma (no transfer conversion). Dispatch: ceil(visW/8) x ceil(visH/8).
//
// Chroma siting (MPEG-2/H.264 "left", vertically centred): chroma sample i sits on luma column 2i, chroma
// sample j sits between luma rows 2j and 2j+1. Upsampling is a manual bilinear (full float precision, no
// fixed-point sampler weights) clamped to the visible chroma area.
Texture2D<float> lumaTex : register(t0);      // plane 0, R8_UNORM
Texture2D<float2> chromaTex : register(t1);   // plane 1, R8G8_UNORM (x = Cb, y = Cr)
RWTexture2D<float4> outTex : register(u0);    // RGBA16F or RGBA8

cbuffer Constants : register(b0)
{
    uint visW;        // visible size (crop of the decoded surface, top-left anchored)
    uint visH;
    uint pad0;
    uint pad1;
    float yOff;       // code values are 0..1 (R8_UNORM): limited 16/255, full 0
    float yScale;     // limited 255/219, full 1
    float cOff;       // 128/255
    float cScale;     // limited 255/224, full 1
    float crR;        // R = Y + crR * Cr
    float cbB;        // B = Y + cbB * Cb
    float crG;        // G = Y - cbG * Cb - crG * Cr
    float cbG;
};

float2 Chroma(int2 c, int2 limit) { return chromaTex[clamp(c, int2(0, 0), limit)]; }

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= visW || id.y >= visH) return;
    const int2 chromaLimit = int2((visW + 1) / 2 - 1, (visH + 1) / 2 - 1);
    const float2 cp = float2(id.x * 0.5, (float(id.y) - 0.5) * 0.5);
    const float2 base = floor(cp);
    const float2 f = cp - base;
    const int2 i0 = int2(base);
    const float2 c00 = Chroma(i0, chromaLimit);
    const float2 c10 = Chroma(i0 + int2(1, 0), chromaLimit);
    const float2 c01 = Chroma(i0 + int2(0, 1), chromaLimit);
    const float2 c11 = Chroma(i0 + int2(1, 1), chromaLimit);
    const float2 cbcr = lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);
    const float y = (lumaTex[id.xy] - yOff) * yScale;
    const float cb = (cbcr.x - cOff) * cScale;
    const float cr = (cbcr.y - cOff) * cScale;
    const float3 rgb = float3(y + crR * cr, y - cbG * cb - crG * cr, y + cbB * cb);
    outTex[id.xy] = float4(saturate(rgb), 1.0);
}
