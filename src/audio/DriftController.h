// SPDX-License-Identifier: GPL-3.0-only
// Drift correction: keeps the buffer fill around a target by nudging the resampling ratio (PI controller on a
// low-passed fill measurement). Pure class. A positive ppm means "consume input faster" (fill is too high).
#pragma once
#include <cstdint>

namespace fluvi::audio {

struct DriftConfig {
    double targetMs = 30;
    double maxPpm = 5000;          // +-0.5%: inaudible pitch change
    double lpfTauSec = 1.0;
    double kpPerMs = 150;          // ppm per ms of error
    double kiPerMsSec = 8;         // ppm per (ms * s) of accumulated error (loop: wn ~ 0.09 rad/s, zeta ~ 0.8)
    double maxArrivalAgeMs = 40;   // cap of the arrival-age correction (a stalled stream must not look like a full buffer)
    double maxSlewPpmPerSec = 1000;
};

class DriftController {
public:
    explicit DriftController(const DriftConfig& cfg = {}) : cfg_(cfg) {}
    void SetTarget(double targetMs) { cfg_.targetMs = targetMs; }
    void Reset();
    // fillMs: buffered audio now; dtSec: time since the last call; msSinceArrival: time since the last packet
    // arrived; packetMs: its duration. The fill is a sawtooth (+one packet at every arrival, draining in
    // between), so sampling it on a grid that is not locked to the arrivals aliases into a slow ripple.
    // fill + age is smooth (the jump at an arrival, +packet, is cancelled by the age resetting, -interval) and
    // sits half a packet above the sawtooth mean, so the controller uses fill + age - packetMs / 2, which has the
    // same meaning as the plain mean fill. Returns the correction in ppm.
    double Update(double fillMs, double dtSec, double msSinceArrival = 0.0, double packetMs = 0.0);
    double ppm() const { return ppm_; }
    double filteredFillMs() const { return fill_; }

private:
    DriftConfig cfg_;
    bool primed_ = false;
    double fill_ = 0, integral_ = 0, ppm_ = 0;
};

} // namespace fluvi::audio
