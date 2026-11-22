// SPDX-License-Identifier: GPL-3.0-only
// Label "device": opens the default render endpoint through the real OpusPlayer and plays ~0.5 s of an Opus stream
// at volume 0 (nothing audible), checking that WASAPI, the render thread and the counters behave.
#include <doctest/doctest.h>
#include <chrono>
#include <thread>
#include "OpusTestStream.h"
#include "fluvi/audio/OpusPlayer.h"

using namespace fluvi;
using namespace fluvi::audio;
using namespace std::chrono;

TEST_CASE("OpusPlayer plays on the default endpoint at volume 0") {
    const test::OpusStream stream = test::EncodeSine(26);   // 520 ms
    OpusPlayerConfig cfg;
    cfg.volume = 0.0f;
    cfg.targetBufferMs = 30;
    Status st;
    auto player = OpusPlayer::Create(cfg, st);
    REQUIRE_MESSAGE(player, st.message);
    CHECK(st.ok);

    const auto t0 = steady_clock::now();
    for (size_t i = 0; i < stream.packets.size(); ++i) {
        std::this_thread::sleep_until(t0 + milliseconds(20) * i);
        player->Push(stream.packets[i].data(), stream.packets[i].size(), uint32_t(i * test::kFrame));
    }
    std::this_thread::sleep_for(milliseconds(150));
    const AudioStats stats = player->Stats();
    MESSAGE("device latency ", stats.deviceLatencyMs, " ms, buffered ", stats.bufferedMs, " ms, underruns ",
            stats.underruns, ", drift ", stats.driftPpm, " ppm");
    CHECK(stats.deviceLatencyMs > 0.0);            // an endpoint was opened (period + stream latency)
    CHECK(stats.deviceLatencyMs < 100.0);
    CHECK(stats.packets == 26);
    CHECK(stats.lostPackets == 0);
    CHECK(stats.overflowDrops == 0);
    CHECK(stats.underruns <= 1);                   // sleep jitter of the pacing loop may cost one

    player->SetVolume(0.0f);
    player->Flush();
    std::this_thread::sleep_for(milliseconds(50));
    CHECK(player->Stats().bufferedMs == doctest::Approx(0.0));
}
