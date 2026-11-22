// SPDX-License-Identifier: GPL-3.0-only
// Internal: the monitor a window is on (exact refresh, VRR) and refresh-mode selection.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <windows.h>
#include "fluvi/gpu/SwapChain.h"

namespace fluvi::gpu::detail {

// Monitor of the window: GDI name, size, exact refresh from QueryDisplayConfig, VRR best effort.
DisplayInfo QueryDisplay(HWND hwnd, bool tearingSupported);

// VRR state for a GDI display name (\\.\DISPLAY1). AMD: ADL FreeSync state. Others: nullopt.
std::optional<bool> QueryVrr(const std::wstring& gdiDeviceName);

// --- refresh override mode selection (pure, tested) ---
struct DisplayModeCandidate {
    uint32_t width = 0, height = 0, bitsPerPixel = 0, frequency = 0;   // DEVMODE fields
};

// DEVMODE only carries integer Hz; Windows lists NTSC-style rates one below (59 = 59.94, 119 = 119.88).
double EffectiveHz(uint32_t frequency);

// Index of the mode with the same size (and bpp, if any matches) whose effective Hz is closest to target.
// Ties prefer the higher rate. -1 when no mode has that size.
int SelectRefreshMode(const std::vector<DisplayModeCandidate>& modes, uint32_t width, uint32_t height,
                      uint32_t bitsPerPixel, double targetHz);

} // namespace fluvi::gpu::detail
