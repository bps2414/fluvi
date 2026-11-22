// SPDX-License-Identifier: GPL-3.0-only
// Fluvi pipeline passes: GPU results against CPU references on procedural data. Label "gpu": needs a hardware
// D3D12 adapter. The debug layer is on (plus GPU-based validation unless FLUVI_TEST_GBV=0) and every test case must
// finish with zero stored errors and no device removal.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include <DirectXPackedVector.h>

#include "Images.h"
#include "TestGpu.h"
#include "fluvi/gpu/FlowPasses.h"
#include "fluvi/gpu/Passes.h"

using namespace fluvi;
using namespace fluvi::gpu;
using namespace fluvi::test;
using DirectX::PackedVector::XMConvertHalfToFloat;
using DirectX::PackedVector::XMConvertFloatToHalf;

namespace {

// Per-test setup: shared device, fresh descriptors, and a zero-errors check at the end.
struct Fixture {
    TestGpu& gpu = TestGpu::Shared();
    Fixture() {
        if (gpu.Valid()) gpu.BeginTest();
    }
    ~Fixture() {
        if (!gpu.Valid()) return;
        std::string report;
        const int errors = gpu.EndTest(&report);
        CHECK_MESSAGE(errors == 0, "D3D12 debug layer reported:\n" << report);
    }
};

#define REQUIRE_GPU(f) \
    do { if (!(f).gpu.Valid()) { MESSAGE("no hardware D3D12 adapter, skipped"); return; } } while (0)

template <class Pass>
void InitPass(Pass& pass, ID3D12Device* device) {
    const Status st = pass.Init(device);
    INFO(st.message);
    REQUIRE(st.ok);
}

Tex NewOut(TestGpu& gpu, uint32_t w, uint32_t h, DXGI_FORMAT format = kIntermediateFormat) { return gpu.Create(w, h, format, true); }

// Textures that a pass will write: UAV state.
void ToUav(TestGpu& gpu, ID3D12GraphicsCommandList* l, Tex& t) { gpu.Transition(l, t, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); }
// Textures that a later pass will read.
void ToSrv(TestGpu& gpu, ID3D12GraphicsCommandList* l, Tex& t) { gpu.Transition(l, t, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); }

float HalfAt(const std::vector<uint8_t>& bytes, size_t index) { return XMConvertHalfToFloat(reinterpret_cast<const uint16_t*>(bytes.data())[index]); }

Image RunDeblock(TestGpu& gpu, const Image& in, float strength) {
    DeblockPass pass;
    InitPass(pass, gpu.Device());
    Tex src = UploadImage(gpu, in, kIntermediateFormat, false);
    Tex out = NewOut(gpu, in.w, in.h);
    gpu.Run([&](ID3D12GraphicsCommandList* l) {
        ToUav(gpu, l, out);
        pass.Record(l, src.gt, out.gt, strength);
    });
    return Download(gpu, out);
}

Image RunDownscale(TestGpu& gpu, const Image& in, uint32_t w, uint32_t h, DownscaleFilter filter) {
    DownscalePass pass;
    InitPass(pass, gpu.Device());
    Tex src = UploadImage(gpu, in, kIntermediateFormat, false);
    Tex out = NewOut(gpu, w, h);
    gpu.Run([&](ID3D12GraphicsCommandList* l) {
        ToUav(gpu, l, out);
        pass.Record(l, src.gt, out.gt, filter);
    });
    return Download(gpu, out);
}

Image RunRcas(TestGpu& gpu, const Image& in, float sharpness) {
    RcasPass pass;
    InitPass(pass, gpu.Device());
    Tex src = UploadImage(gpu, in, kIntermediateFormat, false);
    Tex out = NewOut(gpu, in.w, in.h);
    gpu.Run([&](ID3D12GraphicsCommandList* l) {
        ToUav(gpu, l, out);
        pass.Record(l, src.gt, out.gt, sharpness);
    });
    return Download(gpu, out);
}

bool AllFinite(const Image& img) {
    return std::all_of(img.px.begin(), img.px.end(), [](float v) { return std::isfinite(v); });
}

} // namespace

TEST_CASE("device: hardware adapter with the debug layer on") {
    Fixture f;
    REQUIRE_GPU(f);
    MESSAGE("adapter: " << f.gpu.AdapterName());
    // Needs the Windows "Graphics Tools" optional feature (d3d12sdklayers.dll): a warning, not a failure, without it.
    WARN_MESSAGE(f.gpu.DebugLayerOn(), "D3D12 debug layer unavailable: GPU tests run without validation");
}

// ---- NV12 -> RGB ------------------------------------------------------------------------------------------
TEST_CASE("Nv12ToRgb matches the CPU matrix reference within 1/255 (709/601, limited/full)") {
    Fixture f;
    REQUIRE_GPU(f);
    Nv12ToRgbPass pass;
    InitPass(pass, f.gpu.Device());
    const Nv12Frame frame = MakeNv12(128, 72);
    Tex nv12 = f.gpu.CreateNv12(frame.w, frame.h);
    f.gpu.UploadNv12(nv12, frame.y.data(), frame.uv.data());
    for (const YuvMatrix m : {YuvMatrix::Bt709, YuvMatrix::Bt601})
        for (const bool full : {false, true}) {
            Nv12Params p;
            p.matrix = m;
            p.fullRange = full;
            // 8-bit output.
            Tex out8 = NewOut(f.gpu, frame.w, frame.h, kOutputFormat);
            f.gpu.Run([&](ID3D12GraphicsCommandList* l) {
                ToUav(f.gpu, l, out8);
                pass.Record(l, nv12.gt, out8.gt, p);
            });
            const auto gpu8 = f.gpu.Readback(out8);
            const auto ref = ReferenceNv12ToRgb8(frame, p, frame.w, frame.h);
            int maxErr = 0;
            for (size_t i = 0; i < ref.size(); ++i) maxErr = std::max(maxErr, std::abs(int(gpu8[i]) - int(ref[i])));
            INFO("matrix " << (m == YuvMatrix::Bt709 ? "709" : "601") << (full ? " full" : " limited"));
            CHECK(maxErr <= 1);
            // RGBA16F intermediate: same values within 1/255 after rounding to 8 bits.
            Tex out16 = NewOut(f.gpu, frame.w, frame.h);
            f.gpu.Run([&](ID3D12GraphicsCommandList* l) {
                ToUav(f.gpu, l, out16);
                pass.Record(l, nv12.gt, out16.gt, p);
            });
            const auto raw16 = f.gpu.Readback(out16);
            int maxErr16 = 0;
            for (size_t i = 0; i < ref.size(); ++i)
                maxErr16 = std::max(maxErr16, std::abs(int(std::lround(HalfAt(raw16, i) * 255.f)) - int(ref[i])));
            CHECK(maxErr16 <= 1);
        }
}

TEST_CASE("Nv12ToRgb crops to the visible size, odd sizes included, and leaves the rest alone") {
    Fixture f;
    REQUIRE_GPU(f);
    Nv12ToRgbPass pass;
    InitPass(pass, f.gpu.Device());
    const Nv12Frame frame = MakeNv12(128, 72);
    Tex nv12 = f.gpu.CreateNv12(frame.w, frame.h);
    f.gpu.UploadNv12(nv12, frame.y.data(), frame.uv.data());
    const Image sentinel = Constant(frame.w, frame.h, 0.25f, 0.5f, 0.75f);
    Tex out = UploadImage(f.gpu, sentinel, kOutputFormat, true);
    Nv12Params p;
    p.visibleWidth = 101;
    p.visibleHeight = 59;
    f.gpu.Run([&](ID3D12GraphicsCommandList* l) {
        ToUav(f.gpu, l, out);
        pass.Record(l, nv12.gt, out.gt, p);
    });
    const auto gpu8 = f.gpu.Readback(out);
    const auto ref = ReferenceNv12ToRgb8(frame, p, 101, 59);
    int maxErr = 0, outsideChanged = 0;
    for (uint32_t y = 0; y < frame.h; ++y)
        for (uint32_t x = 0; x < frame.w; ++x)
            for (int c = 0; c < 3; ++c) {
                const int g = gpu8[(size_t(y) * frame.w + x) * 4 + c];
                if (x < 101 && y < 59) maxErr = std::max(maxErr, std::abs(g - int(ref[(size_t(y) * 101 + x) * 4 + c])));
                else if (std::abs(g - int(std::lround(sentinel.at(x, y, c) * 255.f))) > 0) ++outsideChanged;
            }
    CHECK(maxErr <= 1);
    CHECK(outsideChanged == 0);
}

// ---- Deblock ----------------------------------------------------------------------------------------------
TEST_CASE("Deblock lowers blockiness on blocky input") {
    Fixture f;
    REQUIRE_GPU(f);
    const Image blocky = BlockyGradient(192, 108, 0.03f);
    const double before = Blockiness(blocky);
    const double strong = Blockiness(RunDeblock(f.gpu, blocky, 1.0f));
    const double mid = Blockiness(RunDeblock(f.gpu, blocky, 0.5f));
    MESSAGE("blockiness before " << before << ", strength 0.5: " << mid << ", strength 1: " << strong);
    CHECK(before > 0.01);
    CHECK(strong < 0.5 * before);
    CHECK(mid < before);
    CHECK(mid > strong);
}

TEST_CASE("Deblock keeps clean edges, fine text-like detail and smooth gradients") {
    Fixture f;
    REQUIRE_GPU(f);
    const Image text = TextEdges(192, 108);
    const Image out = RunDeblock(f.gpu, text, 1.0f);
    CHECK(MaxAbsDiff(out, text) < 0.002);
    CHECK(MaxGradient(out) >= 0.999 * MaxGradient(text));
    const Image grad = Gradient(192, 108);
    CHECK(MaxAbsDiff(RunDeblock(f.gpu, grad, 1.0f), grad) < 1.0 / 255.0);
    // A real, soft edge on the block grid (step of 0.3, above the filter's alpha) must survive too.
    Image edge = Constant(192, 108, 0.3f, 0.3f, 0.3f);
    for (uint32_t y = 0; y < edge.h; ++y)
        for (uint32_t x = 96; x < edge.w; ++x) for (int c = 0; c < 3; ++c) edge.at(x, y, c) = 0.6f;
    CHECK(MaxAbsDiff(RunDeblock(f.gpu, edge, 1.0f), edge) < 0.002);
}

TEST_CASE("Deblock with strength 0 is an exact copy") {
    Fixture f;
    REQUIRE_GPU(f);
    const Image blocky = HalfRounded(BlockyGradient(192, 108, 0.03f));
    const Image out = RunDeblock(f.gpu, blocky, 0.0f);
    CHECK(out.px == blocky.px);
}

// ---- Downscale --------------------------------------------------------------------------------------------
TEST_CASE("Downscale matches the CPU reference (PSNR) for every filter") {
    Fixture f;
    REQUIRE_GPU(f);
    // 4:3 reduction, the 2560x1440 -> 1920x1080 ratio. The reference runs on the half-rounded input in double;
    // the remaining difference is the half-precision rounding of the output (about 1e-4 rms, ~74 dB), so a 60 dB
    // floor passes only an implementation that uses the same kernels, and an error in weights, centring or
    // normalisation falls far below it.
    const struct { const char* name; Image img; } sources[] = {
        {"zone plate", ZonePlate(640, 360)}, {"text edges", TextEdges(640, 360)}, {"noise", Noise(640, 360, 7, 5)}};
    for (const auto filter : {DownscaleFilter::Lanczos3, DownscaleFilter::Bicubic, DownscaleFilter::Box})
        for (const auto& s : sources) {
            const Image out = RunDownscale(f.gpu, s.img, 480, 270, filter);
            const Image ref = ReferenceDownscale(HalfRounded(s.img), 480, 270, filter);
            const double psnr = Psnr(out, ref);
            INFO(s.name << " filter " << int(filter) << " PSNR " << psnr);
            CHECK(psnr > 60.0);
        }
}

TEST_CASE("Downscale: constants stay constant, edges ring little, 2560x1440 -> 1920x1080 runs") {
    Fixture f;
    REQUIRE_GPU(f);
    // DC gain 1.
    const Image flat = Constant(640, 360, 0.37f, 0.5f, 0.81f);
    const Image flatOut = RunDownscale(f.gpu, flat, 480, 270, DownscaleFilter::Lanczos3);
    CHECK(MaxAbsDiff(flatOut, HalfRounded(flat)) < 2e-3);
    // Ringing of a black/white step: plain Lanczos3 overshoots by about 7 %, the anti-ringing clamp keeps it scaled.
    const Image step = StepEdge(640, 360);
    const Image stepOut = RunDownscale(f.gpu, step, 480, 270, DownscaleFilter::Lanczos3);
    float lo = 1.f, hi = 0.f;
    for (size_t i = 0; i < stepOut.px.size(); i += 4) { lo = std::min(lo, stepOut.px[i]); hi = std::max(hi, stepOut.px[i]); }
    MESSAGE("step response range [" << lo << ", " << hi << "]");
    CHECK(hi <= 1.0f + 0.015f);
    CHECK(lo >= -0.015f);
    // The edge is still sharp: it goes from <0.1 to >0.9 within 2 output pixels.
    const uint32_t mid = 240;
    CHECK(stepOut.at(mid - 2, 100, 0) < 0.1f);
    CHECK(stepOut.at(mid + 1, 100, 0) > 0.9f);
    // The real thing: 2560x1440 -> 1920x1080.
    const Image big = Noise(2560, 1440, 3, 9);
    const Image bigOut = RunDownscale(f.gpu, big, 1920, 1080, DownscaleFilter::Lanczos3);
    CHECK(AllFinite(bigOut));
    // A smooth noise field downscales to nearly itself: compare a sample of pixels with a bilinear lookup.
    double worst = 0;
    for (uint32_t y = 50; y < 1030; y += 97)
        for (uint32_t x = 50; x < 1870; x += 131) {
            const float sx = (x + 0.5f) * 4.f / 3.f - 0.5f, sy = (y + 0.5f) * 4.f / 3.f - 0.5f;
            const uint32_t x0 = static_cast<uint32_t>(sx), y0 = static_cast<uint32_t>(sy);
            const float tx = sx - x0, ty = sy - y0;
            const float v = (big.at(x0, y0, 1) * (1 - tx) + big.at(x0 + 1, y0, 1) * tx) * (1 - ty) + (big.at(x0, y0 + 1, 1) * (1 - tx) + big.at(x0 + 1, y0 + 1, 1) * tx) * ty;
            worst = std::max(worst, std::abs(double(v) - bigOut.at(x, y, 1)));
        }
    CHECK(worst < 0.02);
}

// ---- EASU / RCAS ------------------------------------------------------------------------------------------
TEST_CASE("EASU upscales sanely: finite, in range, flat stays flat, a linear ramp is reconstructed") {
    Fixture f;
    REQUIRE_GPU(f);
    EasuPass pass;
    InitPass(pass, f.gpu.Device());
    auto run = [&](const Image& in, uint32_t w, uint32_t h) {
        Tex src = UploadImage(f.gpu, in, kIntermediateFormat, false);
        Tex out = NewOut(f.gpu, w, h);
        f.gpu.Run([&](ID3D12GraphicsCommandList* l) {
            ToUav(f.gpu, l, out);
            pass.Record(l, src.gt, out.gt);
        });
        return Download(f.gpu, out);
    };
    const Image flatOut = run(Constant(240, 135, 0.3f, 0.6f, 0.9f), 480, 270);
    CHECK(MaxAbsDiff(flatOut, HalfRounded(Constant(480, 270, 0.3f, 0.6f, 0.9f))) < 2e-3);

    const Image ramp = Gradient(240, 135);
    const Image rampOut = run(ramp, 480, 270);
    double sq = 0;
    size_t n = 0;
    for (uint32_t y = 6; y < 264; ++y)
        for (uint32_t x = 6; x < 474; ++x) {
            const double ix = (x + 0.5) / 2.0 - 0.5, iy = (y + 0.5) / 2.0 - 0.5;
            const double expectR = 0.1 + 0.8 * ix / 239.0, expectG = 0.1 + 0.8 * iy / 134.0;
            sq += std::pow(rampOut.at(x, y, 0) - expectR, 2) + std::pow(rampOut.at(x, y, 1) - expectG, 2);
            n += 2;
        }
    const double psnr = 10.0 * std::log10(1.0 / (sq / double(n)));
    MESSAGE("EASU ramp PSNR " << psnr);
    CHECK(psnr > 45.0);

    const Image zone = run(ZonePlate(320, 180), 640, 360);
    CHECK(AllFinite(zone));
    CHECK(*std::min_element(zone.px.begin(), zone.px.end()) >= 0.f);
    CHECK(*std::max_element(zone.px.begin(), zone.px.end()) <= 1.f);
    CHECK(AllFinite(run(TextEdges(320, 180), 480, 270)));   // 1.5x, the 720p -> 1080p ratio
}

TEST_CASE("RCAS: sharpness 0 is identity, output is finite, and it sharpens") {
    Fixture f;
    REQUIRE_GPU(f);
    const Image noisy = HalfRounded(Noise(192, 108, 11, 3));
    CHECK(RunRcas(f.gpu, noisy, 0.f).px == noisy.px);
    // Black regions and hard edges stress RCAS' limiter divisions: no NaN/Inf allowed.
    Image hard = TextEdges(192, 108);
    for (uint32_t y = 0; y < 40; ++y) for (uint32_t x = 0; x < 60; ++x) for (int c = 0; c < 3; ++c) hard.at(x, y, c) = 0.f;
    for (const float s : {0.25f, 0.5f, 1.0f}) {
        const Image out = RunRcas(f.gpu, hard, s);
        CHECK(AllFinite(out));
        CHECK(*std::min_element(out.px.begin(), out.px.end()) >= -0.001f);
        // RCAS' limiter only looks at the 4-neighbour ring, so an isolated peak can overshoot (AMD's design; the
        // output format clamps it). The lobe is capped at -3/16, so the gain is bounded: stay under 1.5.
        CHECK(*std::max_element(out.px.begin(), out.px.end()) <= 1.5f);
    }
    // Flat stays flat.
    const Image flat = Constant(64, 36, 0.4f, 0.4f, 0.4f);
    CHECK(MaxAbsDiff(RunRcas(f.gpu, flat, 1.f), HalfRounded(flat)) < 1.0 / 255.0);   // approximate rcp in the resolve: ~0.4 LSB
    // A soft edge gets steeper, and the effect grows with sharpness.
    Image soft(192, 108);
    for (uint32_t y = 0; y < soft.h; ++y)
        for (uint32_t x = 0; x < soft.w; ++x) {
            const float v = 0.2f + 0.6f * std::clamp((float(x) - 93.f) / 6.f, 0.f, 1.f);
            for (int c = 0; c < 3; ++c) soft.at(x, y, c) = v;
        }
    const double g0 = MaxGradient(soft), g5 = MaxGradient(RunRcas(f.gpu, soft, 0.5f)), g10 = MaxGradient(RunRcas(f.gpu, soft, 1.f));
    MESSAGE("max gradient: " << g0 << " -> " << g5 << " -> " << g10);
    CHECK(g5 > g0);
    CHECK(g10 > g5);
}

// ---- whole chain ------------------------------------------------------------------------------------------
TEST_CASE("chain NV12 -> RGB -> deblock -> downscale -> RCAS runs and produces a plausible picture") {
    Fixture f;
    REQUIRE_GPU(f);
    Nv12ToRgbPass nv;
    DeblockPass db;
    DownscalePass ds;
    RcasPass rc;
    InitPass(nv, f.gpu.Device());
    InitPass(db, f.gpu.Device());
    InitPass(ds, f.gpu.Device());
    InitPass(rc, f.gpu.Device());
    const Image source = Noise(512, 288, 5, 24);
    const Nv12Frame frame = RgbToNv12(source, YuvMatrix::Bt709, false);
    Tex nv12 = f.gpu.CreateNv12(frame.w, frame.h);
    f.gpu.UploadNv12(nv12, frame.y.data(), frame.uv.data());
    Tex rgb = NewOut(f.gpu, 512, 288), deb = NewOut(f.gpu, 512, 288), scaled = NewOut(f.gpu, 384, 216), fin = NewOut(f.gpu, 384, 216, kOutputFormat);
    f.gpu.Run([&](ID3D12GraphicsCommandList* l) {
        ToUav(f.gpu, l, rgb);
        nv.Record(l, nv12.gt, rgb.gt, {});
        f.gpu.UavBarrier(l, rgb);
        ToSrv(f.gpu, l, rgb);
        ToUav(f.gpu, l, deb);
        db.Record(l, rgb.gt, deb.gt, 0.5f);
        f.gpu.UavBarrier(l, deb);
        ToSrv(f.gpu, l, deb);
        ToUav(f.gpu, l, scaled);
        ds.Record(l, deb.gt, scaled.gt, DownscaleFilter::Lanczos3);
        f.gpu.UavBarrier(l, scaled);
        ToSrv(f.gpu, l, scaled);
        ToUav(f.gpu, l, fin);
        rc.Record(l, scaled.gt, fin.gt, 0.3f);
    });
    const Image outImg = Download(f.gpu, fin);
    const Image expect = ReferenceDownscale(source, 384, 216, DownscaleFilter::Lanczos3);
    const double psnr = Psnr(outImg, expect);
    MESSAGE("chain PSNR vs source downscale " << psnr);
    CHECK(psnr > 30.0);   // chroma subsampling + 8-bit quantisation + sharpening
}

// ---- flow chain -------------------------------------------------------------------------------------------
namespace {
struct FlowStats { double refinedOk, meanCost, meanAgreement, filledOk; };

// Builds a synthetic pair with a uniform current->previous motion of (3, 2) flow pixels, a raw grid with 1/3 of the
// cells wrong, and runs FlowRefine -> FlowClaim -> FlowFill at the given flow extent.
FlowStats RunFlowChain(TestGpu& gpu, uint32_t fw, uint32_t fh) {
    const uint32_t grid = 8, gw = (fw + grid - 1) / grid, gh = (fh + grid - 1) / grid;
    const Image base = Noise(fw + 16, fh + 16, 21, 6);
    Image cur(fw, fh), prev(fw, fh);
    for (uint32_t y = 0; y < fh; ++y)
        for (uint32_t x = 0; x < fw; ++x)
            for (int c = 0; c < 4; ++c) {
                cur.at(x, y, c) = base.at(x + 8, y + 8, c);
                prev.at(x, y, c) = base.at(x + 8 - 3, y + 8 - 2, c);   // prev(p + v) == cur(p) for v = (3, 2)
            }
    std::vector<int16_t> raw(size_t(gw) * gh * 2);
    for (uint32_t y = 0; y < gh; ++y)
        for (uint32_t x = 0; x < gw; ++x) {
            const bool wrong = (x * 7 + y * 13) % 3 == 0;
            raw[(size_t(y) * gw + x) * 2 + 0] = wrong ? int16_t(-5) : int16_t(3);
            raw[(size_t(y) * gw + x) * 2 + 1] = wrong ? int16_t(7) : int16_t(2);
        }
    FlowRefinePass refine;
    FlowClaimPass claim;
    FlowFillPass fill;
    InitPass(refine, gpu.Device());
    InitPass(claim, gpu.Device());
    InitPass(fill, gpu.Device());
    Tex rawTex = gpu.Create(gw, gh, kFlowRawFormat, false);
    gpu.Upload(rawTex, raw.data(), size_t(gw) * 4);
    const uint32_t noCut[2] = {0, 0};   // FidelityFX scene-change history, entry (1,0) = 0: no cut in the last frames
    Tex sceneTex = gpu.Create(2, 1, kFlowClaimsFormat, false);
    gpu.Upload(sceneTex, noCut, sizeof noCut);
    Tex curTex = UploadImage(gpu, cur, kOutputFormat, false), prevTex = UploadImage(gpu, prev, kOutputFormat, false);
    Tex refined = gpu.Create(fw, fh, kFlowRefinedFormat, true), claims = gpu.Create(fw, fh, kFlowClaimsFormat, true);
    Tex agreement = gpu.Create(fw, fh, kFlowAgreementFormat, true), filled = gpu.Create(fw, fh, kFlowVectorFormat, true);
    FlowRefineParams rp;
    gpu.Run([&](ID3D12GraphicsCommandList* l) {
        ToUav(gpu, l, refined);
        refine.Record(l, rawTex.gt, prevTex.gt, curTex.gt, {}, sceneTex.gt, refined.gt, rp);
        TestGpu::UavBarrier(l, refined);
        ToSrv(gpu, l, refined);
        ToUav(gpu, l, claims);
        ToUav(gpu, l, agreement);
        claim.Record(l, refined.gt, claims.gt, agreement.gt);
        TestGpu::UavBarrier(l, agreement);
        ToSrv(gpu, l, agreement);
        ToUav(gpu, l, filled);
        fill.Record(l, refined.gt, curTex.gt, agreement.gt, filled.gt, {});
    });
    const auto ref = gpu.Readback(refined);
    const auto agr = gpu.Readback(agreement);
    const auto fil = gpu.Readback(filled);
    FlowStats s{};
    size_t n = 0;
    for (uint32_t y = 12; y + 12 < fh; ++y)
        for (uint32_t x = 12; x + 12 < fw; ++x) {
            const size_t i = size_t(y) * fw + x;
            const float vx = HalfAt(ref, i * 4), vy = HalfAt(ref, i * 4 + 1), cost = HalfAt(ref, i * 4 + 2);
            s.refinedOk += (std::abs(vx - 3.f) < 0.01f && std::abs(vy - 2.f) < 0.01f);
            s.meanCost += cost;
            s.meanAgreement += HalfAt(agr, i);
            s.filledOk += (std::abs(HalfAt(fil, i * 2) - 3.f) < 0.05f && std::abs(HalfAt(fil, i * 2 + 1) - 2.f) < 0.05f);
            ++n;
        }
    s.refinedOk /= double(n);
    s.meanCost /= double(n);
    s.meanAgreement /= double(n);
    s.filledOk /= double(n);
    return s;
}
} // namespace

TEST_CASE("FlowRefine -> FlowClaim -> FlowFill recover a uniform motion from a half-wrong grid") {
    Fixture f;
    REQUIRE_GPU(f);
    const FlowStats s = RunFlowChain(f.gpu, 192, 108);
    MESSAGE("refined ok " << s.refinedOk << " cost " << s.meanCost << " agreement " << s.meanAgreement << " filled ok " << s.filledOk);
    CHECK(s.refinedOk > 0.9);
    CHECK(s.meanCost < 0.01);
    CHECK(s.meanAgreement < 0.1);
    CHECK(s.filledOk > 0.95);
}

TEST_CASE("flow chain has no resolution cap: 2560x1440 flow extent (Veyra #232)") {
    Fixture f;
    REQUIRE_GPU(f);
    const FlowStats s = RunFlowChain(f.gpu, 2560, 1440);
    MESSAGE("refined ok " << s.refinedOk << " cost " << s.meanCost << " agreement " << s.meanAgreement << " filled ok " << s.filledOk);
    CHECK(s.refinedOk > 0.9);
    CHECK(s.meanCost < 0.01);
    CHECK(s.meanAgreement < 0.1);
    CHECK(s.filledOk > 0.95);
}

TEST_CASE("FlowAdapt rescales vectors to another extent") {
    Fixture f;
    REQUIRE_GPU(f);
    FlowAdaptPass pass;
    InitPass(pass, f.gpu.Device());
    const uint32_t sw = 24, sh = 14, ow = 192, oh = 108;
    std::vector<uint16_t> data(size_t(sw) * sh * 2);
    for (size_t i = 0; i < data.size(); i += 2) { data[i] = XMConvertFloatToHalf(2.f); data[i + 1] = XMConvertFloatToHalf(-1.f); }
    Tex in = f.gpu.Create(sw, sh, kFlowVectorFormat, false);
    f.gpu.Upload(in, data.data(), size_t(sw) * 4);
    Tex out = f.gpu.Create(ow, oh, kFlowVectorFormat, true);
    f.gpu.Run([&](ID3D12GraphicsCommandList* l) {
        ToUav(f.gpu, l, out);
        pass.Record(l, in.gt, out.gt);
    });
    const auto bytes = f.gpu.Readback(out);
    int bad = 0;
    for (uint32_t y = 0; y < oh; ++y)
        for (uint32_t x = 0; x < ow; ++x) {
            const size_t i = (size_t(y) * ow + x) * 2;
            if (std::abs(HalfAt(bytes, i) - 2.f * ow / sw) > 0.02f || std::abs(HalfAt(bytes, i + 1) + 1.f * oh / sh) > 0.02f) ++bad;
        }
    CHECK(bad == 0);
}

// ---- shot change ------------------------------------------------------------------------------------------
namespace {
struct ShotResult { bool cut; float sad, histogram; uint32_t cuts; };

ShotResult RunShot(TestGpu& gpu, const Image& a, const Image& b, uint32_t parity, bool compare) {
    ShotChangePass pass;
    InitPass(pass, gpu.Device());
    Tex prev = UploadImage(gpu, a, kOutputFormat, false), cur = UploadImage(gpu, b, kOutputFormat, false);
    auto results = gpu.CreateResultBuffer(kShotChangeResultBytes);
    ShotChangeParams p;
    p.parity = parity;
    p.compare = compare;
    gpu.Run([&](ID3D12GraphicsCommandList* l) { pass.Record(l, prev.gt, cur.gt, results.Get(), p); });
    const auto bytes = gpu.ReadbackBuffer(results.Get(), kShotChangeResultBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ShotResult r{};
    uint64_t flag;
    std::memcpy(&flag, bytes.data() + parity * 8, 8);
    r.cut = flag == 1;
    std::memcpy(&r.sad, bytes.data() + 16 + parity * 16, 4);
    std::memcpy(&r.histogram, bytes.data() + 20 + parity * 16, 4);
    std::memcpy(&r.cuts, bytes.data() + 48, 4);
    return r;
}
} // namespace

TEST_CASE("ShotChange flags a hard cut, ignores identical frames, resets, at any resolution") {
    Fixture f;
    REQUIRE_GPU(f);
    const Image grad = Gradient(320, 180), black = Constant(320, 180, 0, 0, 0), white = Constant(320, 180, 1, 1, 1);
    const ShotResult same = RunShot(f.gpu, grad, grad, 0, true);
    CHECK_FALSE(same.cut);
    CHECK(same.sad == doctest::Approx(0.f).epsilon(1e-6));
    const ShotResult cut = RunShot(f.gpu, black, white, 0, true);
    CHECK(cut.cut);
    CHECK(cut.sad == doctest::Approx(1.f).epsilon(0.01));
    CHECK(cut.histogram == doctest::Approx(1.f).epsilon(0.01));
    CHECK(cut.cuts == 1);
    const ShotResult cutOdd = RunShot(f.gpu, black, white, 1, true);   // slot 1
    CHECK(cutOdd.cut);
    CHECK_FALSE(RunShot(f.gpu, black, white, 0, false).cut);           // no valid previous frame
    // A different picture of the same brightness, at 2560x1440 (no resolution limit).
    const Image bigA = Constant(2560, 1440, 0, 0, 0), bigB = Constant(2560, 1440, 1, 1, 1);
    CHECK(RunShot(f.gpu, bigA, bigB, 0, true).cut);
    CHECK_FALSE(RunShot(f.gpu, bigA, bigA, 0, true).cut);
}

// ---- HUD lock ---------------------------------------------------------------------------------------------
TEST_CASE("HudLockUpdate + HudLockCompose lock a still HUD and leave moving content alone") {
    Fixture f;
    REQUIRE_GPU(f);
    HudLockUpdatePass update;
    HudLockComposePass compose;
    InitPass(update, f.gpu.Device());
    InitPass(compose, f.gpu.Device());
    const uint32_t w = 128, h = 72;
    // Left half: content that changes every frame. Right half: a still HUD (hard strokes).
    const Image hud = TextEdges(w, h), noiseA = Noise(w, h, 1, 4);
    Image a = hud, b = hud;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w / 2; ++x)
            for (int c = 0; c < 4; ++c) {
                // Every moving pixel changes by 0.5 each frame: with two alternating random frames some pixels
                // would, by chance, differ by less than the "still" threshold in every frame and really be still.
                const float v = noiseA.at(x, y, c);
                a.at(x, y, c) = v;
                b.at(x, y, c) = v < 0.5f ? v + 0.5f : v - 0.5f;
            }
    Tex texA = UploadImage(f.gpu, a, kIntermediateFormat, false), texB = UploadImage(f.gpu, b, kIntermediateFormat, false);
    const Image gray = Constant(w, h, 0.5f, 0.5f, 0.5f);
    Tex generated = UploadImage(f.gpu, gray, kIntermediateFormat, false);
    Tex state = f.gpu.Create(w, h, DXGI_FORMAT_R16G16_FLOAT, true);
    Tex out = NewOut(f.gpu, w, h);
    f.gpu.Run([&](ID3D12GraphicsCommandList* l) {
        ToUav(f.gpu, l, state);
        for (int frame = 0; frame < 64; ++frame) {
            HudLockParams p;
            p.reset = frame == 0;
            update.Record(l, frame % 2 ? texB.gt : texA.gt, frame % 2 ? texA.gt : texB.gt, state.gt, p);
            TestGpu::UavBarrier(l, state);
        }
        ToSrv(f.gpu, l, state);
        ToUav(f.gpu, l, out);
        compose.Record(l, generated.gt, texA.gt, state.gt, out.gt, {});
    });
    const Image result = Download(f.gpu, out);
    const auto stateBytes = f.gpu.Readback(state);
    MESSAGE("state at a still pixel (x): " << HalfAt(stateBytes, (size_t(36) * w + 100) * 2));
    int movingWrong = 0, strokeWrong = 0, flatWrong = 0, strokes = 0, flats = 0;
    for (uint32_t y = 4; y + 4 < h; ++y)
        for (uint32_t x = 4; x + 4 < w; ++x) {
            const float v = result.at(x, y, 0);
            if (x < w / 2 - 2) { movingWrong += std::abs(v - 0.5f) > 0.01f; continue; }
            if (x < w / 2 + 4) continue;
            bool flat = true;
            for (int dy = -3; dy <= 3; ++dy) for (int dx = -3; dx <= 3; ++dx) flat &= std::abs(hud.at(x + dx, y + dy, 0) - hud.at(x, y, 0)) < 1e-6f;
            if (hud.at(x, y, 0) > 0.5f) { ++strokes; strokeWrong += std::abs(v - hud.at(x, y, 0)) > 0.01f; }
            else if (flat) { ++flats; flatWrong += std::abs(v - 0.5f) > 0.01f; }
        }
    CHECK(strokes > 50);
    CHECK(flats > 100);
    CHECK(movingWrong == 0);
    CHECK(strokeWrong == 0);
    CHECK(flatWrong == 0);
}
