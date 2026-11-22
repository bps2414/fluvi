// SPDX-License-Identifier: GPL-3.0-only
// Fluvi: high-quality single-pass downscale (Lanczos3 / Catmull-Rom bicubic / exact-area box). Any ratio;
// ratios below 1 act as a plain resampler of the same kernel. Dispatch: ceil(outW/8) x ceil(outH/8).
//
// Kernel support grows with the reduction ratio (anti-aliased). Edge pixels replicate. Weights are
// normalised, so DC gain is 1. Ringing is limited by clamping toward the min/max of the four nearest source
// texels, blended by `antiring` (0 = off). Source values stay in the video's gamma.
Texture2D<float4> inTex : register(t0);
RWTexture2D<float4> outTex : register(u0);

cbuffer Constants : register(b0)
{
    uint inW;
    uint inH;
    uint outW;
    uint outH;
    uint filterId;    // 0 Lanczos3, 1 Bicubic (Catmull-Rom), 2 Box
    float antiring;   // 0..1
    uint pad0;
    uint pad1;
};

static const float kPi = 3.14159265358979;
static const int kMaxTaps = 48;   // per axis, bounds the loop for extreme ratios

float Lanczos3(float x)
{
    x = abs(x);
    if (x < 1e-4) return 1.0;
    if (x >= 3.0) return 0.0;
    const float px = kPi * x;
    return 3.0 * sin(px) * sin(px * (1.0 / 3.0)) / (px * px);
}

float CatmullRom(float x)
{
    x = abs(x);
    if (x < 1.0) return (1.5 * x - 2.5) * x * x + 1.0;
    if (x < 2.0) return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
    return 0.0;
}

// x: distance in source pixels, stretch: kernel stretch (>= 1)
float Weight(float x, float stretch)
{
    if (filterId == 0) return Lanczos3(x / stretch);
    if (filterId == 1) return CatmullRom(x / stretch);
    return saturate(0.5 * stretch + 0.5 - abs(x));   // box: exact pixel coverage
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= outW || id.y >= outH) return;
    const float2 scale = float2(inW, inH) / float2(outW, outH);
    const float2 stretch = max(scale, 1.0);
    const float radius = filterId == 0 ? 3.0 : (filterId == 1 ? 2.0 : 0.5);
    const float2 center = (float2(id.xy) + 0.5) * scale - 0.5;    // in source pixel index space
    const float2 reach = radius * stretch + (filterId == 2 ? 0.5 : 0.0);
    const int2 lo = int2(ceil(center - reach));
    const int2 hi = int2(floor(center + reach));
    const int2 maxIdx = int2(inW - 1, inH - 1);
    const int nx = min(hi.x - lo.x + 1, kMaxTaps), ny = min(hi.y - lo.y + 1, kMaxTaps);

    float4 sum = 0;
    float wsum = 0;
    for (int j = 0; j < ny; ++j) {
        const int sy = lo.y + j;
        const float wy = Weight(float(sy) - center.y, stretch.y);
        if (wy == 0.0) continue;
        const int cy = clamp(sy, 0, maxIdx.y);
        for (int i = 0; i < nx; ++i) {
            const int sx = lo.x + i;
            const float w = wy * Weight(float(sx) - center.x, stretch.x);
            sum += w * inTex[int2(clamp(sx, 0, maxIdx.x), cy)];
            wsum += w;
        }
    }
    float4 result = sum / wsum;
    if (antiring > 0.0) {
        const int2 f = int2(floor(center));
        const float4 a = inTex[clamp(f, int2(0, 0), maxIdx)];
        const float4 b = inTex[clamp(f + int2(1, 0), int2(0, 0), maxIdx)];
        const float4 c = inTex[clamp(f + int2(0, 1), int2(0, 0), maxIdx)];
        const float4 d = inTex[clamp(f + int2(1, 1), int2(0, 0), maxIdx)];
        const float4 lowV = min(min(a, b), min(c, d));
        const float4 highV = max(max(a, b), max(c, d));
        result = lerp(result, clamp(result, lowV, highV), antiring);
    }
    outTex[id.xy] = result;
}
