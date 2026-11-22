// SPDX-License-Identifier: GPL-3.0-only
// Pure mapping from backend reports to fluvi::GamepadState (and rumble levels back to motor values).
// No Windows headers here so the functions are unit-testable.
#pragma once
#include <cstdint>
#include "fluvi/common/Gamepad.h"

namespace fluvi::input {

// Mirror of GameInputGamepadState (GameInput.h, SDK 10.0.26100): buttons bitmask + floats.
struct RawGameInputPad {
    uint32_t buttons = 0;
    float leftTrigger = 0, rightTrigger = 0;               // 0..1
    float leftX = 0, leftY = 0, rightX = 0, rightY = 0;    // -1..1, up positive
    bool guide = false;                                    // system button (separate callback)
};

// Mirror of XINPUT_GAMEPAD.
struct RawXInputPad {
    uint16_t buttons = 0;
    uint8_t leftTrigger = 0, rightTrigger = 0;
    int16_t leftX = 0, leftY = 0, rightX = 0, rightY = 0;  // -32768..32767, up positive
};

// GameInputGamepadButtons bit values.
inline constexpr uint32_t kGiMenu = 0x1, kGiView = 0x2, kGiA = 0x4, kGiB = 0x8, kGiX = 0x10, kGiY = 0x20,
                          kGiUp = 0x40, kGiDown = 0x80, kGiLeft = 0x100, kGiRight = 0x200,
                          kGiLeftShoulder = 0x400, kGiRightShoulder = 0x800,
                          kGiLeftThumb = 0x1000, kGiRightThumb = 0x2000;

// XINPUT_GAMEPAD_* bit values (GUIDE is the undocumented 0x0400 of XInputGetStateEx).
inline constexpr uint16_t kXiUp = 0x1, kXiDown = 0x2, kXiLeft = 0x4, kXiRight = 0x8, kXiStart = 0x10,
                          kXiBack = 0x20, kXiLeftThumb = 0x40, kXiRightThumb = 0x80,
                          kXiLeftShoulder = 0x100, kXiRightShoulder = 0x200, kXiGuide = 0x400,
                          kXiA = 0x1000, kXiB = 0x2000, kXiX = 0x4000, kXiY = 0x8000;

// float -1..1 -> -32767..32767 (clamped, rounded; NaN -> 0).
int16_t StickFromFloat(float v);
// float 0..1 -> 0..65535 (clamped, rounded; NaN -> 0).
uint16_t TriggerFromFloat(float v);
// XInput SHORT -> -32767..32767 (-32768 is clamped).
int16_t StickFromShort(int16_t v);
// XInput BYTE 0..255 -> 0..65535 (x * 257, so 255 -> 65535).
uint16_t TriggerFromByte(uint8_t v);

GamepadState MapGameInput(const RawGameInputPad& raw);
GamepadState MapXInput(const RawXInputPad& raw);

// Rumble percent (0..100) scaled by strength (0..100) -> 0..1 / 0..65535.
float RumbleLevel(uint8_t percent, uint8_t strength);
uint16_t RumbleToMotor(uint8_t percent, uint8_t strength);

} // namespace fluvi::input
