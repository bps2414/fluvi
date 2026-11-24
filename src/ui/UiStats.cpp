// SPDX-License-Identifier: GPL-3.0-only
// Fluvi ui: latency history, health bands and the session clock for the overlay and the corner HUD.
#include "UiStats.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

namespace fluvi::ui {

HealthBand ClassifyHealth(double totalMs, double jitterMs, uint64_t newLosses) {
    // Thresholds for play feel: under ~35 ms nobody notices; past ~70 ms or with visible spikes it feels loose;
    // past ~120 ms or with steady losses it is hard to play.
    if (totalMs >= 120.0 || newLosses >= 20) return HealthBand::Poor;
    if (totalMs >= 70.0 || jitterMs >= 12.0 || newLosses > 0) return HealthBand::Unsteady;
    if (totalMs >= 35.0) return HealthBand::Good;
    return HealthBand::Great;
}

void StreamHealth::Add(double seconds, double latencyMs, double rttMs, uint64_t packetsLost) {
    if (!samples_.empty() && (packetsLost < samples_.back().lost || seconds < samples_.back().t)) samples_.clear();
    samples_.push_back({seconds, std::max(0.0, latencyMs) + std::max(0.0, rttMs), packetsLost});
    while (!samples_.empty() && samples_.front().t < seconds - window_) samples_.pop_front();
}

std::vector<int> StreamHealth::Bars(double nowSeconds, size_t count, double floorMs) const {
    std::vector<int> bars(count, 0);
    if (samples_.empty() || count == 0) return bars;
    double top = floorMs;
    for (const Sample& s : samples_) top = std::max(top, s.totalMs * 1.15);
    const double start = nowSeconds - window_;
    const double slot = window_ / static_cast<double>(count);
    size_t next = 0;
    double carry = -1.0;
    for (size_t i = 0; i < count; ++i) {
        // The last slot ends at "now" inclusive, so the newest sample is always drawn.
        const double end = i + 1 == count ? std::numeric_limits<double>::infinity() : start + slot * static_cast<double>(i + 1);
        double peak = -1.0;
        while (next < samples_.size() && samples_[next].t < end) peak = std::max(peak, samples_[next++].totalMs);
        if (peak >= 0.0) carry = peak;
        if (carry >= 0.0) bars[i] = std::clamp(static_cast<int>(std::lround(carry / top * 100.0)), 3, 100);
    }
    return bars;
}

double StreamHealth::JitterMs() const {
    if (samples_.size() < 2) return 0.0;
    double mean = 0.0;
    for (const Sample& s : samples_) mean += s.totalMs;
    mean /= static_cast<double>(samples_.size());
    double var = 0.0;
    for (const Sample& s : samples_) var += (s.totalMs - mean) * (s.totalMs - mean);
    return std::sqrt(var / static_cast<double>(samples_.size()));
}

uint64_t StreamHealth::NewLosses() const {
    return samples_.empty() ? 0 : samples_.back().lost - samples_.front().lost;
}

HealthBand StreamHealth::Band() const { return ClassifyHealth(LastTotalMs(), JitterMs(), NewLosses()); }

std::string FormatSessionTime(double seconds) {
    const auto total = static_cast<long long>(std::max(0.0, seconds));
    const long long h = total / 3600, m = (total / 60) % 60, s = total % 60;
    return h > 0 ? std::format("{}:{:02}:{:02}", h, m, s) : std::format("{}:{:02}", m, s);
}

} // namespace fluvi::ui
