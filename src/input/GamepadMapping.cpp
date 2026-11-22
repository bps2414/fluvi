// SPDX-License-Identifier: GPL-3.0-only
#include "GamepadMapping.h"
#include <algorithm>
#include <cmath>

namespace fluvi::input {

int16_t StickFromFloat(float v) {
    if (!(v == v)) return 0;                      // NaN
    v = std::clamp(v, -1.0f, 1.0f);
    return static_cast<int16_t>(std::lround(v * 32767.0f));
}

uint16_t TriggerFromFloat(float v) {
    if (!(v == v)) return 0;
    v = std::clamp(v, 0.0f, 1.0f);
    return static_cast<uint16_t>(std::lround(v * 65535.0f));
}

int16_t StickFromShort(int16_t v) { return v < -32767 ? int16_t(-32767) : v; }
uint16_t TriggerFromByte(uint8_t v) { return static_cast<uint16_t>(v * 257u); }

GamepadState MapGameInput(const RawGameInputPad& raw) {
    struct Pair { uint32_t from; uint16_t to; };
    static constexpr Pair kMap[] = {
        {kGiMenu, BtnMenu}, {kGiView, BtnView}, {kGiA, BtnA}, {kGiB, BtnB}, {kGiX, BtnX}, {kGiY, BtnY},
        {kGiUp, BtnUp}, {kGiDown, BtnDown}, {kGiLeft, BtnLeft}, {kGiRight, BtnRight},
        {kGiLeftShoulder, BtnLeftShoulder}, {kGiRightShoulder, BtnRightShoulder},
        {kGiLeftThumb, BtnLeftThumb}, {kGiRightThumb, BtnRightThumb},
    };
    GamepadState s;
    uint16_t b = 0;
    for (const auto& p : kMap)
        if (raw.buttons & p.from) b |= p.to;
    if (raw.guide) b |= BtnNexus;
    s.buttons = b;
    s.leftX = StickFromFloat(raw.leftX);
    s.leftY = StickFromFloat(raw.leftY);
    s.rightX = StickFromFloat(raw.rightX);
    s.rightY = StickFromFloat(raw.rightY);
    s.leftTrigger = TriggerFromFloat(raw.leftTrigger);
    s.rightTrigger = TriggerFromFloat(raw.rightTrigger);
    return s;
}

GamepadState MapXInput(const RawXInputPad& raw) {
    struct Pair { uint16_t from; uint16_t to; };
    static constexpr Pair kMap[] = {
        {kXiStart, BtnMenu}, {kXiBack, BtnView}, {kXiGuide, BtnNexus},
        {kXiA, BtnA}, {kXiB, BtnB}, {kXiX, BtnX}, {kXiY, BtnY},
        {kXiUp, BtnUp}, {kXiDown, BtnDown}, {kXiLeft, BtnLeft}, {kXiRight, BtnRight},
        {kXiLeftShoulder, BtnLeftShoulder}, {kXiRightShoulder, BtnRightShoulder},
        {kXiLeftThumb, BtnLeftThumb}, {kXiRightThumb, BtnRightThumb},
    };
    GamepadState s;
    uint16_t b = 0;
    for (const auto& p : kMap)
        if (raw.buttons & p.from) b |= p.to;
    s.buttons = b;
    s.leftX = StickFromShort(raw.leftX);
    s.leftY = StickFromShort(raw.leftY);
    s.rightX = StickFromShort(raw.rightX);
    s.rightY = StickFromShort(raw.rightY);
    s.leftTrigger = TriggerFromByte(raw.leftTrigger);
    s.rightTrigger = TriggerFromByte(raw.rightTrigger);
    return s;
}

float RumbleLevel(uint8_t percent, uint8_t strength) {
    const float p = std::min<uint8_t>(percent, 100) / 100.0f;
    const float k = std::min<uint8_t>(strength, 100) / 100.0f;
    return p * k;
}

uint16_t RumbleToMotor(uint8_t percent, uint8_t strength) {
    return static_cast<uint16_t>(std::lround(RumbleLevel(percent, strength) * 65535.0f));
}

} // namespace fluvi::input
