// SPDX-License-Identifier: GPL-3.0-only
// Fluvi quality chain: path selection, presets, GPU detection, the new-frames gate (no GPU needed) and the
// chain itself on the GPU (same rules as PipelineTests.cpp: debug layer on, zero errors per test case).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "ChainHarness.h"
#include "Images.h"
#include "TestGpu.h"
#include "fluvi/gpu/QualityChain.h"

using namespace fluvi;
using namespace fluvi::gpu;
using namespace fluvi::test;

namespace {

// ---- path planning, presets, gate: pure logic ---------------------------------------------------------------------

struct PathCase {
    uint32_t srcW, srcH, outW, outH;
    QualityPath path;
    uint32_t fitW, fitH, offX, offY;
};

TEST_CASE("PlanLayout picks the path from stream vs output and fits without stretching") {
    const PathCase cases[] = {
        {1280, 720, 1920, 1080, QualityPath::UpscaleFsr4, 1920, 1080, 0, 0},
        {2560, 1440, 1920, 1080, QualityPath::Downscale, 1920, 1080, 0, 0},
        {2560, 1440, 2560, 1440, QualityPath::Native, 2560, 1440, 0, 0},
        {2560, 1440, 3840, 2160, QualityPath::UpscaleFsr4, 3840, 2160, 0, 0},
        {2560, 1440, 3440, 1440, QualityPath::Native, 2560, 1440, 440, 0},         // ultrawide: pillarbox, 1:1
        {1920, 1080, 2560, 1600, QualityPath::UpscaleFsr4, 2560, 1440, 0, 80},     // 16:10 bigger: letterbox
        {2560, 1440, 1920, 1200, QualityPath::Downscale, 1920, 1080, 0, 60},       // 16:10 smaller
        {2560, 1440, 1280, 720, QualityPath::Downscale, 1280, 720, 0, 0},
        {1920, 1080, 1920, 1080, QualityPath::Native, 1920, 1080, 0, 0},
        {1920, 1080, 1366, 768, QualityPath::Downscale, 1366, 768, 0, 0},          // 16:9 laptop panel
        {1280, 720, 1920, 1200, QualityPath::UpscaleFsr4, 1920, 1080, 0, 60},
        {1280, 720, 3840, 2160, QualityPath::UpscaleFsr4, 3840, 2160, 0, 0},       // exactly 3x: FSR 4's limit
        {1280, 720, 5120, 2880, QualityPath::UpscaleEasu, 5120, 2880, 0, 0},       // 4x: over the FSR 4 range
        {2560, 1440, 5120, 1440, QualityPath::Native, 2560, 1440, 1280, 0},        // super ultrawide
        {2560, 1440, 1080, 1920, QualityPath::Downscale, 1080, 608, 0, 656},       // portrait window
        {1920, 1080, 800, 600, QualityPath::Downscale, 800, 450, 0, 75},
    };
    for (const PathCase& c : cases) {
        INFO(c.srcW << "x" << c.srcH << " -> " << c.outW << "x" << c.outH);
        const ChainLayout l = PlanLayout(c.srcW, c.srcH, c.outW, c.outH, Upscaler::Fsr4, true);
        CHECK(l.path == c.path);
        CHECK(l.fitW == c.fitW);
        CHECK(l.fitH == c.fitH);
        CHECK(l.offX == c.offX);
        CHECK(l.offY == c.offY);
        CHECK(l.HasBars() == (c.fitW != c.outW || c.fitH != c.outH));
    }
}

TEST_CASE("PlanLayout: Easu or an unusable FSR 4 always gives EASU on an upscale, other paths ignore the choice") {
    CHECK(PlanLayout(1280, 720, 1920, 1080, Upscaler::Easu, true).path == QualityPath::UpscaleEasu);
    CHECK(PlanLayout(1280, 720, 1920, 1080, Upscaler::Fsr4, false).path == QualityPath::UpscaleEasu);
    CHECK(PlanLayout(2560, 1440, 3840, 2160, Upscaler::Fsr4, false).path == QualityPath::UpscaleEasu);
    CHECK(PlanLayout(2560, 1440, 1920, 1080, Upscaler::Fsr4, false).path == QualityPath::Downscale);
    CHECK(PlanLayout(2560, 1440, 2560, 1440, Upscaler::Fsr4, false).path == QualityPath::Native);
}

TEST_CASE("PlanLayout invariants over many stream/output pairs") {
    const uint32_t streams[][2] = {{1280, 720}, {1920, 1080}, {2560, 1440}, {1920, 1088}};
    const uint32_t outs[][2] = {{640, 360}, {800, 600}, {1024, 768}, {1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {1920, 1200}, {2560, 1080},
                                {2560, 1440}, {2560, 1600}, {3440, 1440}, {3840, 1600}, {3840, 2160}, {5120, 1440}, {7680, 4320}, {1080, 1920}, {301, 777}};
    for (const auto& s : streams)
        for (const auto& o : outs) {
            INFO(s[0] << "x" << s[1] << " -> " << o[0] << "x" << o[1]);
            const ChainLayout l = PlanLayout(s[0], s[1], o[0], o[1], Upscaler::Fsr4, true);
            REQUIRE(l.fitW >= 1);
            REQUIRE(l.fitH >= 1);
            CHECK(l.fitW <= o[0]);
            CHECK(l.fitH <= o[1]);
            CHECK((l.fitW == o[0] || l.fitH == o[1]));                       // as large as it fits
            CHECK(l.offX == (o[0] - l.fitW) / 2);                            // centred
            CHECK(l.offY == (o[1] - l.fitH) / 2);
            // Aspect kept to within two pixels of the shorter fit side (rounding, and the near-16:9 snap).
            const double aspectSrc = double(s[0]) / s[1], aspectFit = double(l.fitW) / l.fitH;
            CHECK(std::abs(aspectSrc - aspectFit) <= 2.0 * aspectSrc / double(std::min(l.fitW, l.fitH)) + 1e-9);   // <= 2 px
            if (l.path == QualityPath::Native) CHECK((l.fitW == s[0] && l.fitH == s[1]));
            if (l.path == QualityPath::Downscale) CHECK((l.fitW <= s[0] && l.fitH <= s[1]));
            if (l.path == QualityPath::UpscaleFsr4) CHECK(l.scale <= kFsr4MaxScale + 1e-9);
        }
    CHECK(PlanLayout(0, 720, 1920, 1080, Upscaler::Easu, true).fitW == 0);
}

GpuCaps Caps(Vendor v, const char* name, uint64_t gib, bool integrated = false) {
    GpuCaps c;
    c.vendor = v;
    c.name = name;
    c.dedicatedVram = gib << 30;
    c.integrated = integrated;
    return c;
}

TEST_CASE("Fsr4Tier by hardware generation and DetectPreset by vendor / VRAM / integrated") {
    struct G { GpuCaps caps; Fsr4Tier tier; QualityPreset preset; };
    const G gpus[] = {
        {Caps(Vendor::Amd, "AMD Radeon RX 6600", 8), Fsr4Tier::Slow, QualityPreset::Standard},
        {Caps(Vendor::Amd, "AMD Radeon RX 6500 XT", 4), Fsr4Tier::Slow, QualityPreset::Standard},
        {Caps(Vendor::Amd, "AMD Radeon RX 6900 XT", 16), Fsr4Tier::Slow, QualityPreset::Standard},
        {Caps(Vendor::Amd, "AMD Radeon RX 7600", 8), Fsr4Tier::Fast, QualityPreset::Standard},
        {Caps(Vendor::Amd, "AMD Radeon RX 7900 XTX", 24), Fsr4Tier::Fast, QualityPreset::Max},
        {Caps(Vendor::Amd, "AMD Radeon RX 9060 XT", 16), Fsr4Tier::Fast, QualityPreset::Max},
        {Caps(Vendor::Amd, "AMD Radeon RX 9070 XT", 16), Fsr4Tier::Fast, QualityPreset::Max},
        {Caps(Vendor::Amd, "AMD Radeon RX 580 Series", 8), Fsr4Tier::None, QualityPreset::Standard},
        {Caps(Vendor::Amd, "AMD Radeon RX 5700 XT", 8), Fsr4Tier::None, QualityPreset::Standard},
        {Caps(Vendor::Amd, "AMD Radeon 780M Graphics", 4, true), Fsr4Tier::Slow, QualityPreset::Light},
        {Caps(Vendor::Amd, "AMD Radeon(TM) Graphics", 2, true), Fsr4Tier::None, QualityPreset::Light},
        {Caps(Vendor::Amd, "AMD Radeon RX 6400", 4), Fsr4Tier::Slow, QualityPreset::Standard},
        {Caps(Vendor::Amd, "AMD Radeon RX 6300", 2), Fsr4Tier::Slow, QualityPreset::Light},   // < 4 GiB
        {Caps(Vendor::Nvidia, "NVIDIA GeForce RTX 4060", 8), Fsr4Tier::None, QualityPreset::Standard},
        {Caps(Vendor::Nvidia, "NVIDIA GeForce RTX 4090", 24), Fsr4Tier::None, QualityPreset::Max},
        {Caps(Vendor::Nvidia, "NVIDIA GeForce GTX 1650", 4), Fsr4Tier::None, QualityPreset::Light},
        {Caps(Vendor::Nvidia, "NVIDIA GeForce MX450", 2), Fsr4Tier::None, QualityPreset::Light},
        {Caps(Vendor::Intel, "Intel(R) UHD Graphics 770", 0, true), Fsr4Tier::None, QualityPreset::Light},
        {Caps(Vendor::Intel, "Intel(R) Arc(TM) A770 Graphics", 16), Fsr4Tier::None, QualityPreset::Standard},
        {Caps(Vendor::Intel, "Intel(R) Arc(TM) A380 Graphics", 6), Fsr4Tier::None, QualityPreset::Light},
        {Caps(Vendor::Unknown, "Mystery GPU", 8), Fsr4Tier::None, QualityPreset::Light},
    };
    for (const G& g : gpus) {
        INFO(g.caps.name);
        CHECK(ClassifyFsr4(g.caps) == g.tier);
        CHECK(DetectPreset(g.caps) == g.preset);
    }
}

TEST_CASE("ResolveChainConfig maps presets and overrides") {
    VideoSettings v;
    v.deblock = 0.7f;
    v.sharpness = 0.4f;

    v.preset = QualityPreset::Light;
    for (Fsr4Tier t : {Fsr4Tier::None, Fsr4Tier::Slow, Fsr4Tier::Fast}) {
        const ChainConfig c = ResolveChainConfig(v, t);
        CHECK(c.upscaler == Upscaler::Easu);
        CHECK(c.downscale == DownscaleFilter::Bicubic);
        CHECK(c.deblock == 0.f);
        CHECK(c.sharpness == doctest::Approx(0.4f));
    }
    v.preset = QualityPreset::Standard;
    CHECK(ResolveChainConfig(v, Fsr4Tier::None).upscaler == Upscaler::Easu);
    CHECK(ResolveChainConfig(v, Fsr4Tier::Slow).upscaler == Upscaler::Easu);
    CHECK(ResolveChainConfig(v, Fsr4Tier::Fast).upscaler == Upscaler::Fsr4);
    CHECK(ResolveChainConfig(v, Fsr4Tier::Fast).downscale == DownscaleFilter::Lanczos3);
    CHECK(ResolveChainConfig(v, Fsr4Tier::Fast).deblock == doctest::Approx(0.7f));
    v.preset = QualityPreset::Max;
    CHECK(ResolveChainConfig(v, Fsr4Tier::None).upscaler == Upscaler::Easu);
    CHECK(ResolveChainConfig(v, Fsr4Tier::Slow).upscaler == Upscaler::Fsr4);
    CHECK(ResolveChainConfig(v, Fsr4Tier::Fast).upscaler == Upscaler::Fsr4);
    CHECK(ResolveChainConfig(v, Fsr4Tier::Slow).downscale == DownscaleFilter::Lanczos3);

    // Overrides beat the preset in both directions.
    v.upscaler = UpscalerChoice::Easu;
    CHECK(ResolveChainConfig(v, Fsr4Tier::Fast).upscaler == Upscaler::Easu);
    v.preset = QualityPreset::Light;
    v.upscaler = UpscalerChoice::Fsr4;
    const ChainConfig forced = ResolveChainConfig(v, Fsr4Tier::None);
    CHECK(forced.upscaler == Upscaler::Fsr4);
    CHECK(forced.deblock == 0.f);   // the rest of the Light preset stays

    // Out-of-range settings are clamped.
    VideoSettings wild;
    wild.deblock = 3.f;
    wild.sharpness = -1.f;
    const ChainConfig w = ResolveChainConfig(wild, Fsr4Tier::None);
    CHECK(w.deblock == 1.f);
    CHECK(w.sharpness == 0.f);
}

TEST_CASE("NewFrameGate presents only new frames when asked, everything otherwise") {
    NewFrameGate off(false);
    for (int i = 0; i < 10; ++i) {
        const FrameDecision d = off.OnFrame(i % 2 == 0, i * 16.6);
        CHECK(d.process);
        CHECK(d.present);
    }
    CHECK(off.Skipped() == 0);

    NewFrameGate gate(true);
    int presented = 0;
    for (int i = 0; i < 60; ++i) {   // 60 Hz stream of a 30 fps game: every second frame is a repeat
        const FrameDecision d = gate.OnFrame(i % 2 == 0, i * (1000.0 / 60.0));
        CHECK(d.process == d.present);
        CHECK(d.present == (i % 2 == 0));
        presented += d.present;
    }
    CHECK(presented == 30);
    CHECK(gate.Processed() == 30);
    CHECK(gate.Skipped() == 30);
    CHECK(gate.ContentHz() == doctest::Approx(30.0).epsilon(0.01));

    CHECK(gate.OnFrame(false, 2000.0, true).present);    // overlay changed: redraw although the content did not
    CHECK(!gate.OnFrame(false, 2016.0).present);
    gate.SetNewFramesOnly(false);
    CHECK(gate.OnFrame(false, 2033.0).present);
}

// ---- the chain on the GPU -------------------------------------------------------------------------------------------

struct Fixture {
    TestGpu& gpu = TestGpu::Shared();
    Fixture() { if (gpu.Valid()) gpu.BeginTest(); }
    ~Fixture() {
        if (!gpu.Valid()) return;
        std::string report;
        const int errors = gpu.EndTest(&report);
        CHECK_MESSAGE(errors == 0, "D3D12 debug layer reported:\n" << report);
    }
};

#define REQUIRE_GPU(f) \
    do { if (!(f).gpu.Valid()) { MESSAGE("no hardware D3D12 adapter, skipped"); return; } } while (0)

UpscalerFactory FailingFactory(const char* why) {
    return [why](ID3D12Device*, const UpscaleSizes&, Status& status) -> std::unique_ptr<UpscalerBackend> {
        status = Status::Fail(why);
        return nullptr;
    };
}

struct Source {
    Image rgb;      // what the NV12 frame decodes to (8-bit, reference conversion)
    Nv12Frame frame;
    Tex nv12;
};

Source MakeSource(TestGpu& gpu, const Image& truthAtStreamSize) {
    Source s;
    s.frame = RgbToNv12(truthAtStreamSize, YuvMatrix::Bt709, false);
    s.nv12 = gpu.CreateNv12(s.frame.w, s.frame.h);
    gpu.UploadNv12(s.nv12, s.frame.y.data(), s.frame.uv.data());
    const auto bytes = ReferenceNv12ToRgb8(s.frame, {}, s.frame.w, s.frame.h);
    s.rgb = Image(s.frame.w, s.frame.h);
    for (size_t i = 0; i < s.rgb.px.size(); ++i) s.rgb.px[i] = bytes[i] / 255.f;
    return s;
}

// Records the chain once into a fresh RGBA8 output of outW x outH and downloads it.
bool RunChain(TestGpu& gpu, TestAllocator& alloc, QualityChain& chain, Source& src, uint32_t outW, uint32_t outH, Image* result) {
    Tex out = gpu.Create(outW, outH, kOutputFormat, true);
    bool ok = false;
    gpu.Run([&](ID3D12GraphicsCommandList* l) {
        gpu.Transition(l, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ok = chain.Record(l, src.nv12.gt, {}, out.gt);
    });
    (void)alloc;
    if (ok && result) *result = Download(gpu, out);
    return ok;
}

std::unique_ptr<QualityChain> MakeChain(TestGpu& gpu, TestAllocator& alloc, const ChainConfig& cfg, UpscalerFactory factory = {}) {
    ChainOptions o;
    o.config = cfg;
    o.fsr4Factory = std::move(factory);
    Status st;
    auto chain = QualityChain::Create(gpu.Device(), alloc, o, st);
    INFO(st.message);
    REQUIRE(chain);
    return chain;
}

ChainConfig Plain(Upscaler up = Upscaler::Easu) {
    ChainConfig c;
    c.upscaler = up;
    c.deblock = 0.f;
    c.sharpness = 0.f;
    return c;
}

TEST_CASE("chain, native path with bars (2560x1440 -> 3440x1440): picture centred 1:1, bars black, nothing stretched") {
    Fixture f;
    REQUIRE_GPU(f);
    TestAllocator alloc(f.gpu);
    Source src = MakeSource(f.gpu, Gradient(2560, 1440));
    auto chain = MakeChain(f.gpu, alloc, Plain());
    REQUIRE(chain->Configure(2560, 1440, 3440, 1440).ok);
    const ChainLayout& l = chain->Layout();
    CHECK(l.path == QualityPath::Native);
    CHECK(l.offX == 440);
    CHECK(l.HasBars());
    CHECK(chain->UpscalerName().empty());
    Image out;
    REQUIRE(RunChain(f.gpu, alloc, *chain, src, 3440, 1440, &out));
    REQUIRE(out.w == 3440);
    REQUIRE(out.h == 1440);
    double worst = 0, bars = 0;
    for (uint32_t y = 0; y < out.h; y += 7)
        for (uint32_t x = 0; x < out.w; ++x) {
            const bool inside = x >= 440 && x < 440 + 2560;
            for (int c = 0; c < 3; ++c) {
                if (inside) worst = std::max<double>(worst, std::abs(out.at(x, y, c) - src.rgb.at(x - 440, y, c)));
                else bars = std::max<double>(bars, out.at(x, y, c));
            }
            CHECK(out.at(x, y, 3) == doctest::Approx(1.0f));
        }
    CHECK(bars == 0.0);
    CHECK(worst <= 2.0 / 255.0);
}

TEST_CASE("chain, downscale 2560x1440 -> 1920x1080 and 16:10 (1920x1200) match the CPU reference") {
    Fixture f;
    REQUIRE_GPU(f);
    TestAllocator alloc(f.gpu);
    Source src = MakeSource(f.gpu, Noise(2560, 1440, 5, 24));
    ChainConfig cfg = Plain();
    auto chain = MakeChain(f.gpu, alloc, cfg);
    const Image expect = ReferenceDownscale(src.rgb, 1920, 1080, DownscaleFilter::Lanczos3);

    Image out;
    REQUIRE(RunChain(f.gpu, alloc, *chain, src, 1920, 1080, &out));
    CHECK(chain->Layout().path == QualityPath::Downscale);
    const double psnr = Psnr(out, expect);
    MESSAGE("1440p -> 1080p chain PSNR vs CPU downscale: " << psnr);
    CHECK(psnr > 38.0);

    // 16:10 monitor: same picture, 60 px bars top and bottom (resize: Configure happens inside Record here).
    REQUIRE(RunChain(f.gpu, alloc, *chain, src, 1920, 1200, &out));
    REQUIRE(chain->Layout().offY == 60);
    Image inner(1920, 1080);
    for (uint32_t y = 0; y < 1080; ++y)
        for (uint32_t x = 0; x < 1920; ++x)
            for (int c = 0; c < 4; ++c) inner.at(x, y, c) = out.at(x, y + 60, c);
    CHECK(Psnr(inner, expect) > 38.0);
    double bars = 0;
    for (uint32_t x = 0; x < 1920; x += 3)
        for (uint32_t y : {0u, 59u, 1140u, 1199u})
            for (int c = 0; c < 3; ++c) bars = std::max<double>(bars, out.at(x, y, c));
    CHECK(bars == 0.0);
}

TEST_CASE("chain, EASU upscale 1280x720 -> 1920x1080 and 1920x1080 -> 2560x1600 (bars) reconstruct a smooth image") {
    Fixture f;
    REQUIRE_GPU(f);
    TestAllocator alloc(f.gpu);
    auto chain = MakeChain(f.gpu, alloc, Plain(Upscaler::Easu));

    Source a = MakeSource(f.gpu, Gradient(1280, 720));
    Image out;
    REQUIRE(RunChain(f.gpu, alloc, *chain, a, 1920, 1080, &out));
    CHECK(chain->Layout().path == QualityPath::UpscaleEasu);
    CHECK(chain->UpscalerName() == "EASU");
    CHECK(chain->Fsr4Fallback().empty());   // EASU was what was asked for
    const double p1 = Psnr(out, Gradient(1920, 1080));
    MESSAGE("720p -> 1080p EASU PSNR vs the exact gradient: " << p1);
    CHECK(p1 > 35.0);

    Source b = MakeSource(f.gpu, Gradient(1920, 1080));
    REQUIRE(RunChain(f.gpu, alloc, *chain, b, 2560, 1600, &out));
    REQUIRE(chain->Layout().fitH == 1440);
    REQUIRE(chain->Layout().offY == 80);
    Image inner(2560, 1440);
    for (uint32_t y = 0; y < 1440; ++y)
        for (uint32_t x = 0; x < 2560; ++x)
            for (int c = 0; c < 4; ++c) inner.at(x, y, c) = out.at(x, y + 80, c);
    const double p2 = Psnr(inner, Gradient(2560, 1440));
    MESSAGE("1080p -> 1600p (fit 2560x1440) EASU PSNR: " << p2);
    CHECK(p2 > 35.0);
    CHECK(out.at(10, 5, 0) == 0.f);
    CHECK(out.at(10, 1595, 1) == 0.f);
}

TEST_CASE("chain falls back to EASU, with the reason, when FSR 4 cannot be created (forced) and matches the EASU config") {
    Fixture f;
    REQUIRE_GPU(f);
    TestAllocator alloc(f.gpu);
    Source src = MakeSource(f.gpu, Noise(640, 360, 3, 16));

    auto forced = MakeChain(f.gpu, alloc, Plain(Upscaler::Fsr4), FailingFactory("GPU sem suporte (teste)"));
    REQUIRE(forced->Configure(640, 360, 1280, 720).ok);
    CHECK(forced->Layout().path == QualityPath::UpscaleEasu);
    CHECK(forced->UpscalerName() == "EASU");
    CHECK(forced->Fsr4Fallback().find("GPU sem suporte") != std::string::npos);

    auto easu = MakeChain(f.gpu, alloc, Plain(Upscaler::Easu));
    Image a, b;
    REQUIRE(RunChain(f.gpu, alloc, *forced, src, 1280, 720, &a));
    REQUIRE(RunChain(f.gpu, alloc, *easu, src, 1280, 720, &b));
    CHECK(a.px == b.px);

    // A ratio over FSR 4's 3x also lands on EASU, with the ratio as the reason.
    auto tooFar = MakeChain(f.gpu, alloc, Plain(Upscaler::Fsr4), FailingFactory("nunca chamada"));
    REQUIRE(tooFar->Configure(640, 360, 2560, 1440).ok);
    CHECK(tooFar->Layout().path == QualityPath::UpscaleEasu);
    CHECK(tooFar->Fsr4Fallback().find("fora do alcance") != std::string::npos);
}

TEST_CASE("chain with the real FSR 4.1.2 backend on this GPU: runs, or falls back to EASU and says why") {
    Fixture f;
    REQUIRE_GPU(f);
    TestAllocator alloc(f.gpu);
    Source src = MakeSource(f.gpu, Gradient(1280, 720));
    auto chain = MakeChain(f.gpu, alloc, Plain(Upscaler::Fsr4));
    REQUIRE(chain->Configure(1280, 720, 1920, 1080).ok);
    MESSAGE("upscaler: " << chain->UpscalerName() << (chain->Fsr4Fallback().empty() ? std::string() : "  (fallback: " + chain->Fsr4Fallback() + ")"));
    Image out;
    REQUIRE(RunChain(f.gpu, alloc, *chain, src, 1920, 1080, &out));
    for (int i = 0; i < 3; ++i) REQUIRE(RunChain(f.gpu, alloc, *chain, src, 1920, 1080, &out));   // history reset every frame
    const double psnr = Psnr(out, Gradient(1920, 1080));
    MESSAGE("720p -> 1080p " << chain->UpscalerName() << " PSNR vs the exact gradient: " << psnr);
    CHECK(psnr > 30.0);
    if (chain->Layout().path != QualityPath::UpscaleFsr4) WARN_MESSAGE(false, "FSR 4 not active here: " << chain->Fsr4Fallback());
}

TEST_CASE("chain resizes: output and stream changes recreate the intermediates, in-flight frames stay valid") {
    Fixture f;
    REQUIRE_GPU(f);
    TestAllocator alloc(f.gpu);
    Source s720 = MakeSource(f.gpu, Gradient(1280, 720));
    Source s1080 = MakeSource(f.gpu, Gradient(1920, 1080));
    ChainConfig cfg = Plain();
    cfg.deblock = 0.5f;
    cfg.sharpness = 0.5f;
    auto chain = MakeChain(f.gpu, alloc, cfg);
    struct Step { Source* src; uint32_t w, h; QualityPath path; };
    const Step steps[] = {
        {&s1080, 1280, 720, QualityPath::Downscale}, {&s1080, 1920, 1080, QualityPath::Native}, {&s1080, 2560, 1440, QualityPath::UpscaleEasu},
        {&s720, 2560, 1440, QualityPath::UpscaleEasu}, {&s720, 1280, 720, QualityPath::Native}, {&s720, 3440, 1440, QualityPath::UpscaleEasu},
        {&s1080, 1000, 1000, QualityPath::Downscale},
    };
    for (const Step& s : steps) {
        INFO(s.w << "x" << s.h);
        REQUIRE(chain->Configure(s.src->frame.w, s.src->frame.h, s.w, s.h).ok);
        CHECK(chain->Layout().path == s.path);
        Image out;
        REQUIRE(RunChain(f.gpu, alloc, *chain, *s.src, s.w, s.h, &out));
        CHECK(out.w == s.w);
        CHECK(out.h == s.h);
        CHECK(chain->Layout().outW == s.w);
    }
    // Settings change without a Configure: deblock off, sharpness off, still fine.
    chain->SetConfig(Plain());
    Image out;
    REQUIRE(RunChain(f.gpu, alloc, *chain, s1080, 1000, 1000, &out));
}

TEST_CASE("chain honours the visible crop of a 16-aligned surface (1920x1088 decoded, 1920x1080 visible)") {
    Fixture f;
    REQUIRE_GPU(f);
    TestAllocator alloc(f.gpu);
    Source src = MakeSource(f.gpu, Gradient(1920, 1088));
    auto chain = MakeChain(f.gpu, alloc, Plain());
    Tex out = f.gpu.Create(1280, 720, kOutputFormat, true);
    Nv12Params p;
    p.visibleWidth = 1920;
    p.visibleHeight = 1080;
    f.gpu.Run([&](ID3D12GraphicsCommandList* l) {
        f.gpu.Transition(l, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        CHECK(chain->Record(l, src.nv12.gt, p, out.gt));
    });
    CHECK(chain->Layout().srcH == 1080);
    CHECK(chain->Layout().path == QualityPath::Downscale);
    CHECK(chain->Layout().fitH == 720);
}

TEST_CASE("chain refuses unusable inputs without recording") {
    Fixture f;
    REQUIRE_GPU(f);
    TestAllocator alloc(f.gpu);
    auto chain = MakeChain(f.gpu, alloc, Plain());
    CHECK(!chain->Configure(0, 720, 1920, 1080).ok);
    Tex out = f.gpu.Create(64, 64, kOutputFormat, true);
    f.gpu.Run([&](ID3D12GraphicsCommandList* l) { CHECK(!chain->Record(l, GpuTexture{}, {}, out.gt)); });
}

} // namespace
