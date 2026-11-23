// SPDX-License-Identifier: GPL-2.0-or-later

#include "endpoint.hpp"

#include <array>
#include <cerrno>
#include <stdexcept>

#include <sys/event.h>
#include <sys/time.h>

namespace uml::gnss {
namespace {

constexpr std::uintptr_t reportIdentifier = 1;
constexpr std::uintptr_t timerIdentifier = 2;

} // namespace

Endpoint::Endpoint(std::unique_ptr<FixSource> source)
    : queue_(::kqueue())
    , source_(std::move(source))
{
    if (queue_.get() < 0)
        throw std::runtime_error("creating the Virtio GNSS event queue failed");
    struct kevent report {};
    EV_SET(
        &report, reportIdentifier, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (::kevent(queue_.get(), &report, 1, nullptr, 0, nullptr) != 0)
        throw std::runtime_error("registering the Virtio GNSS event failed");
}

Endpoint::~Endpoint() = default;

void Endpoint::poke() noexcept
{
    struct kevent report {};
    EV_SET(&report, reportIdentifier, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    /* Nothing to do about a failure: the next timer tick reports the fix. */
    (void)::kevent(queue_.get(), &report, 1, nullptr, 0, nullptr);
}

void Endpoint::report(const Fix& fix)
{
    {
        std::lock_guard lock(mutex_);
        if (source_ != nullptr)
            return;
        reported_ = fix;
        ++generation_;
    }
    poke();
}

void Endpoint::clear()
{
    {
        std::lock_guard lock(mutex_);
        if (source_ != nullptr)
            return;
        reported_.reset();
        ++generation_;
    }
    poke();
}

void Endpoint::setPowerObserver(PowerObserver observer)
{
    bool powered = false;
    PowerObserver notify;
    {
        std::lock_guard lock(mutex_);
        observer_ = std::move(observer);
        powered = powered_;
        notify = observer_;
    }
    if (notify && powered)
        notify(true);
}

void Endpoint::setWakeHook(WakeHook hook)
{
    std::lock_guard lock(mutex_);
    wakeHook_ = std::move(hook);
}

bool Endpoint::powered() const
{
    std::lock_guard lock(mutex_);
    return powered_;
}

bool Endpoint::drain()
{
    bool timerFired = false;
    constexpr timespec noWait {};
    std::array<struct kevent, 8> raw {};
    for (;;) {
        const int count = ::kevent(
            queue_.get(),
            nullptr,
            0,
            raw.data(),
            static_cast<int>(raw.size()),
            &noWait);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error("draining Virtio GNSS events failed");
        }
        for (int index = 0; index < count; ++index) {
            const auto& event = raw[static_cast<std::size_t>(index)];
            if ((event.flags & EV_ERROR) != 0)
                continue;
            if (event.filter == EVFILT_TIMER)
                timerFired = true;
        }
        if (count < static_cast<int>(raw.size()))
            return timerFired;
    }
}

void Endpoint::startTimer(std::chrono::milliseconds period)
{
    struct kevent timer {};
    // Adding the same identifier again restarts the period.
    EV_SET(
        &timer,
        timerIdentifier,
        EVFILT_TIMER,
        EV_ADD | EV_ENABLE,
        0,
        static_cast<intptr_t>(period.count()),
        nullptr);
    if (::kevent(queue_.get(), &timer, 1, nullptr, 0, nullptr) != 0)
        throw std::runtime_error("starting the Virtio GNSS timer failed");
}

void Endpoint::stopTimer() noexcept
{
    struct kevent timer {};
    EV_SET(&timer, timerIdentifier, EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
    /* ENOENT: the timer was not running. */
    (void)::kevent(queue_.get(), &timer, 1, nullptr, 0, nullptr);
}

std::uint64_t Endpoint::generation() const
{
    std::lock_guard lock(mutex_);
    return generation_;
}

std::optional<Fix> Endpoint::latest(std::int64_t nowUtcNs)
{
    std::lock_guard lock(mutex_);
    if (source_ != nullptr)
        return source_->latest(nowUtcNs);
    return heldFix(reported_, nowUtcNs);
}

void Endpoint::setPowered(bool powered, std::int64_t nowUtcNs)
{
    PowerObserver notify;
    {
        std::lock_guard lock(mutex_);
        if (powered_ == powered)
            return;
        powered_ = powered;
        if (source_ != nullptr)
            source_->powerChanged(powered, nowUtcNs);
        notify = observer_;
    }
    if (notify)
        notify(powered);
}

void Endpoint::requestWake()
{
    WakeHook hook;
    {
        std::lock_guard lock(mutex_);
        hook = wakeHook_;
    }
    if (hook)
        hook();
}

} // namespace uml::gnss
