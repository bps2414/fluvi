// SPDX-License-Identifier: GPL-3.0-only
// Fluvi app: where the window opens. Pure geometry (no Win32 calls), so it is tested without a window.
#pragma once
#include <windows.h>

#include "fluvi/common/Settings.h"

namespace fluvi::app {

// Window sizes are in dp (96 dpi = 1 dp per pixel), multiplied by the DPI scale of the monitor the window is on.
inline constexpr int kMinWidthDp = 1120, kMinHeightDp = 630;
inline constexpr int kDefaultWidthDp = 1600, kDefaultHeightDp = 900;

struct MonitorArea {
    RECT work{};              // work area of the monitor, desktop pixels
    unsigned dpi = 96;        // effective DPI of that monitor
};

struct StartRect {
    int x = 0, y = 0, width = 0, height = 0;   // desktop pixels (the client area is the whole window)
    bool maximized = false;
};

// Minimum window size in pixels at `dpi`.
SIZE MinWindowSize(unsigned dpi);
// The saved placement clamped to the monitor's work area and to the minimum size (maximized flag kept), or, without a
// saved placement, the default size limited to the work area and centered.
StartRect ComputeStartRect(const WindowSettings& saved, const MonitorArea& monitor);

} // namespace fluvi::app
