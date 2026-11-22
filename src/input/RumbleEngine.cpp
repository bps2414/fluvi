// SPDX-License-Identifier: GPL-3.0-only
#include "RumbleEngine.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>
#include "GamepadMapping.h"
#include "fluvi/core/Time.h"

namespace fluvi::input {

void RumbleSchedule::Start(const Rumble& r, uint8_t strength, double nowMs) {
    on_.lowFrequency = RumbleLevel(r.leftMotor, strength);
    on_.highFrequency = RumbleLevel(r.rightMotor, strength);
    on_.leftTrigger = RumbleLevel(r.leftTrigger, strength);
    on_.rightTrigger = RumbleLevel(r.rightTrigger, strength);
    startMs_ = nowMs;
    durationMs_ = r.durationMs ? double(r.durationMs) : kDefaultDurationMs;
    gapMs_ = double(r.delayMs);
    pulses_ = 1u + r.repeat;
    active_ = on_.Any();
}

MotorLevels RumbleSchedule::Poll(double nowMs, double& nextChangeMs) const {
    nextChangeMs = -1;
    if (!active_) return {};
    const double e = std::max(0.0, nowMs - startMs_);
    const double cycle = durationMs_ + gapMs_;
    const double total = double(pulses_ - 1) * cycle + durationMs_;
    if (e >= total) return {};
    const uint32_t idx = pulses_ > 1 && cycle > 0 ? std::min<uint32_t>(pulses_ - 1, uint32_t(e / cycle)) : 0;
    const double within = e - double(idx) * cycle;
    if (within < durationMs_) {
        nextChangeMs = startMs_ + double(idx) * cycle + durationMs_;
        return on_;
    }
    nextChangeMs = startMs_ + double(idx + 1) * cycle;
    return {};
}

struct RumbleEngine::Impl {
    Apply apply;
    std::mutex mutex;
    std::array<RumbleSchedule, kMaxPads> schedules;
    std::array<MotorLevels, kMaxPads> applied{};
    HANDLE wake = nullptr, timer = nullptr;
    bool quit = false;
    std::thread thread;

    static double NowMs() { return fluvi::time::QpcToMs(fluvi::time::NowQpc()); }

    void Run() {
        for (;;) {
            struct Change { uint8_t index; MotorLevels levels; };
            Change changes[kMaxPads];
            int n = 0;
            double nextMs = -1;
            {
                std::lock_guard lock(mutex);
                if (quit) return;
                const double now = NowMs();
                for (uint8_t i = 0; i < kMaxPads; ++i) {
                    double next = -1;
                    const MotorLevels lv = schedules[i].Poll(now, next);
                    if (!(lv == applied[i])) { applied[i] = lv; changes[n++] = {i, lv}; }
                    if (next >= 0 && (nextMs < 0 || next < nextMs)) nextMs = next;
                }
                if (nextMs >= 0) nextMs -= now;       // relative
            }
            for (int i = 0; i < n; ++i) apply(changes[i].index, changes[i].levels);

            HANDLE waits[2] = {wake, timer};
            DWORD count = 1;
            if (nextMs >= 0 && timer) {
                LARGE_INTEGER due;
                due.QuadPart = -static_cast<LONGLONG>(std::max(0.0, nextMs) * 10000.0);   // 100 ns, relative
                if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) count = 2;
            }
            WaitForMultipleObjects(count, waits, FALSE, count == 2 ? INFINITE : (nextMs >= 0 ? 1 : INFINITE));
        }
    }
};

RumbleEngine::RumbleEngine(Apply apply) : impl_(std::make_unique<Impl>()) {
    impl_->apply = std::move(apply);
    impl_->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    impl_->timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!impl_->timer) impl_->timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    impl_->thread = std::thread([p = impl_.get()] { p->Run(); });
}

RumbleEngine::~RumbleEngine() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->quit = true;
        for (auto& s : impl_->schedules) s.Stop();
    }
    SetEvent(impl_->wake);
    if (impl_->thread.joinable()) impl_->thread.join();
    // Leave every motor off.
    for (uint8_t i = 0; i < kMaxPads; ++i)
        if (impl_->applied[i].Any()) impl_->apply(i, {});
    if (impl_->timer) CloseHandle(impl_->timer);
    CloseHandle(impl_->wake);
}

void RumbleEngine::Set(uint8_t index, const Rumble& rumble, uint8_t strength) {
    if (index >= kMaxPads) return;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->schedules[index].Start(rumble, strength, Impl::NowMs());
    }
    SetEvent(impl_->wake);
}

void RumbleEngine::Stop(uint8_t index) {
    if (index >= kMaxPads) return;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->schedules[index].Stop();
    }
    SetEvent(impl_->wake);
}

void RumbleEngine::StopAll() {
    {
        std::lock_guard lock(impl_->mutex);
        for (auto& s : impl_->schedules) s.Stop();
    }
    SetEvent(impl_->wake);
}

} // namespace fluvi::input
