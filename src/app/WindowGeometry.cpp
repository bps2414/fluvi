// SPDX-License-Identifier: GPL-3.0-only
#include "WindowGeometry.h"

#include <algorithm>
#include <cmath>

namespace fluvi::app {

namespace {
int Scaled(int dp, unsigned dpi) { return static_cast<int>(std::lround(dp * (dpi / 96.0))); }
}

SIZE MinWindowSize(unsigned dpi) { return {Scaled(kMinWidthDp, dpi), Scaled(kMinHeightDp, dpi)}; }

StartRect ComputeStartRect(const WindowSettings& saved, const MonitorArea& monitor) {
    const int workW = std::max<int>(1, monitor.work.right - monitor.work.left);
    const int workH = std::max<int>(1, monitor.work.bottom - monitor.work.top);
    StartRect r;
    if (saved.hasPlacement) {
        const SIZE minSize = MinWindowSize(monitor.dpi);
        // The minimum wins over a work area that is smaller than it; a work area wins over a saved size that is larger.
        r.width = std::max<int>(minSize.cx, std::min<int>(static_cast<int>(saved.width), workW));
        r.height = std::max<int>(minSize.cy, std::min<int>(static_cast<int>(saved.height), workH));
        r.x = std::clamp(saved.x, static_cast<int>(monitor.work.left), std::max<int>(monitor.work.left, monitor.work.right - r.width));
        r.y = std::clamp(saved.y, static_cast<int>(monitor.work.top), std::max<int>(monitor.work.top, monitor.work.bottom - r.height));
        r.maximized = saved.maximized;
        return r;
    }
    r.width = std::min(Scaled(kDefaultWidthDp, monitor.dpi), workW);
    r.height = std::min(Scaled(kDefaultHeightDp, monitor.dpi), workH);
    r.x = monitor.work.left + (workW - r.width) / 2;
    r.y = monitor.work.top + (workH - r.height) / 2;
    return r;
}

} // namespace fluvi::app
