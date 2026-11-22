// SPDX-License-Identifier: GPL-3.0-only
// Rumble timeline: the console sends (levels, duration, delay, repeat); something has to turn the motors off again.
// RumbleSchedule is the pure timeline, RumbleEngine runs it on a small high-resolution-timer thread.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include "fluvi/common/Gamepad.h"

namespace fluvi::input {

struct MotorLevels {
    float lowFrequency = 0, highFrequency = 0, leftTrigger = 0, rightTrigger = 0;   // 0..1
    bool Any() const { return lowFrequency > 0 || highFrequency > 0 || leftTrigger > 0 || rightTrigger > 0; }
    bool operator==(const MotorLevels&) const = default;
};

// Pulse pattern (GIP-style): on for durationMs, off for delayMs, 1 + repeat pulses in total, first pulse
// starts immediately. durationMs == 0 means "unspecified" and falls back to kDefaultDurationMs (Veyra does the
// same), so a lost "stop" can never leave a motor running.
class RumbleSchedule {
public:
    static constexpr double kDefaultDurationMs = 1000.0;

    void Start(const Rumble& rumble, uint8_t strength, double nowMs);
    void Stop() { active_ = false; }
    // Levels at nowMs. nextChangeMs is the time of the next on/off transition, or < 0 when nothing more changes.
    MotorLevels Poll(double nowMs, double& nextChangeMs) const;

private:
    bool active_ = false;
    MotorLevels on_;
    double startMs_ = 0, durationMs_ = 0, gapMs_ = 0;
    uint32_t pulses_ = 1;
};

class RumbleEngine {
public:
    // Called on the engine thread when the levels of a pad actually change.
    using Apply = std::function<void(uint8_t index, const MotorLevels&)>;
    static constexpr uint8_t kMaxPads = 8;

    explicit RumbleEngine(Apply apply);
    ~RumbleEngine();
    RumbleEngine(const RumbleEngine&) = delete;
    RumbleEngine& operator=(const RumbleEngine&) = delete;

    void Set(uint8_t index, const Rumble& rumble, uint8_t strength);
    void Stop(uint8_t index);
    void StopAll();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fluvi::input
