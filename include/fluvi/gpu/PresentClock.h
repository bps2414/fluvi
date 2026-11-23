// SPDX-License-Identifier: GPL-3.0-only
// Fluvi gpu-core: our own presentation clock (what makes Veyra smooth on AMD: FSR FG in an independent
// context, the app shows the real and the generated frame evenly spaced).
//
// Play mode: every frame goes out as soon as it is ready (VRR/tearing); the clock only measures.
// Smooth mode: for each real frame N (ready at t), the generated frame between N-1 and N is shown first
// and N half a source interval later. The source interval is tracked from real arrivals (robust to
// jitter and to 59.94 vs 60), so the spacing stays even. Without VRR, times snap to refresh boundaries.
#pragma once
#include <cstdint>
#include <memory>
#include "fluvi/common/Settings.h"

namespace fluvi::gpu {

struct PresentClockConfig {
    StreamMode mode = StreamMode::Play;
    double refreshHz = 60;        // the monitor's real refresh
    bool vrr = false;             // VRR active: any interval inside the range is shown as-is
    double sourceHz = 60;         // expected unique-frame rate (30 for a 30 fps game in Smooth)
};

struct PresentPlan {
    bool showGenerated = false;
    int64_t generatedAtQpc = 0;   // valid if showGenerated
    int64_t realAtQpc = 0;        // 0 = now
};

class PresentClock {
public:
    static std::unique_ptr<PresentClock> Create();
    virtual ~PresentClock() = default;

    virtual void Configure(const PresentClockConfig& config) = 0;
    // A new real frame is ready to present at readyQpc (its GPU work submitted).
    virtual PresentPlan Plan(int64_t readyQpc) = 0;
    // Feed back what happened, to adapt the source interval and the phase.
    virtual void OnPresented(int64_t presentQpc, bool generated) = 0;
    virtual double EstimatedSourceIntervalMs() const = 0;

    // Precise wait (high-resolution waitable timer + short spin). Returns immediately if already past.
    static void WaitUntil(int64_t qpc);
};

} // namespace fluvi::gpu
