// SPDX-License-Identifier: GPL-3.0-only
// Fluvi: light, edge-aware deblocking of H.264 4x4/8x8 transform block boundaries (decoded-surface grid: the
// image must not be cropped or scaled yet). Dispatch: ceil(w/8) x ceil(h/8).
//
// Per boundary line (p1 p0 | q0 q1, plus p2/q2 on 8-pixel boundaries) the excess step m = (q0-p0) - gradient,
// where gradient is the mean of the sides' own slopes, is spread over the nearby pixels as a ramp. Applied
// only where both sides are flat and the step is small (an alpha/beta pair that grows with strength), like the
// H.264 in-loop filter's gating, so real edges and fine detail are untouched. strength 0 = exact copy.
// Horizontal and vertical corrections are both computed from the unmodified input and summed.
Texture2D<float4> inTex : register(t0);
RWTexture2D<float4> outTex : register(u0);

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    float strength;   // 0..1
    uint pad0;
};

float3 At(int2 p) { return inTex[clamp(p, int2(0, 0), int2(width - 1, height - 1))].rgb; }
float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// Correction for pixel `pos` (coordinate c along `dir`) from the boundary whose first "q" pixel is at b.
float3 BoundaryDelta(int2 pos, int2 dir, int c, int b, int size, float alpha)
{
    if (b <= 0 || b >= size) return 0;
    const int rel = c - b;                   // >= 0: q_rel, < 0: p_(-rel-1)
    const bool big = (b & 7) == 0;           // 8x8 transform edge: wider spread
    float k;
    if (rel == -1) k = 0.5;
    else if (rel == -2) k = 0.25;
    else if (rel == -3 && big) k = 0.125;
    else if (rel == 0) k = -0.5;
    else if (rel == 1) k = -0.25;
    else if (rel == 2 && big) k = -0.125;
    else return 0;
    const int2 o = pos - dir * c;            // origin of the line
    const float3 p1 = At(o + dir * (b - 2));
    const float3 p0 = At(o + dir * (b - 1));
    const float3 q0 = At(o + dir * b);
    const float3 q1 = At(o + dir * (b + 1));
    const float3 m = (q0 - p0) - 0.5 * ((p0 - p1) + (q1 - q0));
    const float stepSize = abs(Luma(q0 - p0));
    const float side = max(abs(Luma(p0 - p1)), abs(Luma(q1 - q0)));
    const float beta = 0.7 * alpha;
    const float w = (1.0 - smoothstep(0.5 * alpha, alpha, stepSize)) * (1.0 - smoothstep(0.5 * beta, beta, side));
    return m * (w * k * strength);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height) return;
    const float4 src = inTex[id.xy];
    if (strength <= 0.0) { outTex[id.xy] = src; return; }
    const int2 p = int2(id.xy);
    const float alpha = 0.03 + 0.09 * saturate(strength);   // gamma-space luma units (about 8..31 of 255)
    float3 delta = 0;
    const int bx = p.x & ~3, by = p.y & ~3;
    delta += BoundaryDelta(p, int2(1, 0), p.x, bx, int(width), alpha);
    delta += BoundaryDelta(p, int2(1, 0), p.x, bx + 4, int(width), alpha);
    delta += BoundaryDelta(p, int2(0, 1), p.y, by, int(height), alpha);
    delta += BoundaryDelta(p, int2(0, 1), p.y, by + 4, int(height), alpha);
    outTex[id.xy] = float4(src.rgb + delta, src.a);
}
