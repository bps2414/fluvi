// SPDX-License-Identifier: GPL-3.0-only
// Fluvi: the controller as the Xbox input report sees it. The input module produces GamepadState, the
// xhome module encodes it (binary "input" channel) and decodes Rumble from the console. Values and
// conventions are the report's, so nobody converts twice.
#pragma once
#include <cstdint>

namespace fluvi {

// Bit values of the report's button field (Greenlight packages/player input, via Veyra Protocol.h).
enum GamepadButton : uint16_t {
    BtnNexus = 2, BtnMenu = 4, BtnView = 8,
    BtnA = 16, BtnB = 32, BtnX = 64, BtnY = 128,
    BtnUp = 256, BtnDown = 512, BtnLeft = 1024, BtnRight = 2048,
    BtnLeftShoulder = 4096, BtnRightShoulder = 8192, BtnLeftThumb = 16384, BtnRightThumb = 32768,
};

struct GamepadState {
    uint16_t buttons = 0;                                  // GamepadButton bits
    int16_t leftX = 0, leftY = 0, rightX = 0, rightY = 0;  // -32767..32767, right and UP are positive; raw (no deadzone, the console applies its own)
    uint16_t leftTrigger = 0, rightTrigger = 0;            // 0..65535
    bool operator==(const GamepadState&) const = default;
};

// Vibration request from the console. Percent 0..100 per motor.
struct Rumble {
    uint8_t leftMotor = 0, rightMotor = 0, leftTrigger = 0, rightTrigger = 0;
    uint16_t durationMs = 0, delayMs = 0;
    uint8_t repeat = 0;
};

} // namespace fluvi
