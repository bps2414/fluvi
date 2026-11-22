// SPDX-License-Identifier: GPL-3.0-only
// Input module: pure mapping, rate meter, rumble timeline (+ engine with a recording callback).
// None of it needs a pad.
#include <doctest/doctest.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#include "GamepadMapping.h"
#include "RateMeter.h"
#include "RumbleEngine.h"
#include "fluvi/input/GamepadReader.h"

using namespace fluvi;
using namespace fluvi::input;

TEST_CASE("sticks: float to report range, up positive, clamped") {
    CHECK(StickFromFloat(0.0f) == 0);
    CHECK(StickFromFloat(1.0f) == 32767);
    CHECK(StickFromFloat(-1.0f) == -32767);
    CHECK(StickFromFloat(2.5f) == 32767);
    CHECK(StickFromFloat(-9.0f) == -32767);
    CHECK(StickFromFloat(0.5f) == 16384);
    CHECK(StickFromFloat(std::nanf("")) == 0);
    CHECK(StickFromShort(-32768) == -32767);
    CHECK(StickFromShort(32767) == 32767);
    CHECK(StickFromShort(-123) == -123);
}

TEST_CASE("triggers: 0..65535") {
    CHECK(TriggerFromFloat(0.0f) == 0);
    CHECK(TriggerFromFloat(1.0f) == 65535);
    CHECK(TriggerFromFloat(3.0f) == 65535);
    CHECK(TriggerFromFloat(-0.2f) == 0);
    CHECK(TriggerFromFloat(0.5f) == 32768);
    CHECK(TriggerFromByte(0) == 0);
    CHECK(TriggerFromByte(255) == 65535);
    CHECK(TriggerFromByte(128) == 128 * 257);
}

TEST_CASE("GameInput mapping: every button and the axes") {
    struct Case { uint32_t gi; uint16_t out; };
    const Case cases[] = {
        {kGiMenu, BtnMenu}, {kGiView, BtnView}, {kGiA, BtnA}, {kGiB, BtnB}, {kGiX, BtnX}, {kGiY, BtnY},
        {kGiUp, BtnUp}, {kGiDown, BtnDown}, {kGiLeft, BtnLeft}, {kGiRight, BtnRight},
        {kGiLeftShoulder, BtnLeftShoulder}, {kGiRightShoulder, BtnRightShoulder},
        {kGiLeftThumb, BtnLeftThumb}, {kGiRightThumb, BtnRightThumb},
    };
    for (const auto& c : cases) {
        RawGameInputPad raw;
        raw.buttons = c.gi;
        CHECK(MapGameInput(raw).buttons == c.out);
    }
    RawGameInputPad raw;
    raw.guide = true;
    CHECK(MapGameInput(raw).buttons == BtnNexus);
    raw = {};
    raw.buttons = kGiA | kGiLeftShoulder;
    raw.leftX = -1; raw.leftY = 1; raw.rightX = 0.25f; raw.rightY = -0.25f;
    raw.leftTrigger = 1; raw.rightTrigger = 0.5f;
    const GamepadState s = MapGameInput(raw);
    CHECK(s.buttons == (BtnA | BtnLeftShoulder));
    CHECK(s.leftX == -32767);
    CHECK(s.leftY == 32767);        // GameInput Y is already up-positive
    CHECK(s.rightX == 8192);
    CHECK(s.rightY == -8192);
    CHECK(s.leftTrigger == 65535);
    CHECK(s.rightTrigger == 32768);
    CHECK(MapGameInput({}) == GamepadState{});
}

TEST_CASE("XInput mapping: every button, Guide -> Nexus, sticks clamped") {
    struct Case { uint16_t xi; uint16_t out; };
    const Case cases[] = {
        {kXiStart, BtnMenu}, {kXiBack, BtnView}, {kXiGuide, BtnNexus}, {kXiA, BtnA}, {kXiB, BtnB}, {kXiX, BtnX},
        {kXiY, BtnY}, {kXiUp, BtnUp}, {kXiDown, BtnDown}, {kXiLeft, BtnLeft}, {kXiRight, BtnRight},
        {kXiLeftShoulder, BtnLeftShoulder}, {kXiRightShoulder, BtnRightShoulder},
        {kXiLeftThumb, BtnLeftThumb}, {kXiRightThumb, BtnRightThumb},
    };
    for (const auto& c : cases) {
        RawXInputPad raw;
        raw.buttons = c.xi;
        CHECK(MapXInput(raw).buttons == c.out);
    }
    RawXInputPad raw;
    raw.leftX = -32768; raw.leftY = 32767; raw.rightX = 100; raw.rightY = -32768;
    raw.leftTrigger = 255; raw.rightTrigger = 1;
    const GamepadState s = MapXInput(raw);
    CHECK(s.leftX == -32767);
    CHECK(s.leftY == 32767);
    CHECK(s.rightX == 100);
    CHECK(s.rightY == -32767);
    CHECK(s.leftTrigger == 65535);
    CHECK(s.rightTrigger == 257);
}

TEST_CASE("the same physical state maps equal on both backends") {
    RawGameInputPad g;
    g.buttons = kGiB | kGiRightThumb | kGiDown;
    g.leftX = 1; g.leftY = -1; g.leftTrigger = 1;
    RawXInputPad x;
    x.buttons = kXiB | kXiRightThumb | kXiDown;
    x.leftX = 32767; x.leftY = -32768; x.leftTrigger = 255;
    CHECK(MapGameInput(g) == MapXInput(x));
}

TEST_CASE("rumble scaling by strength") {
    CHECK(RumbleLevel(100, 100) == doctest::Approx(1.0f));
    CHECK(RumbleLevel(50, 100) == doctest::Approx(0.5f));
    CHECK(RumbleLevel(100, 50) == doctest::Approx(0.5f));
    CHECK(RumbleLevel(50, 50) == doctest::Approx(0.25f));
    CHECK(RumbleLevel(200, 100) == doctest::Approx(1.0f));   // out-of-range percent is clamped
    CHECK(RumbleToMotor(100, 100) == 65535);
    CHECK(RumbleToMotor(100, 0) == 0);
}

TEST_CASE("RateMeter: steady 1 kHz reads as ~1000/s and decays when idle") {
    RateMeter m;
    for (int ms = 0; ms < 3000; ++ms) m.Add(double(ms));
    CHECK(m.PerSecond(2999.0) == doctest::Approx(1000.0).epsilon(0.02));
    CHECK(m.PerSecond(4500.0) < 400.0);
    CHECK(m.PerSecond(9000.0) == doctest::Approx(0.0));
}

TEST_CASE("RumbleSchedule: single pulse, default duration, repeats, strength") {
    RumbleSchedule s;
    double next = 0;
    Rumble r;
    r.leftMotor = 100; r.rightMotor = 50; r.durationMs = 100;
    s.Start(r, 100, 1000.0);
    auto lv = s.Poll(1000.0, next);
    CHECK(lv.lowFrequency == doctest::Approx(1.0f));
    CHECK(lv.highFrequency == doctest::Approx(0.5f));
    CHECK(next == doctest::Approx(1100.0));
    CHECK(s.Poll(1099.0, next).Any());
    CHECK_FALSE(s.Poll(1100.0, next).Any());
    CHECK(next < 0);

    // strength scales the levels
    s.Start(r, 50, 0.0);
    CHECK(s.Poll(10.0, next).lowFrequency == doctest::Approx(0.5f));

    // duration 0 -> safety default, never stuck on
    r.durationMs = 0;
    s.Start(r, 100, 0.0);
    CHECK(s.Poll(999.0, next).Any());
    CHECK_FALSE(s.Poll(1000.0, next).Any());

    // 3 pulses of 100 ms with 50 ms gaps
    r.durationMs = 100; r.delayMs = 50; r.repeat = 2;
    s.Start(r, 100, 0.0);
    CHECK(s.Poll(50.0, next).Any());   CHECK(next == doctest::Approx(100.0));
    CHECK_FALSE(s.Poll(120.0, next).Any()); CHECK(next == doctest::Approx(150.0));
    CHECK(s.Poll(160.0, next).Any());  CHECK(next == doctest::Approx(250.0));
    CHECK_FALSE(s.Poll(260.0, next).Any()); CHECK(next == doctest::Approx(300.0));
    CHECK(s.Poll(310.0, next).Any());  CHECK(next == doctest::Approx(400.0));
    CHECK_FALSE(s.Poll(400.0, next).Any());
    CHECK(next < 0);

    // all-zero request is a stop
    Rumble z;
    z.durationMs = 500;
    s.Start(z, 100, 0.0);
    CHECK_FALSE(s.Poll(10.0, next).Any());
}

TEST_CASE("RumbleEngine: applies on change, turns itself off, ignores duplicates") {
    struct Call { uint8_t index; MotorLevels levels; };
    std::mutex m;
    std::vector<Call> calls;
    {
        RumbleEngine engine([&](uint8_t i, const MotorLevels& l) {
            std::lock_guard lock(m);
            calls.push_back({i, l});
        });
        Rumble r;
        r.leftMotor = 100; r.durationMs = 60;
        engine.Set(0, r, 100);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::lock_guard lock(m);
        REQUIRE(calls.size() == 2);
        CHECK(calls[0].levels.lowFrequency == doctest::Approx(1.0f));
        CHECK_FALSE(calls[1].levels.Any());
    }
    // a motor still running when the engine dies is switched off
    calls.clear();
    {
        RumbleEngine engine([&](uint8_t i, const MotorLevels& l) {
            std::lock_guard lock(m);
            calls.push_back({i, l});
        });
        Rumble r;
        r.rightMotor = 100; r.durationMs = 5000;
        engine.Set(1, r, 100);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::lock_guard lock(m);
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].index == 1);
    CHECK_FALSE(calls.back().levels.Any());
}

TEST_CASE("reader factory: None gives no reader, backends can be created without a pad") {
    CHECK(GamepadReader::Create(Backend::None) == nullptr);
    auto xi = GamepadReader::Create(Backend::XInput);
    REQUIRE(xi);
    CHECK(xi->Stats().backend == Backend::None);   // not started
    CHECK(xi->Current(0) == GamepadState{});
}

TEST_CASE("reader starts and stops cleanly with no pad connected") {
    for (Backend b : {Backend::GameInput, Backend::XInput}) {
        auto reader = GamepadReader::Create(b);
        REQUIRE(reader);
        std::atomic<int> events{0};
        REQUIRE(reader->Start([&](const GamepadEvent&) { ++events; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const ReaderStats st = reader->Stats();
        CHECK(st.backend != Backend::None);
        reader->SetRumble(0, Rumble{50, 50, 0, 0, 20, 0, 0}, 100);   // must be harmless without a pad
        reader->Stop();
        CHECK(reader->Stats().backend == Backend::None);
    }
}
