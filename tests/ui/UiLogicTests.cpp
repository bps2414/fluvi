// SPDX-License-Identifier: GPL-3.0-only
// Fluvi ui: pure logic without a GPU: "[key]" translation, the layout scale, stream health and the session clock.
#include <doctest/doctest.h>

#include "UiHelpers.h"
#include "UiStats.h"
#include "fluvi/common/I18n.h"

using namespace fluvi;
using namespace fluvi::ui;

namespace {

void InstallTestCatalog() {
    i18n::Catalog pt;
    Status status;
    REQUIRE(pt.AddJson(R"({"common.back": "Voltar", "stream.ms": "ms", "consoles.title": "Seus consoles"})", "test", status));
    i18n::Install("pt-BR", pt, pt);
}

} // namespace

TEST_CASE("TranslateBrackets replaces [group.key] and leaves everything else alone") {
    InstallTestCatalog();
    int count = 0;
    CHECK(TranslateBrackets("[common.back]", &count) == "Voltar");
    CHECK(count == 1);
    CHECK(TranslateBrackets("  [consoles.title] e [stream.ms]  ", &count) == "  Seus consoles e ms  ");
    CHECK(count == 2);
    // Not keys: no dot, upper case, a trailing dot, unclosed, empty, arrays in data expressions.
    for (const char* text : {"[back]", "[Common.back]", "[common.]", "[common.back", "[]", "{{ list[0] }}", "a [b c.d]"}) {
        CHECK(TranslateBrackets(text, &count) == text);
        CHECK(count == 0);
    }
}

TEST_CASE("DpRatio scales from the 1600x900 design size, between 1x and 1.5x of the monitor DPI") {
    CHECK(DpRatio(1600, 900, 1.0f) == doctest::Approx(1.0));
    CHECK(DpRatio(1120, 630, 1.0f) == doctest::Approx(1.0));     // smaller windows keep 1 dp = 1 px
    CHECK(DpRatio(1920, 1080, 1.0f) == doctest::Approx(1.2));
    CHECK(DpRatio(2560, 1440, 1.0f) == doctest::Approx(1.5));    // clamped at 1.5
    CHECK(DpRatio(3840, 2160, 1.5f) == doctest::Approx(2.25));   // 4K at 150%: 1.5 * 1.5 (clamped) = 2.25
    CHECK(DpRatio(1920, 1080, 1.5f) == doctest::Approx(1.5));    // 1080p at 150%: never below the DPI
    CHECK(DpRatio(2560, 1080, 1.0f) == doctest::Approx(1.2));    // ultrawide: the height decides
    CHECK(DpRatio(1600, 900, 0.0f) == doctest::Approx(1.0));     // unknown DPI falls back to 1
}

TEST_CASE("ClassifyHealth bands by total latency, jitter and losses") {
    CHECK(ClassifyHealth(20, 1, 0) == HealthBand::Great);
    CHECK(ClassifyHealth(40, 1, 0) == HealthBand::Good);
    CHECK(ClassifyHealth(80, 1, 0) == HealthBand::Unsteady);
    CHECK(ClassifyHealth(20, 15, 0) == HealthBand::Unsteady);
    CHECK(ClassifyHealth(20, 1, 1) == HealthBand::Unsteady);
    CHECK(ClassifyHealth(130, 1, 0) == HealthBand::Poor);
    CHECK(ClassifyHealth(20, 1, 25) == HealthBand::Poor);
}

TEST_CASE("StreamHealth keeps a 10 s window and draws it as bars") {
    StreamHealth h;
    CHECK(h.Empty());
    CHECK(h.Bars(0, 4) == std::vector<int>{0, 0, 0, 0});
    h.Add(0.0, 10, 20, 5);
    h.Add(5.0, 15, 15, 5);
    CHECK(h.LastTotalMs() == doctest::Approx(30));
    CHECK(h.NewLosses() == 0);
    CHECK(h.Band() == HealthBand::Great);
    // Window [0, 10]: four 2.5 s slots, scale max(60, 1.15 * 30) = 60 ms.
    CHECK(h.Bars(10.0, 4) == std::vector<int>{50, 50, 50, 50});
    h.Add(12.0, 60, 40, 9);   // the 0 s sample leaves the window; 4 packets lost inside it
    CHECK(h.NewLosses() == 4);
    CHECK(h.Band() == HealthBand::Unsteady);
    const auto bars = h.Bars(12.0, 4);
    CHECK(bars.front() == 0);       // before the first sample in the window
    CHECK(bars.back() == 87);       // 100 / 115
    // A counter that goes backwards is a new session.
    h.Add(13.0, 10, 10, 0);
    CHECK(h.NewLosses() == 0);
    CHECK(h.LastTotalMs() == doctest::Approx(20));
    h.Reset();
    CHECK(h.Empty());
}

TEST_CASE("FormatSessionTime shows minutes and seconds, hours when needed") {
    CHECK(FormatSessionTime(0) == "0:00");
    CHECK(FormatSessionTime(42.9) == "0:42");
    CHECK(FormatSessionTime(725) == "12:05");
    CHECK(FormatSessionTime(3723) == "1:02:03");
    CHECK(FormatSessionTime(-5) == "0:00");
}
