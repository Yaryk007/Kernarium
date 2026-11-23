// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <sys/event.h>

#include <cerrno>
#include <cstdint>
#include <stdexcept>

namespace uml::video {

/* A user event on a device's kqueue, which the device raises on the
 * transport's own thread to have the transport run its queues again. A video
 * device keeps no event buffer of the guest's: it answers the next one the
 * guest offers, or the one it holds in the ring, once an event is waiting, and
 * this is how the transport learns that one is. The kqueue is the device's
 * `eventDescriptor()`. */
inline constexpr std::uintptr_t wakeIdentifier = 1;

inline void registerWake(int queue)
{
    struct kevent change {};
    EV_SET(&change, wakeIdentifier, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (::kevent(queue, &change, 1, nullptr, 0, nullptr) < 0)
        throw std::runtime_error("registering the video device wake-up failed");
}

inline void signalWake(int queue) noexcept
{
    struct kevent change {};
    EV_SET(&change, wakeIdentifier, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    (void)::kevent(queue, &change, 1, nullptr, 0, nullptr);
}

/* Takes the wake-up off a queue that holds nothing else. */
inline void drainWake(int queue)
{
    struct kevent ready[4] {};
    const timespec none {};
    int count;
    do {
        do {
            count = ::kevent(queue, nullptr, 0, ready, 4, &none);
        } while (count < 0 && errno == EINTR);
        if (count < 0) throw std::runtime_error("draining the video device wake-up failed");
    } while (count == 4);
}

} // namespace uml::video
