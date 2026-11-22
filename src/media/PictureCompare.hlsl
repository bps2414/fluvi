// SPDX-License-Identifier: GPL-3.0-only
// Fluvi media: did the picture change? Luma of the new picture vs the previous one, one 16x16 tile per group.
// result[0] += tiles with >= changedPixels samples moved by >= pixelDelta; result[1] = max |dY| (0..255).
// Dispatch: ceil(width/16) x ceil(height/16).
#define RS "RootConstants(num32BitConstants=4, b0), DescriptorTable(SRV(t0)), DescriptorTable(SRV(t1)), UAV(u0)"

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint pixelDelta;
    uint changedPixels;
};

Texture2D<float> curLuma : register(t0);
Texture2D<float> prevLuma : register(t1);
RWByteAddressBuffer result : register(u0);

groupshared uint gCount;
groupshared uint gMax;

[RootSignature(RS)]
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    if (gi == 0) { gCount = 0; gMax = 0; }
    GroupMemoryBarrierWithGroupSync();
    uint d = 0;
    if (id.x < width && id.y < height)
        d = (uint)round(abs(curLuma.Load(int3(id.xy, 0)) - prevLuma.Load(int3(id.xy, 0))) * 255.0);
    if (d >= pixelDelta) InterlockedAdd(gCount, 1);
    InterlockedMax(gMax, d);
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0) {
        if (gCount >= changedPixels) result.InterlockedAdd(0, 1);
        uint ignored;
        result.InterlockedMax(4, gMax, ignored);
    }
}
