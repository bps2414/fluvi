// SPDX-License-Identifier: GPL-3.0-only
// Fluvi app: small helpers to keep every network call off the UI/render thread.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace fluvi::app {

// Cancellable sleep shared between the thread that waits and the one that cancels.
class CancelToken {
public:
    void Cancel() {
        { std::lock_guard lock(mutex_); cancelled_ = true; }
        cv_.notify_all();
    }
    bool Cancelled() const { std::lock_guard lock(mutex_); return cancelled_; }
    // Sleeps up to `d`; returns false as soon as the token is cancelled (true = the time passed).
    bool Sleep(std::chrono::milliseconds d) {
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, d, [this] { return cancelled_; });
        return !cancelled_;
    }
private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool cancelled_ = false;
};
using CancelTokenPtr = std::shared_ptr<CancelToken>;

// Runs jobs on their own threads (a sign-in poll loop must not queue behind a console refresh) and joins them all
// on destruction. Finished threads are reaped on the next Run().
class TaskSet {
public:
    ~TaskSet() { JoinAll(); }
    void Run(std::function<void()> job) {
        std::lock_guard lock(mutex_);
        for (auto it = tasks_.begin(); it != tasks_.end();) {
            if ((*it)->done.load()) {
                (*it)->thread.join();
                it = tasks_.erase(it);
            } else {
                ++it;
            }
        }
        auto task = std::make_unique<Task>();
        Task* raw = task.get();
        raw->thread = std::thread([raw, job = std::move(job)] {
            job();
            raw->done = true;
        });
        tasks_.push_back(std::move(task));
    }
    void JoinAll() {
        std::vector<std::unique_ptr<Task>> tasks;
        { std::lock_guard lock(mutex_); tasks.swap(tasks_); }
        for (auto& t : tasks) if (t->thread.joinable()) t->thread.join();
    }
private:
    struct Task {
        std::thread thread;
        std::atomic<bool> done{false};
    };
    std::mutex mutex_;
    std::vector<std::unique_ptr<Task>> tasks_;
};

} // namespace fluvi::app
