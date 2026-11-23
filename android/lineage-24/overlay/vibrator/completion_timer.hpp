// SPDX-License-Identifier: Apache-2.0
//
// The completion callbacks of the vibrator HAL. A callback is armed for the
// moment a vibration ends and runs on the timer's own thread, so a call into
// the framework never runs inside a binder transaction of the HAL and never
// under one of its locks. A callback runs once: either at its deadline or, when
// the vibration is cancelled or replaced, as soon as the thread can.
#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace andarium::haptics {
class CompletionTimer {
public:
    using Clock = std::chrono::steady_clock;
    using Callback = std::function<void()>;

    CompletionTimer() : thread_([this] { run(); }) {}

    ~CompletionTimer() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        changed_.notify_all();
        thread_.join();
    }

    CompletionTimer(const CompletionTimer&) = delete;
    CompletionTimer& operator=(const CompletionTimer&) = delete;

    // Arms `callback` for `deadline`. A callback armed earlier and not yet run
    // is due at once: the HAL arms one vibration at a time, and a new one
    // replaces the one before it.
    void schedule(Callback callback, Clock::time_point deadline) {
        {
            std::lock_guard lock(mutex_);
            if (scheduled_) due_.push_back(std::move(scheduled_));
            scheduled_ = std::move(callback);
            deadline_ = deadline;
        }
        changed_.notify_all();
    }

    // Makes the callback that is armed due at once. Nothing happens when none is.
    void complete_scheduled() {
        {
            std::lock_guard lock(mutex_);
            if (!scheduled_) return;
            due_.push_back(std::move(scheduled_));
            scheduled_ = nullptr;
        }
        changed_.notify_all();
    }

private:
    void run() {
        std::unique_lock lock(mutex_);
        for (;;) {
            if (!due_.empty()) {
                auto callback = std::move(due_.front());
                due_.pop_front();
                lock.unlock();
                callback();
                lock.lock();
                continue;
            }
            if (stop_) return;
            if (!scheduled_) {
                changed_.wait(lock);
            } else if (Clock::now() >= deadline_) {
                due_.push_back(std::move(scheduled_));
                scheduled_ = nullptr;
            } else {
                changed_.wait_until(lock, deadline_);
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    Callback scheduled_;
    Clock::time_point deadline_;
    std::deque<Callback> due_;
    bool stop_ = false;
    // Last, so the thread is joined before anything it reads is destroyed.
    std::thread thread_;
};
}  // namespace andarium::haptics
