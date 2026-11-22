// SPDX-License-Identifier: GPL-3.0-only
// Our presentation clock. Source interval: mean of the inter-arrival deltas that look like one (or a
// whole number of) source frames. Deltas telescope, so the mean over a long window is as precise as
// (last - first) / n and tells 59.94 from 60 within seconds, while a delayed frame (late + early pair)
// falls outside the gate and is ignored.
#include "fluvi/gpu/PresentClock.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>

#include "fluvi/core/Time.h"

namespace fluvi::gpu {
namespace {

constexpr size_t kWindow = 512;        // inlier deltas kept (~8.5 s at 60 fps)
constexpr size_t kReseedWindow = 32;   // recent raw deltas used to re-seed a wrong estimate
constexpr double kGate = 0.25;         // |delta / estimate - 1| accepted for one frame
constexpr double kGateDrop = 0.08;     // tighter for k > 1 (a late frame must not pass as a drop)
constexpr double kPullMs = 0.2;        // latency given back per frame when we wait for the grid (VRR)

class PresentClockImpl final : public PresentClock {
public:
    PresentClockImpl() { Configure({}); }

    void Configure(const PresentClockConfig& c) override {
        const bool sourceChanged = c.sourceHz != config_.sourceHz || !estimate_;
        config_ = c;
        freq_ = double(time::QpcFrequency());
        refresh_ = c.refreshHz > 1 ? freq_ / c.refreshHz : 0;
        if (sourceChanged) {
            estimate_ = freq_ / std::max(1.0, c.sourceHz);
            inliers_.clear();
            inlierSum_ = 0;
            recent_.clear();
        }
        ResetPhase();
    }

    PresentPlan Plan(int64_t ready) override {
        const bool gap = lastReady_ && double(ready - lastReady_) > 4 * estimate_;
        if (lastReady_ && !gap) Track(double(ready - lastReady_));
        lastReady_ = ready;
        if (gap) ResetPhase();

        PresentPlan plan;
        if (config_.mode == StreamMode::Play) return plan;   // present now; we only measure

        const bool snap = !config_.vrr && refresh_ > 0;
        const double half = estimate_ * 0.5;
        if (!anchor_) anchor_ = ready;
        if (!lastReal_) {   // first frame after a (re)start: nothing to interpolate from
            plan.realAtQpc = snap ? Snap(double(ready)) : ready;
            lastReal_ = plannedReal_ = plan.realAtQpc;
            return plan;
        }

        const double ideal = double(lastReal_) + half;
        double gen;
        if (double(ready) >= ideal) {
            gen = double(ready);                                   // late: re-phase on this arrival
            slackFrames_ = 0;
        } else if (snap) {
            gen = ideal;
            // A whole refresh of slack for a while: give it back in one step.
            slackFrames_ = ideal - double(ready) >= refresh_ ? slackFrames_ + 1 : 0;
            if (slackFrames_ >= 60) { gen -= refresh_; slackFrames_ = 0; }
        } else {
            gen = std::max(double(ready), ideal - time::MsToQpc(kPullMs));
        }
        double real;
        if (snap) {
            gen = double(Snap(gen));
            real = gen + std::max(1.0, std::round(half / refresh_)) * refresh_;
        } else {
            real = gen + half;
        }
        plan.showGenerated = true;
        plan.generatedAtQpc = int64_t(std::llround(gen));
        plan.realAtQpc = int64_t(std::llround(real));
        lastReal_ = plannedReal_ = plan.realAtQpc;
        return plan;
    }

    void OnPresented(int64_t presentQpc, bool generated) override {
        if (generated || !plannedReal_) return;
        // Small overshoot (timer, spin) keeps the grid; a real miss moves the phase.
        const double tolerance = (!config_.vrr && refresh_ > 0) ? refresh_ * 0.5 : double(time::MsToQpc(1.0));
        if (double(presentQpc - plannedReal_) > tolerance) lastReal_ = presentQpc;
    }

    double EstimatedSourceIntervalMs() const override { return estimate_ * 1000.0 / freq_; }

private:
    void ResetPhase() {
        lastReal_ = plannedReal_ = 0;
        anchor_ = 0;
        slackFrames_ = 0;
    }

    int64_t Snap(double t) const {
        const double n = std::ceil((t - double(anchor_)) / refresh_ - kGate);
        return anchor_ + int64_t(std::llround(n * refresh_));
    }

    void Track(double delta) {
        recent_.push_back(delta);
        if (recent_.size() > kReseedWindow) recent_.pop_front();
        const double ratio = delta / estimate_;
        const double k = std::round(ratio);
        bool inlier = false;
        if (k >= 1 && k <= 3 && std::abs(ratio / k - 1) <= (k == 1 ? kGate : kGateDrop)) {
            Add(delta / k);
            inlier = k == 1;
        }
        recentSingle_.push_back(inlier);
        if (recentSingle_.size() > kReseedWindow) recentSingle_.pop_front();
        // The configured rate was wrong (most deltas are not one estimate): re-seed from the median.
        if (recent_.size() == kReseedWindow) {
            const auto singles = std::count(recentSingle_.begin(), recentSingle_.end(), true);
            if (size_t(singles) * 4 < kReseedWindow) {
                std::vector<double> v(recent_.begin(), recent_.end());
                std::nth_element(v.begin(), v.begin() + ptrdiff_t(v.size() / 2), v.end());
                estimate_ = v[v.size() / 2];
                inliers_.clear();
                inlierSum_ = 0;
                recent_.clear();
                recentSingle_.clear();
            }
        }
    }

    void Add(double interval) {
        inliers_.push_back(interval);
        inlierSum_ += interval;
        if (inliers_.size() > kWindow) {
            inlierSum_ -= inliers_.front();
            inliers_.pop_front();
        }
        if (inliers_.size() >= 8) estimate_ = inlierSum_ / double(inliers_.size());
    }

    PresentClockConfig config_{};
    double freq_ = 1, refresh_ = 0;
    double estimate_ = 0;   // source interval, QPC ticks
    std::deque<double> inliers_, recent_;
    std::deque<bool> recentSingle_;
    double inlierSum_ = 0;
    int64_t lastReady_ = 0, lastReal_ = 0, plannedReal_ = 0, anchor_ = 0;
    int slackFrames_ = 0;
};

struct Timer {
    HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    ~Timer() { if (h) CloseHandle(h); }
};

} // namespace

std::unique_ptr<PresentClock> PresentClock::Create() { return std::make_unique<PresentClockImpl>(); }

void PresentClock::WaitUntil(int64_t qpc) {
    constexpr double kSpinMs = 0.3;
    thread_local Timer timer;
    const double remaining = time::QpcToMs(qpc - time::NowQpc());
    if (remaining <= 0) return;
    if (remaining > kSpinMs && timer.h) {
        LARGE_INTEGER due{};
        due.QuadPart = -LONGLONG((remaining - kSpinMs) * 10000.0);   // relative, 100 ns units
        if (SetWaitableTimerEx(timer.h, &due, 0, nullptr, nullptr, nullptr, 0))
            WaitForSingleObject(timer.h, INFINITE);
    }
    while (time::NowQpc() < qpc) YieldProcessor();
}

} // namespace fluvi::gpu
