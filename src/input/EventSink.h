// SPDX-License-Identifier: GPL-3.0-only
// Shared by both backends: change filter (exact equality), per-pad current state, rate/latency statistics.
// The backend serialises its own calls to Submit(); the sink only guards its state against Stats()/Current().
#pragma once
#include <array>
#include <mutex>
#include "RateMeter.h"
#include "fluvi/core/Time.h"
#include "fluvi/input/GamepadReader.h"

namespace fluvi::input {

class EventSink {
public:
    static constexpr uint8_t kMaxPads = 8;

    void Reset(GamepadReader::OnChange cb) {
        std::lock_guard lock(mutex_);
        cb_ = std::move(cb);
        current_.fill({});
        reads_ = {};
        events_ = {};
        latCur_ = latPrev_ = 0;
        latWindowMs_ = -1;
        connected_ = 0;
    }

    void NoteRead(uint32_t count = 1) {
        const double now = NowMs();
        std::lock_guard lock(mutex_);
        reads_.Add(now, count);
    }

    // Delivers the state if it differs from the last delivered one (or `force`). readQpc is when the report was
    // read; the delay to now is tracked as read-to-callback latency.
    bool Submit(uint8_t index, const GamepadState& s, int64_t readQpc, bool force = false) {
        if (index >= kMaxPads) return false;
        GamepadReader::OnChange cb;
        const int64_t nowQpc = fluvi::time::NowQpc();
        {
            std::lock_guard lock(mutex_);
            if (!force && s == current_[index]) return false;
            current_[index] = s;
            const double now = fluvi::time::QpcToMs(nowQpc);
            events_.Add(now);
            const double lat = std::max(0.0, fluvi::time::QpcToMs(nowQpc - readQpc));
            if (latWindowMs_ < 0 || now - latWindowMs_ > 2500) { latPrev_ = latCur_; latCur_ = 0; latWindowMs_ = now; }
            latCur_ = std::max(latCur_, lat);
            cb = cb_;
        }
        if (cb) {
            GamepadEvent e;
            e.state = s;
            e.qpc = readQpc;
            e.index = index;
            cb(e);
        }
        return true;
    }

    void SetConnectedCount(int n) { std::lock_guard lock(mutex_); connected_ = n; }
    GamepadState Current(uint8_t index) const {
        std::lock_guard lock(mutex_);
        return index < kMaxPads ? current_[index] : GamepadState{};
    }
    void Fill(ReaderStats& st) {
        const double now = NowMs();
        std::lock_guard lock(mutex_);
        st.connected = connected_ > 0;
        st.readsPerSecond = reads_.PerSecond(now);
        st.eventsPerSecond = events_.PerSecond(now);
        st.maxReadToCallbackMs = std::max(latCur_, latPrev_);
    }

private:
    static double NowMs() { return fluvi::time::QpcToMs(fluvi::time::NowQpc()); }
    mutable std::mutex mutex_;
    GamepadReader::OnChange cb_;
    std::array<GamepadState, kMaxPads> current_{};
    RateMeter reads_, events_;
    double latCur_ = 0, latPrev_ = 0, latWindowMs_ = -1;
    int connected_ = 0;
};

} // namespace fluvi::input
