// SPDX-License-Identifier: GPL-3.0-only
#include "Images.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <DirectXPackedVector.h>

namespace fluvi::test {
using gpu::DownscaleFilter;

namespace {
uint32_t Hash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
float Rand01(uint32_t x) { return static_cast<float>(Hash(x) & 0xFFFFFF) / static_cast<float>(0x1000000); }
void Set(Image& img, uint32_t x, uint32_t y, float r, float g, float b) {
    img.at(x, y, 0) = r; img.at(x, y, 1) = g; img.at(x, y, 2) = b; img.at(x, y, 3) = 1.f;
}
float Clamp01(double v) { return static_cast<float>(std::clamp(v, 0.0, 1.0)); }
} // namespace

Image Gradient(uint32_t w, uint32_t h) {
    Image img(w, h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            Set(img, x, y, 0.1f + 0.8f * x / (w - 1), 0.1f + 0.8f * y / (h - 1), 0.1f + 0.8f * (x + y) / static_cast<float>(w + h - 2));
    return img;
}

Image ZonePlate(uint32_t w, uint32_t h) {
    Image img(w, h);
    const double rMax = std::sqrt(double(w) * w + double(h) * h) / 2.0;
    const double k = std::numbers::pi * 0.5 / rMax;   // local frequency reaches Nyquist at the corners
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const double dx = x - w / 2.0, dy = y - h / 2.0;
            const float v = static_cast<float>(0.5 + 0.5 * std::cos(k * (dx * dx + dy * dy)));
            Set(img, x, y, v, v, v);
        }
    return img;
}

Image TextEdges(uint32_t w, uint32_t h) {
    Image img(w, h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            // 10x14 cells, each with a hash-selected set of 1-2 px strokes.
            const uint32_t cx = x / 10, cy = y / 14, lx = x % 10, ly = y % 14;
            const uint32_t bits = Hash(cx * 7919u + cy * 104729u + 17u);
            bool on = false;
            if (lx >= 1 && lx <= 7 && ly >= 1 && ly <= 11) {
                on |= (bits & 1) && lx == 1;
                on |= (bits & 2) && lx == 6;
                on |= (bits & 4) && (ly == 1 || ly == 2);
                on |= (bits & 8) && ly == 6;
                on |= (bits & 16) && (ly == 11);
                on |= (bits & 32) && lx == ly - 3;
            }
            const float v = on ? 0.92f : 0.12f;
            Set(img, x, y, v, v, v);
        }
    return img;
}

Image BlockyGradient(uint32_t w, uint32_t h, float amplitude) {
    Image img = Gradient(w, h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const float off8 = (Rand01((x / 8) * 31u + (y / 8) * 977u + 5u) - 0.5f) * 2.f * amplitude;
            const float off4 = (Rand01((x / 4) * 131u + (y / 4) * 4091u + 9u) - 0.5f) * 2.f * amplitude * 0.5f;
            for (int c = 0; c < 3; ++c) img.at(x, y, c) = Clamp01(img.at(x, y, c) + off8 + off4);
        }
    return img;
}

Image Noise(uint32_t w, uint32_t h, uint32_t seed, uint32_t cell) {
    Image img(w, h);
    auto lattice = [&](int cx, int cy, int c) { return Rand01(static_cast<uint32_t>(cx) * 73856093u ^ static_cast<uint32_t>(cy) * 19349663u ^ (seed + c) * 83492791u); };
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const float fx = float(x) / cell, fy = float(y) / cell;
            const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
            const float tx = fx - ix, ty = fy - iy;
            const float sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty);
            float rgb[3];
            for (int c = 0; c < 3; ++c) {
                const float a = lattice(ix, iy, c), b = lattice(ix + 1, iy, c), d = lattice(ix, iy + 1, c), e = lattice(ix + 1, iy + 1, c);
                rgb[c] = (a + (b - a) * sx) + ((d + (e - d) * sx) - (a + (b - a) * sx)) * sy;
            }
            Set(img, x, y, 0.1f + 0.8f * rgb[0], 0.1f + 0.8f * rgb[1], 0.1f + 0.8f * rgb[2]);
        }
    return img;
}

Image Constant(uint32_t w, uint32_t h, float r, float g, float b) {
    Image img(w, h);
    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) Set(img, x, y, r, g, b);
    return img;
}

Image StepEdge(uint32_t w, uint32_t h) {
    Image img(w, h);
    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) { const float v = x < w / 2 ? 0.f : 1.f; Set(img, x, y, v, v, v); }
    return img;
}

// ---- GPU round trips --------------------------------------------------------------------------------------
float HalfRound(float v) {
    return DirectX::PackedVector::XMConvertHalfToFloat(DirectX::PackedVector::XMConvertFloatToHalf(v));
}

Image HalfRounded(const Image& img) {
    Image o = img;
    for (auto& v : o.px) v = HalfRound(v);
    return o;
}

Tex UploadImage(TestGpu& gpu, const Image& img, DXGI_FORMAT format, bool uav) {
    Tex t = gpu.Create(img.w, img.h, format, uav);
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        std::vector<uint16_t> data(img.px.size());
        for (size_t i = 0; i < data.size(); ++i) data[i] = DirectX::PackedVector::XMConvertFloatToHalf(img.px[i]);
        gpu.Upload(t, data.data(), size_t(img.w) * 8);
    } else {
        std::vector<uint8_t> data(img.px.size());
        for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(std::lround(std::clamp(img.px[i], 0.f, 1.f) * 255.f));
        gpu.Upload(t, data.data(), size_t(img.w) * 4);
    }
    return t;
}

Image Download(TestGpu& gpu, Tex& t) {
    const auto bytes = gpu.Readback(t);
    Image img(t.w, t.h);
    if (t.format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        const auto* h = reinterpret_cast<const uint16_t*>(bytes.data());
        for (size_t i = 0; i < img.px.size(); ++i) img.px[i] = DirectX::PackedVector::XMConvertHalfToFloat(h[i]);
    } else {
        for (size_t i = 0; i < img.px.size(); ++i) img.px[i] = bytes[i] / 255.f;
    }
    return img;
}

// ---- NV12 -------------------------------------------------------------------------------------------------
Nv12Frame MakeNv12(uint32_t w, uint32_t h) {
    Nv12Frame f;
    f.w = w;
    f.h = h;
    f.y.resize(size_t(w) * h);
    f.uv.resize(size_t(w) * (h / 2));
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const double v = 16.0 + 219.0 * (0.5 + 0.25 * std::sin(x * 0.11) + 0.25 * std::cos(y * 0.07 + x * 0.013));
            f.y[size_t(y) * w + x] = static_cast<uint8_t>(std::lround(std::clamp(v + (Rand01(x * 3571u + y * 12923u) - 0.5) * 6.0, 0.0, 255.0)));
        }
    for (uint32_t y = 0; y < h / 2; ++y)
        for (uint32_t x = 0; x < w / 2; ++x) {
            // Full chroma range with sharp transitions so a wrong siting or swapped channel shows up.
            const double cb = 128.0 + 100.0 * std::sin(x * 0.19 + y * 0.05) + ((x / 6 + y / 5) % 2 ? 15.0 : -15.0);
            const double cr = 128.0 + 100.0 * std::cos(y * 0.17 - x * 0.03) + ((x / 7) % 2 ? 12.0 : -12.0);
            f.uv[size_t(y) * w + x * 2 + 0] = static_cast<uint8_t>(std::lround(std::clamp(cb, 16.0, 240.0)));
            f.uv[size_t(y) * w + x * 2 + 1] = static_cast<uint8_t>(std::lround(std::clamp(cr, 16.0, 240.0)));
        }
    return f;
}

Nv12Frame RgbToNv12(const Image& rgb, gpu::YuvMatrix matrix, bool fullRange) {
    const bool bt709 = matrix == gpu::YuvMatrix::Bt709;
    const double kr = bt709 ? 0.2126 : 0.299, kb = bt709 ? 0.0722 : 0.114, kg = 1.0 - kr - kb;
    Nv12Frame f;
    f.w = rgb.w & ~1u;
    f.h = rgb.h & ~1u;
    f.y.resize(size_t(f.w) * f.h);
    f.uv.resize(size_t(f.w) * (f.h / 2));
    std::vector<double> cbFull(size_t(f.w) * f.h), crFull(size_t(f.w) * f.h);
    for (uint32_t y = 0; y < f.h; ++y)
        for (uint32_t x = 0; x < f.w; ++x) {
            const double r = rgb.at(x, y, 0), g = rgb.at(x, y, 1), b = rgb.at(x, y, 2);
            const double yy = kr * r + kg * g + kb * b;
            const double cb = (b - yy) / (2 * (1 - kb)), cr = (r - yy) / (2 * (1 - kr));
            const double yc = fullRange ? yy * 255.0 : 16.0 + 219.0 * yy;
            f.y[size_t(y) * f.w + x] = static_cast<uint8_t>(std::lround(std::clamp(yc, 0.0, 255.0)));
            const double scale = fullRange ? 255.0 : 224.0;
            cbFull[size_t(y) * f.w + x] = 128.0 + scale * cb;
            crFull[size_t(y) * f.w + x] = 128.0 + scale * cr;
        }
    for (uint32_t y = 0; y < f.h / 2; ++y)
        for (uint32_t x = 0; x < f.w / 2; ++x) {
            // Left siting: average of the 2 vertically adjacent pixels of luma column 2x (centred vertically).
            const size_t i0 = size_t(2 * y) * f.w + 2 * x, i1 = i0 + f.w;
            f.uv[size_t(y) * f.w + x * 2 + 0] = static_cast<uint8_t>(std::lround(std::clamp(0.5 * (cbFull[i0] + cbFull[i1]), 0.0, 255.0)));
            f.uv[size_t(y) * f.w + x * 2 + 1] = static_cast<uint8_t>(std::lround(std::clamp(0.5 * (crFull[i0] + crFull[i1]), 0.0, 255.0)));
        }
    return f;
}

std::vector<uint8_t> ReferenceNv12ToRgb8(const Nv12Frame& f, const gpu::Nv12Params& p, uint32_t visW, uint32_t visH) {
    const bool bt709 = p.matrix == gpu::YuvMatrix::Bt709;
    const double kr = bt709 ? 0.2126 : 0.299, kb = bt709 ? 0.0722 : 0.114, kg = 1.0 - kr - kb;
    std::vector<uint8_t> out(size_t(visW) * visH * 4);
    const int cw = static_cast<int>((visW + 1) / 2), ch = static_cast<int>((visH + 1) / 2);
    auto chroma = [&](int cx, int cy, int comp) {
        cx = std::clamp(cx, 0, cw - 1);
        cy = std::clamp(cy, 0, ch - 1);
        return double(f.uv[size_t(cy) * f.w + size_t(cx) * 2 + comp]);
    };
    for (uint32_t y = 0; y < visH; ++y)
        for (uint32_t x = 0; x < visW; ++x) {
            // Chroma sample (i, j) is located at luma (2i, 2j + 0.5).
            const double fx = x / 2.0, fy = (y - 0.5) / 2.0;
            const int i0 = static_cast<int>(std::floor(fx)), j0 = static_cast<int>(std::floor(fy));
            const double tx = fx - i0, ty = fy - j0;
            double c[2];
            for (int k = 0; k < 2; ++k) {
                const double top = chroma(i0, j0, k) * (1 - tx) + chroma(i0 + 1, j0, k) * tx;
                const double bottom = chroma(i0, j0 + 1, k) * (1 - tx) + chroma(i0 + 1, j0 + 1, k) * tx;
                c[k] = top * (1 - ty) + bottom * ty;
            }
            const double Y = p.fullRange ? f.y[size_t(y) * f.w + x] / 255.0 : (f.y[size_t(y) * f.w + x] - 16.0) / 219.0;
            const double cs = p.fullRange ? 255.0 : 224.0;
            const double cb = (c[0] - 128.0) / cs, cr = (c[1] - 128.0) / cs;
            const double r = Y + 2 * (1 - kr) * cr, b = Y + 2 * (1 - kb) * cb;
            const double g = (Y - kr * r - kb * b) / kg;
            uint8_t* o = &out[(size_t(y) * visW + x) * 4];
            o[0] = static_cast<uint8_t>(std::lround(std::clamp(r, 0.0, 1.0) * 255.0));
            o[1] = static_cast<uint8_t>(std::lround(std::clamp(g, 0.0, 1.0) * 255.0));
            o[2] = static_cast<uint8_t>(std::lround(std::clamp(b, 0.0, 1.0) * 255.0));
            o[3] = 255;
        }
    return out;
}

// ---- reference downscale (same math as shaders/Downscale.hlsl, double precision) ---------------------------
namespace {
double Lanczos3(double x) {
    x = std::abs(x);
    if (x < 1e-4) return 1.0;
    if (x >= 3.0) return 0.0;
    const double px = std::numbers::pi * x;
    return 3.0 * std::sin(px) * std::sin(px / 3.0) / (px * px);
}
double CatmullRom(double x) {
    x = std::abs(x);
    if (x < 1.0) return (1.5 * x - 2.5) * x * x + 1.0;
    if (x < 2.0) return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
    return 0.0;
}
double Weight(DownscaleFilter f, double x, double stretch) {
    if (f == DownscaleFilter::Lanczos3) return Lanczos3(x / stretch);
    if (f == DownscaleFilter::Bicubic) return CatmullRom(x / stretch);
    return std::clamp(0.5 * stretch + 0.5 - std::abs(x), 0.0, 1.0);
}
} // namespace

Image ReferenceDownscale(const Image& in, uint32_t outW, uint32_t outH, DownscaleFilter filter) {
    Image out(outW, outH);
    const double sx = double(in.w) / outW, sy = double(in.h) / outH;
    const double stx = std::max(sx, 1.0), sty = std::max(sy, 1.0);
    const double radius = filter == DownscaleFilter::Lanczos3 ? 3.0 : filter == DownscaleFilter::Bicubic ? 2.0 : 0.5;
    const double antiring = filter == DownscaleFilter::Lanczos3 ? 0.85 : filter == DownscaleFilter::Bicubic ? 0.5 : 0.0;
    const int mx = int(in.w) - 1, my = int(in.h) - 1;
    for (uint32_t oy = 0; oy < outH; ++oy)
        for (uint32_t ox = 0; ox < outW; ++ox) {
            const double cx = (ox + 0.5) * sx - 0.5, cy = (oy + 0.5) * sy - 0.5;
            const double rx = radius * stx + (filter == DownscaleFilter::Box ? 0.5 : 0.0), ry = radius * sty + (filter == DownscaleFilter::Box ? 0.5 : 0.0);
            const int x0 = static_cast<int>(std::ceil(cx - rx)), x1 = static_cast<int>(std::floor(cx + rx));
            const int y0 = static_cast<int>(std::ceil(cy - ry)), y1 = static_cast<int>(std::floor(cy + ry));
            double sum[3] = {0, 0, 0}, wsum = 0;
            for (int y = y0; y <= y1; ++y) {
                const double wy = Weight(filter, y - cy, sty);
                if (wy == 0.0) continue;
                for (int x = x0; x <= x1; ++x) {
                    const double w = wy * Weight(filter, x - cx, stx);
                    const uint32_t px = static_cast<uint32_t>(std::clamp(x, 0, mx)), py = static_cast<uint32_t>(std::clamp(y, 0, my));
                    for (int c = 0; c < 3; ++c) sum[c] += w * in.at(px, py, c);
                    wsum += w;
                }
            }
            const int fx = static_cast<int>(std::floor(cx)), fy = static_cast<int>(std::floor(cy));
            for (int c = 0; c < 3; ++c) {
                double v = sum[c] / wsum;
                if (antiring > 0.0) {
                    double lo = 1e9, hi = -1e9;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx) {
                            const double s = in.at(static_cast<uint32_t>(std::clamp(fx + dx, 0, mx)), static_cast<uint32_t>(std::clamp(fy + dy, 0, my)), c);
                            lo = std::min(lo, s);
                            hi = std::max(hi, s);
                        }
                    v = v + (std::clamp(v, lo, hi) - v) * antiring;
                }
                out.at(ox, oy, c) = static_cast<float>(v);
            }
        }
    return out;
}

double Psnr(const Image& a, const Image& b) {
    double mse = 0;
    size_t n = 0;
    for (size_t i = 0; i < a.px.size(); i += 4)
        for (int c = 0; c < 3; ++c) { const double d = a.px[i + c] - b.px[i + c]; mse += d * d; ++n; }
    mse /= double(n);
    return mse <= 1e-20 ? 200.0 : 10.0 * std::log10(1.0 / mse);
}

double MaxAbsDiff(const Image& a, const Image& b) {
    double m = 0;
    for (size_t i = 0; i < a.px.size(); i += 4)
        for (int c = 0; c < 3; ++c) m = std::max(m, std::abs(double(a.px[i + c]) - b.px[i + c]));
    return m;
}

double Blockiness(const Image& img) {
    double onSum = 0, offSum = 0;
    size_t onN = 0, offN = 0;
    for (uint32_t y = 1; y < img.h; ++y)
        for (uint32_t x = 1; x < img.w; ++x) {
            const double dx = std::abs(double(img.luma(x, y)) - img.luma(x - 1, y));
            const double dy = std::abs(double(img.luma(x, y)) - img.luma(x, y - 1));
            if (x % 4 == 0) { onSum += dx; ++onN; } else { offSum += dx; ++offN; }
            if (y % 4 == 0) { onSum += dy; ++onN; } else { offSum += dy; ++offN; }
        }
    return onSum / double(onN) - offSum / double(offN);
}

double MaxGradient(const Image& img) {
    double m = 0;
    for (uint32_t y = 0; y < img.h; ++y)
        for (uint32_t x = 1; x < img.w; ++x) m = std::max(m, std::abs(double(img.luma(x, y)) - img.luma(x - 1, y)));
    for (uint32_t y = 1; y < img.h; ++y)
        for (uint32_t x = 0; x < img.w; ++x) m = std::max(m, std::abs(double(img.luma(x, y)) - img.luma(x, y - 1)));
    return m;
}

} // namespace fluvi::test
