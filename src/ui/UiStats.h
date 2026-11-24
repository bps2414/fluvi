// SPDX-License-Identifier: GPL-3.0-only
// Fluvi ui: stream health for the overlay and the corner HUD (pure logic, no RmlUi): the latency history drawn as
// the graph, the health band ("Ótimo" .. "Ruim") and the session clock text.
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace fluvi::ui {

// Ótimo, Bom, Instável, Ruim (in that order; the RML classes are band-0 .. band-3).
enum class HealthBand : uint8_t { Great, Good, Unsteady, Poor };

// totalMs = client latency + network round trip; jitterMs = spread of totalMs over the window;
// newLosses = packets lost inside the window.
HealthBand ClassifyHealth(double totalMs, double jitterMs, uint64_t newLosses);

// Samples of the last `windowSeconds`, fed from each stats update (a few per second).
class StreamHealth {
public:
    explicit StreamHealth(double windowSeconds = 10.0) : window_(windowSeconds) {}

    // `seconds` is a monotonic clock. A counter that goes backwards (new session) restarts the history.
    void Add(double seconds, double latencyMs, double rttMs, uint64_t packetsLost);
    void Reset() { samples_.clear(); }
    bool Empty() const { return samples_.empty(); }

    // The newest client + network latency (0 before any sample).
    double LastTotalMs() const { return samples_.empty() ? 0.0 : samples_.back().totalMs; }
    // `count` bar heights in percent (0..100), oldest first; empty slots before the first sample are 0, later
    // gaps repeat the previous value. The scale is at least `floorMs` tall so a calm stream draws a calm line.
    std::vector<int> Bars(double nowSeconds, size_t count, double floorMs = 60.0) const;
    double JitterMs() const;
    uint64_t NewLosses() const;
    HealthBand Band() const;

private:
    struct Sample {
        double t;
        double totalMs;
        uint64_t lost;
    };
    double window_;
    std::deque<Sample> samples_;
};

// "0:42", "12:05", "1:02:03".
std::string FormatSessionTime(double seconds);

} // namespace fluvi::ui
