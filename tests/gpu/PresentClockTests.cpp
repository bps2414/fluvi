// SPDX-License-Identifier: GPL-3.0-only
// PresentClock with synthetic QPC times, refresh-mode selection, WaitUntil precision. No GPU needed.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include "Display.h"   // src/gpu/present (internal)
#include "fluvi/core/Time.h"
#include "fluvi/gpu/PresentClock.h"

using namespace fluvi;
using namespace fluvi::gpu;

namespace {

struct Shown { int64_t qpc; bool generated; };

// Feeds `count` arrivals at `sourceHz` with uniform jitter, presents exactly at the planned times.
std::vector<Shown> Run(PresentClock& clock, double sourceHz, int count, double jitterMs, uint32_t seed = 1,
                       double lateFraction = 0, double lateMs = 0) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> jitter(0, jitterMs);
    std::uniform_real_distribution<double> coin(0, 1);
    const double interval = 1000.0 / sourceHz;
    const int64_t base = time::MsToQpc(1000);
    std::vector<Shown> shown;
    for (int i = 0; i < count; ++i) {
        double ms = i * interval + jitter(rng);
        if (coin(rng) < lateFraction) ms += lateMs;
        const int64_t ready = base + time::MsToQpc(ms);
        const PresentPlan p = clock.Plan(ready);
        if (p.showGenerated) {
            CHECK(p.generatedAtQpc >= ready - time::MsToQpc(3));
            shown.push_back({p.generatedAtQpc, true});
            clock.OnPresented(p.generatedAtQpc, true);
        }
        const int64_t real = p.realAtQpc ? p.realAtQpc : ready;
        shown.push_back({real, false});
        clock.OnPresented(real, false);
    }
    return shown;
}

std::vector<double> Intervals(const std::vector<Shown>& s, size_t skip) {
    std::vector<double> v;
    for (size_t i = skip + 1; i < s.size(); ++i) v.push_back(time::ElapsedMs(s[i - 1].qpc, s[i].qpc));
    return v;
}

} // namespace

TEST_CASE("Smooth 30->60 with VRR spaces real and generated frames evenly") {
    auto clock = PresentClock::Create();
    clock->Configure({StreamMode::Smooth, 165, true, 30});
    const auto shown = Run(*clock, 30, 600, 1.0);
    // Alternates generated / real after the first frame.
    for (size_t i = 1; i + 1 < shown.size(); i += 2) {
        CHECK(shown[i].generated);
        CHECK_FALSE(shown[i + 1].generated);
    }
    auto iv = Intervals(shown, 60);
    double sum = 0;
    for (double v : iv) sum += v;
    const double mean = sum / double(iv.size());
    std::sort(iv.begin(), iv.end());
    const double p05 = iv[iv.size() / 20], p95 = iv[iv.size() * 19 / 20];
    MESSAGE("mean ", mean, " p05 ", p05, " p95 ", p95, " min ", iv.front(), " max ", iv.back());
    CHECK(mean == doctest::Approx(1000.0 / 60).epsilon(0.01));
    CHECK(p05 > 16.67 - 1.0);
    CHECK(p95 < 16.67 + 1.0);
    CHECK(clock->EstimatedSourceIntervalMs() == doctest::Approx(1000.0 / 30).epsilon(0.002));
}

TEST_CASE("source interval tracks 59.94 vs 60") {
    auto clock = PresentClock::Create();
    clock->Configure({StreamMode::Play, 165, true, 60});
    Run(*clock, 60000.0 / 1001.0, 900, 2.0, 7);
    const double ntsc = clock->EstimatedSourceIntervalMs();
    MESSAGE("59.94 estimate ", ntsc, " ms");
    CHECK(std::abs(ntsc - 1001.0 / 60.0) < 0.006);

    auto clock60 = PresentClock::Create();
    clock60->Configure({StreamMode::Play, 165, true, 60});
    Run(*clock60, 60, 900, 2.0, 9);
    const double exact = clock60->EstimatedSourceIntervalMs();
    CHECK(std::abs(exact - 1000.0 / 60.0) < 0.006);
    CHECK(ntsc > exact);
}

TEST_CASE("jitter outliers and dropped frames don't move the estimate") {
    auto clock = PresentClock::Create();
    clock->Configure({StreamMode::Play, 165, true, 60});
    // 10% of frames 9 ms late (a late + early pair), plus 1 ms jitter.
    Run(*clock, 60, 900, 1.0, 3, 0.10, 9.0);
    CHECK(std::abs(clock->EstimatedSourceIntervalMs() - 1000.0 / 60.0) < 0.05);

    // Every 10th frame missing (2x interval): counted as two frames, not a slower source.
    auto clock2 = PresentClock::Create();
    clock2->Configure({StreamMode::Play, 165, true, 60});
    const int64_t base = time::MsToQpc(1000);
    for (int i = 0; i < 600; ++i)
        if (i % 10 != 5) clock2->Plan(base + time::MsToQpc(i * 1000.0 / 60));
    CHECK(std::abs(clock2->EstimatedSourceIntervalMs() - 1000.0 / 60.0) < 0.02);

    // Wrong configured rate (said 60, gets 30): re-seeds and converges.
    auto clock3 = PresentClock::Create();
    clock3->Configure({StreamMode::Play, 165, true, 60});
    Run(*clock3, 30, 300, 0.5, 5);
    CHECK(clock3->EstimatedSourceIntervalMs() == doctest::Approx(1000.0 / 30).epsilon(0.01));
}

TEST_CASE("without VRR, times snap to refresh boundaries") {
    SUBCASE("120 Hz, 30 -> 60: exactly two refreshes apart") {
        auto clock = PresentClock::Create();
        clock->Configure({StreamMode::Smooth, 120, false, 30});
        const auto shown = Run(*clock, 30, 300, 1.0, 11);
        const double r = 1000.0 / 120;
        const auto iv = Intervals(shown, 30);
        size_t even = 0;
        for (double v : iv) {
            const double k = v / r;
            CHECK(std::abs(k - std::round(k)) < 0.01);   // whole refreshes
            even += std::round(k) == 2;
        }
        CHECK(double(even) / double(iv.size()) > 0.97);
    }
    SUBCASE("165 Hz: every interval is a whole number of refreshes") {
        auto clock = PresentClock::Create();
        clock->Configure({StreamMode::Smooth, 165, false, 30});
        const auto shown = Run(*clock, 30, 300, 1.0, 13);
        const double r = 1000.0 / 165;
        for (double v : Intervals(shown, 30)) {
            const double k = v / r;
            CHECK(std::abs(k - std::round(k)) < 0.01);
            CHECK(std::round(k) >= 1);
        }
    }
}

TEST_CASE("Play mode presents now") {
    auto clock = PresentClock::Create();
    clock->Configure({StreamMode::Play, 165, true, 60});
    const auto p = clock->Plan(time::NowQpc());
    CHECK_FALSE(p.showGenerated);
    CHECK(p.realAtQpc == 0);
}

TEST_CASE("WaitUntil is precise") {
    double worst = 0;
    for (int i = 0; i < 20; ++i) {
        const int64_t target = time::NowQpc() + time::MsToQpc(3.0 + i * 0.37);
        PresentClock::WaitUntil(target);
        const double late = time::ElapsedMs(target, time::NowQpc());
        CHECK(late >= 0);
        worst = std::max(worst, late);
    }
    MESSAGE("worst overshoot ", worst, " ms");
    CHECK(worst < 0.5);
    const int64_t before = time::NowQpc();
    PresentClock::WaitUntil(before - time::MsToQpc(5));   // already past
    CHECK(time::ElapsedMs(before, time::NowQpc()) < 1.0);
}

TEST_CASE("refresh override picks the closest mode with the same size") {
    using detail::DisplayModeCandidate;
    const std::vector<DisplayModeCandidate> modes = {
        {1920, 1080, 32, 60},  {1920, 1080, 32, 59},  {1920, 1080, 32, 100}, {1920, 1080, 32, 119},
        {1920, 1080, 32, 120}, {1920, 1080, 32, 144}, {1920, 1080, 32, 165}, {1280, 720, 32, 120},
        {1920, 1080, 16, 120},
    };
    auto pick = [&](double hz) { return modes[size_t(detail::SelectRefreshMode(modes, 1920, 1080, 32, hz))].frequency; };
    CHECK(pick(120) == 120);
    CHECK(pick(119.88) == 119);   // 119 is the 119.88 NTSC mode
    CHECK(pick(60) == 60);
    CHECK(pick(59.94) == 59);
    CHECK(pick(165) == 165);
    CHECK(pick(240) == 165);
    CHECK(pick(130) == 120);
    CHECK(modes[size_t(detail::SelectRefreshMode(modes, 1920, 1080, 32, 120))].bitsPerPixel == 32);
    CHECK(detail::SelectRefreshMode(modes, 2560, 1440, 32, 120) == -1);
    CHECK(detail::EffectiveHz(59) == doctest::Approx(59.94).epsilon(0.0001));
    CHECK(detail::EffectiveHz(165) == 165);
    CHECK(detail::EffectiveHz(164) == 164);
}
