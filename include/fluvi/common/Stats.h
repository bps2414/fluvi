// SPDX-License-Identifier: GPL-3.0-only
// Fluvi: what the in-game overlay shows. Filled by the integration (wave 2/3), drawn by the UI.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include "fluvi/common/Settings.h"

namespace fluvi {

enum class PresentMode : uint8_t { Unknown, Composed, IndependentFlip, HardwareOverlay };

struct OverlayStats {
    StreamMode mode = StreamMode::Play;
    uint32_t streamWidth = 0, streamHeight = 0;     // what the console sends
    uint32_t outputWidth = 0, outputHeight = 0;     // what reaches the screen
    double bitrateMbps = 0;
    double gameFps = 0;        // unique frames from the game (repeats removed)
    double streamFps = 0;      // frames decoded
    double generatedFps = 0;   // FG frames shown
    double presentedFps = 0;   // everything shown
    double latencyMs = 0;      // client side: packet arrival -> present
    double rttMs = 0;
    double decodeMs = 0, gpuMs = 0;
    uint64_t packetsLost = 0, framesDropped = 0;
    PresentMode presentMode = PresentMode::Unknown;
    bool tearing = false;
    std::optional<bool> vrr;   // nullopt = can't tell
    double refreshHz = 0;
    std::string warning;       // a degraded path is never silent (e.g. "FSR recusado, usando EASU")
};

} // namespace fluvi
