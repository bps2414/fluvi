// SPDX-License-Identifier: GPL-3.0-only
// Fluvi: the short queue between threads. Bounded; pushing into a full queue drops the OLDEST item, so a
// slow consumer sees fresh data instead of building latency. Never blocks the producer.
#pragma once
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace fluvi {

template <typename T, size_t Capacity>
class LatestQueue {
    static_assert(Capacity >= 1);
public:
    // Returns true if an older item had to be dropped.
    bool Push(T item) {
        bool dropped = false;
        {
            std::lock_guard lock(mutex_);
            if (count_ == Capacity) {
                head_ = (head_ + 1) % Capacity;
                --count_;
                ++dropped_;
                dropped = true;
            }
            items_[(head_ + count_) % Capacity] = std::move(item);
            ++count_;
        }
        cv_.notify_one();
        return dropped;
    }

    std::optional<T> TryPop() {
        std::lock_guard lock(mutex_);
        return popLocked();
    }

    std::optional<T> WaitPop(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, timeout, [&] { return count_ > 0 || closed_; });
        return popLocked();
    }

    // Wakes every waiter; later WaitPop calls return immediately.
    void Close() {
        { std::lock_guard lock(mutex_); closed_ = true; }
        cv_.notify_all();
    }

    void Clear() { std::lock_guard lock(mutex_); head_ = count_ = 0; }
    size_t Size() const { std::lock_guard lock(mutex_); return count_; }
    uint64_t Dropped() const { std::lock_guard lock(mutex_); return dropped_; }

private:
    std::optional<T> popLocked() {
        if (count_ == 0) return std::nullopt;
        std::optional<T> out(std::move(items_[head_]));
        head_ = (head_ + 1) % Capacity;
        --count_;
        return out;
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::array<T, Capacity> items_{};
    size_t head_ = 0, count_ = 0;
    uint64_t dropped_ = 0;
    bool closed_ = false;
};

} // namespace fluvi
