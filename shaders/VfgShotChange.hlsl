// SPDX-License-Identifier: GPL-3.0-only
// Ported from Veyra (GPL-3.0-only), shaders/VfgShotChange.hlsl @ c6633121. Changes: the two frames are read from
// textures (Fluvi frames are textures, not raw buffers); 8-bit luma is derived from the quantised texel so the
// measures stay identical to the CPU path.
//
// Shot-change test for one frame pair (frame generation). One group of 256 threads.
//
// Same measures and thresholds as the CPU SceneCadenceAnalyzer: 64x36 luma samples, 256-bin histogram distance
// (L1 / 2) and normalised SAD; a flash (large SAD, near-identical histogram) is not a cut. No CPU readback.
//
// results[parity] (64-bit, offset parity*8) = 1 when the pair crosses a cut.
// Diagnostics: offset 16 + parity*16: {asuint(sad), asuint(histogram), compare}; offset 48: running cut count.

cbuffer Constants : register(b0)
{
    uint width;      // size of both textures (any resolution)
    uint height;
    uint parity;     // which result slot to write: 0 or 1
    uint compare;    // 0 when the pair has no valid previous frame (reset): never a cut
    float cutSad;    // sceneCutSadThreshold
    float cutHistogram;
    float replacementSad;
    float replacementHistogram;
    float flashSad;
    float flashHistogram;
    uint unused0;
    uint unused1;
};

Texture2D<float4> previousFrame : register(t0);
Texture2D<float4> currentFrame : register(t1);
RWByteAddressBuffer flags : register(u0);

static const uint kColumns = 64;
static const uint kRows = 36;
static const uint kSamples = kColumns * kRows;

groupshared uint histogramPrevious[256];
groupshared uint histogramCurrent[256];
groupshared uint sadSum;
groupshared uint histogramL1;

// Integer luma weights of the CPU sampler: (54R+183G+19B+128)>>8 on 8-bit values.
uint lumaAt(Texture2D<float4> frame, uint x, uint y)
{
    const uint3 c = uint3(saturate(frame[uint2(x, y)].rgb) * 255.0 + 0.5);
    return (54u * c.r + 183u * c.g + 19u * c.b + 128u) >> 8;
}

[numthreads(256, 1, 1)]
void main(uint index : SV_GroupIndex)
{
    histogramPrevious[index] = 0;
    histogramCurrent[index] = 0;
    if (index == 0) {
        sadSum = 0;
        histogramL1 = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    if (compare != 0) {
        for (uint i = index; i < kSamples; i += 256) {
            const uint x = (i % kColumns) * width / kColumns;
            const uint y = (i / kColumns) * height / kRows;
            const uint a = lumaAt(previousFrame, x, y);
            const uint b = lumaAt(currentFrame, x, y);
            InterlockedAdd(histogramPrevious[a], 1);
            InterlockedAdd(histogramCurrent[b], 1);
            InterlockedAdd(sadSum, a > b ? a - b : b - a);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    const uint p = histogramPrevious[index], c = histogramCurrent[index];
    InterlockedAdd(histogramL1, p > c ? p - c : c - p);
    GroupMemoryBarrierWithGroupSync();
    if (index == 0) {
        const float sad = float(sadSum) / (255.0 * kSamples);
        const float histogram = float(histogramL1) / (2.0 * kSamples);
        const bool flash = sad > flashSad && histogram < flashHistogram;
        const bool cut = compare != 0 && !flash &&
            ((sad > cutSad && histogram > cutHistogram) ||
             (sad > replacementSad && histogram > replacementHistogram));
        flags.Store2(parity * 8, uint2(cut ? 1u : 0u, 0u));
        flags.Store3(16 + parity * 16, uint3(asuint(sad), asuint(histogram), compare));
        if (cut) {
            uint previousCount;
            flags.InterlockedAdd(48, 1, previousCount);
        }
    }
}
