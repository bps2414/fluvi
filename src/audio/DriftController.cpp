// SPDX-License-Identifier: GPL-3.0-only
#include "DriftController.h"
#include <algorithm>
#include <cmath>

namespace fluvi::audio {

void DriftController::Reset() {
    primed_ = false;
    fill_ = integral_ = ppm_ = 0;
}

double DriftController::Update(double fillMs, double dtSec) {
    if (dtSec <= 0) return ppm_;
    if (!primed_) { fill_ = fillMs; primed_ = true; }
    fill_ += (fillMs - fill_) * (1.0 - std::exp(-dtSec / cfg_.lpfTauSec));

    const double err = fill_ - cfg_.targetMs;
    integral_ += err * dtSec;
    // anti-windup: the integral term alone may not exceed the allowed correction
    const double iMax = cfg_.maxPpm / cfg_.kiPerMsSec;
    integral_ = std::clamp(integral_, -iMax, iMax);

    const double want = std::clamp(cfg_.kpPerMs * err + cfg_.kiPerMsSec * integral_, -cfg_.maxPpm, cfg_.maxPpm);
    const double maxStep = cfg_.maxSlewPpmPerSec * dtSec;
    ppm_ += std::clamp(want - ppm_, -maxStep, maxStep);
    return ppm_;
}

} // namespace fluvi::audio
