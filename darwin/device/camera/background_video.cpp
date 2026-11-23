// SPDX-License-Identifier: GPL-2.0-or-later
#include "background_video.hpp"

#include <utility>

namespace uml::camera {

BackgroundVideo::Outcome BackgroundVideo::end(Clock::time_point deadline)
{
    std::unique_lock serial(callMutex_, std::defer_lock);
    if (!serial.try_lock_until(deadline)) return {0, false, false};
    std::unique_lock lock(mutex_);
    if (!attached_) return {};
    const auto request = ++asked_;
    const auto abandonedAtStart = abandoned_;
    requested_.store(true, std::memory_order_release);
    wake_();
    const auto cut = [&] { return abandoned_ != abandonedAtStart; };
    changed_.wait_until(lock, deadline, [&] { return answered_ >= request || !attached_ || cut(); });
    if (!attached_) return {};
    if (cut()) return {0, false, answered_ >= request, true};
    if (answered_ < request) return {0, false, false};
    const auto streams = streams_;
    changed_.wait_until(lock, deadline, [&] { return remaining_ == 0 || !attached_ || cut(); });
    if (cut() && remaining_ != 0 && attached_) return {streams, false, true, true};
    return {streams, remaining_ == 0 || !attached_, true};
}

void BackgroundVideo::abandon()
{
    {
        std::lock_guard lock(mutex_);
        ++abandoned_;
    }
    changed_.notify_all();
}

void BackgroundVideo::attach(std::function<void()> wake)
{
    std::lock_guard lock(mutex_);
    wake_ = std::move(wake);
    attached_ = true;
    requested_.store(false, std::memory_order_relaxed);
    remaining_ = 0;
}

void BackgroundVideo::detach() noexcept
{
    {
        std::lock_guard lock(mutex_);
        attached_ = false;
        wake_ = {};
        requested_.store(false, std::memory_order_relaxed);
        remaining_ = 0;
    }
    changed_.notify_all();
}

bool BackgroundVideo::take(std::uint64_t& request)
{
    if (!requested_.load(std::memory_order_acquire)) return false;
    std::lock_guard lock(mutex_);
    if (!requested_.load(std::memory_order_relaxed)) return false;
    requested_.store(false, std::memory_order_relaxed);
    request = asked_;
    return true;
}

void BackgroundVideo::ended(std::uint64_t request, std::uint32_t streams)
{
    {
        std::lock_guard lock(mutex_);
        answered_ = request;
        streams_ = streams;
        remaining_ = streams;
    }
    changed_.notify_all();
}

void BackgroundVideo::remaining(std::uint32_t streams)
{
    {
        std::lock_guard lock(mutex_);
        remaining_ = streams;
    }
    changed_.notify_all();
}

} // namespace uml::camera
