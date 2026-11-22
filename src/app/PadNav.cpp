// SPDX-License-Identifier: GPL-3.0-only
#include "PadNav.h"

#include <cstdlib>
#include <windows.h>

namespace fluvi::app {

namespace {
constexpr uint32_t kVk[] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_ESCAPE};
}

PadNavResult PadNav::Update(const GamepadState& pad, double nowMs, bool navEnabled) {
    PadNavResult out;

    const bool chord = (pad.buttons & (BtnView | BtnMenu)) == (BtnView | BtnMenu);
    out.toggleOverlay = chord && !chord_;
    chord_ = chord;

    // Physical inputs. The stick counts on its dominant axis only (a diagonal gives one arrow), with hysteresis.
    const int x = pad.leftX, y = pad.leftY;   // y: up is positive
    const bool horizontal = std::abs(x) >= std::abs(y);
    const auto stick = [&](Slot slot, bool axisActive, int value, int sign) {
        const int threshold = keys_[slot].raw ? config_.stickOff : config_.stickOn;
        return axisActive && value * sign >= threshold;
    };
    bool raw[SlotCount];
    raw[Up] = (pad.buttons & BtnUp) || stick(Up, !horizontal, y, +1);
    raw[Down] = (pad.buttons & BtnDown) || stick(Down, !horizontal, y, -1);
    raw[Left] = (pad.buttons & BtnLeft) || stick(Left, horizontal, x, -1);
    raw[Right] = (pad.buttons & BtnRight) || stick(Right, horizontal, x, +1);
    raw[Accept] = (pad.buttons & BtnA) != 0;
    raw[Back] = (pad.buttons & BtnB) != 0;
    // Opposite arrows together cancel out.
    if (raw[Up] && raw[Down]) raw[Up] = raw[Down] = false;
    if (raw[Left] && raw[Right]) raw[Left] = raw[Right] = false;

    for (int i = 0; i < SlotCount; ++i) {
        KeyState& k = keys_[i];
        const bool repeats = i <= Right;
        if (!raw[i]) k.blocked = false;
        else if (!navEnabled) k.blocked = true;
        k.raw = raw[i];

        const bool want = raw[i] && navEnabled && !k.blocked;
        if (want && !k.held) {
            k.held = true;
            k.nextRepeatMs = nowMs + config_.repeatDelayMs;
            out.keys.push_back({kVk[i], true, false});
        } else if (want && k.held && repeats && nowMs >= k.nextRepeatMs) {
            k.nextRepeatMs += config_.repeatIntervalMs;
            if (k.nextRepeatMs <= nowMs) k.nextRepeatMs = nowMs + config_.repeatIntervalMs;   // a long frame: one repeat, not a burst
            out.keys.push_back({kVk[i], true, true});
        } else if (!want && k.held) {
            k.held = false;
            out.keys.push_back({kVk[i], false, false});
        }
    }
    return out;
}

} // namespace fluvi::app
