// SPDX-License-Identifier: GPL-3.0-only
// Drift correction: keeps the buffer fill around a target by nudging the resampling ratio (PI controller on a
// low-passed fill measurement). Pure class. A positive ppm means "consume input faster" (fill is too high).
#pragma once
#include <cstdint>

namespace fluvi::audio {

struct DriftConfig {
    double targetMs = 30;
    double maxPpm = 5000;          // +-0.5%: inaudible pitch change
    double lpfTauSec = 0.5;        // smoothing of the fill measurement (packets arrive in 20 ms steps)
    double kpPerMs = 300;          // ppm per ms of error
    double kiPerMsSec = 30;        // ppm per (ms * s) of accumulated error
    double maxSlewPpmPerSec = 1000;
};

class DriftController {
public:
    explicit DriftController(const DriftConfig& cfg = {}) : cfg_(cfg) {}
    void SetTarget(double targetMs) { cfg_.targetMs = targetMs; }
    void Reset();
    // fillMs: buffered audio now; dtSec: time since the last call. Returns the correction in ppm.
    double Update(double fillMs, double dtSec);
    double ppm() const { return ppm_; }
    double filteredFillMs() const { return fill_; }

private:
    DriftConfig cfg_;
    bool primed_ = false;
    double fill_ = 0, integral_ = 0, ppm_ = 0;
};

} // namespace fluvi::audio
