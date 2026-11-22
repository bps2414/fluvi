// SPDX-License-Identifier: GPL-3.0-only
// Events-per-second over a sliding one-second window (10 buckets of 100 ms). Not thread-safe; the owner locks.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace fluvi::input {

class RateMeter {
public:
    void Add(double nowMs, uint32_t count = 1) {
        Advance(nowMs);
        buckets_[Index(nowMs)] += count;
        if (firstMs_ < 0) firstMs_ = nowMs;
    }
    // Rate over the last second (or over the elapsed time if the meter is younger than that).
    double PerSecond(double nowMs) {
        Advance(nowMs);
        if (firstMs_ < 0) return 0;
        uint64_t sum = 0;
        for (auto b : buckets_) sum += b;
        const double span = std::min(1000.0, std::max(100.0, nowMs - firstMs_));
        return double(sum) * 1000.0 / span;
    }

private:
    static constexpr int kBuckets = 10;
    static constexpr double kBucketMs = 100.0;
    static int64_t Tick(double ms) { return static_cast<int64_t>(ms / kBucketMs); }
    static size_t Index(double ms) { return static_cast<size_t>(Tick(ms) % kBuckets); }
    void Advance(double nowMs) {
        const int64_t tick = Tick(nowMs);
        if (lastTick_ < 0) { lastTick_ = tick; return; }
        const int64_t steps = tick - lastTick_;
        if (steps <= 0) return;
        if (steps >= kBuckets) buckets_.fill(0);
        else for (int64_t i = 1; i <= steps; ++i) buckets_[static_cast<size_t>((lastTick_ + i) % kBuckets)] = 0;
        lastTick_ = tick;
    }
    std::array<uint32_t, kBuckets> buckets_{};
    int64_t lastTick_ = -1;
    double firstMs_ = -1;
};

} // namespace fluvi::input
