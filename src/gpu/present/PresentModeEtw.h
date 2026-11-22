// SPDX-License-Identifier: GPL-3.0-only
// Independent Flip vs Composed, read from ETW (needs admin or "Performance Log Users").
//
// Why ETW: nothing in DXGI/DWM tells a windowed flip-model app whether DWM composed its frame.
// IDXGISwapChainMedia::GetFrameStatisticsMedia().CompositionMode is about the YUV decode swapchain path,
// GetFrameStatistics and DwmGetCompositionTimingInfo report timing, not the path. PresentMon reads it from
// Microsoft-Windows-Win32k: TokenCompositionSurfaceObject (id 201, raised on the presenting process for
// each flip-model present, carries CompositionSurfaceLuid) and TokenStateChanged (id 301, raised by the
// token manager as the present moves through DWM, carries IndependentFlip). Field layout verified against
// this machine's manifest (Windows 11 26200, both events version 1); other versions go through TDH.
// A present counts as Independent Flip if any of its 301 events has IndependentFlip set, else Composed.
#pragma once
#include <cstdint>
#include <memory>
#include "fluvi/common/Stats.h"

namespace fluvi::gpu {

class PresentModeEtw {
public:
    // One real-time session per process, shared. nullptr when ETW is unavailable (not elevated).
    static std::shared_ptr<PresentModeEtw> Acquire();
    virtual ~PresentModeEtw() = default;
    // Majority over this process' presents finalized in the last ~1 s; the last verdict if none since;
    // Unknown before any.
    virtual PresentMode Mode() const = 0;
    virtual uint64_t PresentsSeen() const = 0;
};

} // namespace fluvi::gpu
