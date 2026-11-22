// SPDX-License-Identifier: GPL-3.0-only
// Fluvi app: the controller as a keyboard for the UI (pure logic, no Win32). D-pad and left stick -> arrow keys,
// A -> Enter, B -> Esc, View + Menu together -> overlay toggle. Arrows repeat while held (delay, then interval);
// Enter and Esc do not. Feed it the pad state every frame with the frame's time.
//
// While streaming the pad belongs to the console: with navEnabled false it emits the key-ups of anything held and
// nothing else. Buttons already held when navigation turns (back) on are ignored until released, so the A that
// picked a console does not also press Enter on the next screen.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "fluvi/common/Gamepad.h"

namespace fluvi::app {

struct PadNavConfig {
    double repeatDelayMs = 400;     // first repeat after the press
    double repeatIntervalMs = 90;   // then every
    int stickOn = 16000;            // |axis| to count as pressed (of 32767)
    int stickOff = 11000;           // and to count as released (hysteresis)
};

struct NavKey {
    uint32_t vk = 0;       // VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_ESCAPE
    bool down = false;     // false = key up
    bool repeat = false;   // a down that is an auto-repeat
    bool operator==(const NavKey&) const = default;
};

struct PadNavResult {
    std::vector<NavKey> keys;
    bool toggleOverlay = false;   // View + Menu just became held (reported even when navEnabled is false)
};

class PadNav {
public:
    explicit PadNav(PadNavConfig config = {}) : config_(config) {}
    PadNavResult Update(const GamepadState& pad, double nowMs, bool navEnabled);

private:
    enum Slot { Up, Down, Left, Right, Accept, Back, SlotCount };
    struct KeyState {
        bool held = false;      // key currently down toward the UI
        bool blocked = false;   // physically held while navigation was off: wait for release
        bool raw = false;       // the physical input, last frame (stick hysteresis)
        double nextRepeatMs = 0;
    };
    std::array<KeyState, SlotCount> keys_{};
    bool chord_ = false;
    PadNavConfig config_;
};

} // namespace fluvi::app
