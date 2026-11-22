// SPDX-License-Identifier: GPL-3.0-only
// Fluvi pipeline tests: procedural images, half/8-bit conversion, CPU reference implementations and metrics.
#pragma once
#include <cstdint>
#include <vector>
#include "fluvi/gpu/Passes.h"
#include "TestGpu.h"

namespace fluvi::test {

struct Image {
    uint32_t w = 0, h = 0;
    std::vector<float> px;   // RGBA, row-major, float
    Image() = default;
    Image(uint32_t w_, uint32_t h_) : w(w_), h(h_), px(static_cast<size_t>(w_) * h_ * 4, 1.f) {}
    float& at(uint32_t x, uint32_t y, int c) { return px[(static_cast<size_t>(y) * w + x) * 4 + c]; }
    float at(uint32_t x, uint32_t y, int c) const { return px[(static_cast<size_t>(y) * w + x) * 4 + c]; }
    float luma(uint32_t x, uint32_t y) const { return 0.2126f * at(x, y, 0) + 0.7152f * at(x, y, 1) + 0.0722f * at(x, y, 2); }
};

// ---- procedural content (values 0..1, alpha 1) ----
Image Gradient(uint32_t w, uint32_t h);
Image ZonePlate(uint32_t w, uint32_t h);
Image TextEdges(uint32_t w, uint32_t h);                       // hard 1-2 px strokes, high contrast
Image BlockyGradient(uint32_t w, uint32_t h, float amplitude); // smooth base + DC offsets per 8x8 and 4x4 block
Image Noise(uint32_t w, uint32_t h, uint32_t seed, uint32_t cell);   // smooth value noise, textured
Image Constant(uint32_t w, uint32_t h, float r, float g, float b);
Image StepEdge(uint32_t w, uint32_t h);                        // left half 0, right half 1

// ---- GPU round trips ----
Tex UploadImage(TestGpu& gpu, const Image& img, DXGI_FORMAT format, bool uav);   // RGBA16F or RGBA8 (state SRV)
Image Download(TestGpu& gpu, Tex& t);                           // RGBA16F / RGBA8 -> float
float HalfRound(float v);                                       // value after a trip through half
Image HalfRounded(const Image& img);

// ---- NV12 ----
struct Nv12Frame {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> y, uv;   // tight pitches: y = w*h, uv = w * h/2 (interleaved Cb,Cr)
};
// Procedural planes (not derived from RGB): exercises every channel and the chroma siting.
Nv12Frame MakeNv12(uint32_t w, uint32_t h);
// Converts an RGB image with the given matrix (box-filtered 2x2 chroma, left siting) - for the bench.
Nv12Frame RgbToNv12(const Image& rgb, gpu::YuvMatrix matrix, bool fullRange);
// CPU reference of Nv12ToRgbPass in double precision, result rounded to 8 bits per channel (RGBA, w*h*4).
std::vector<uint8_t> ReferenceNv12ToRgb8(const Nv12Frame& f, const gpu::Nv12Params& p, uint32_t visW, uint32_t visH);

// ---- references and metrics ----
Image ReferenceDownscale(const Image& in, uint32_t outW, uint32_t outH, gpu::DownscaleFilter filter);
double Psnr(const Image& a, const Image& b);                    // RGB, peak 1.0
double MaxAbsDiff(const Image& a, const Image& b);              // RGB
// Mean |luma step| over horizontal and vertical transitions that straddle a 4-pixel boundary minus the mean over
// the others: 0 for a smooth image, large for visible block seams.
double Blockiness(const Image& img);
double MaxGradient(const Image& img);                           // max |luma step| between neighbours

} // namespace fluvi::test
