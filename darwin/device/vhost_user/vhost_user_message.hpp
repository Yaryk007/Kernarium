// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "vhost_user_protocol.hpp"
#include "unique_descriptor.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace uml::gpu {

inline constexpr std::size_t maximumVhostUserMessagePayload =
    1024U * 1024U;
// Linux vhost-user memory tables may contain up to 64 regions. Keep the wire
// receiver's descriptor capacity aligned with that protocol limit so a valid
// table cannot be rejected solely by the ancillary-data buffer size.
inline constexpr std::size_t maximumVhostUserDescriptors = 64;

class ReceivedDescriptors final {
public:
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }

    [[nodiscard]] UniqueDescriptor& operator[](std::size_t index) noexcept
    {
        return descriptors_[index];
    }

    [[nodiscard]] const UniqueDescriptor& operator[](
        std::size_t index) const noexcept
    {
        return descriptors_[index];
    }

    [[nodiscard]] bool adopt(UniqueDescriptor descriptor) noexcept
    {
        if (count_ == descriptors_.size())
            return false;
        descriptors_[count_++] = std::move(descriptor);
        return true;
    }

private:
    std::array<UniqueDescriptor, maximumVhostUserDescriptors> descriptors_;
    std::size_t count_ {};
};

struct VhostUserMessage final {
    protocol::VhostHeader header {};
    std::vector<std::uint8_t> payload;
    ReceivedDescriptors descriptors;
};

// This process's open-descriptor ceiling, as the kernel applies it.
struct DescriptorCeiling final {
    std::uint64_t soft {};
    std::uint64_t hard {};
    /* The kernel's own per-process cap. However high the soft limit is set, no
     * descriptor is ever allocated past this; zero when the host will not say.
     */
    std::uint64_t perProcess {};

    // The number that actually decides how many descriptors this process gets.
    [[nodiscard]] std::uint64_t effective() const noexcept
    {
        if (perProcess != 0 && perProcess < soft)
            return perProcess;
        return soft;
    }
};

[[nodiscard]] DescriptorCeiling descriptorCeiling() noexcept;

// One line naming that ceiling, for a startup note or a failure.
[[nodiscard]] std::string describeDescriptorCeiling();

/* Raise the soft open-descriptor ceiling as far as the host permits.
 *
 * A vhost-user backend that publishes a shared-memory region hands the guest
 * one descriptor for every host-visible blob it maps, and the guest holds each
 * one for as long as the mapping lives. The transport is an in-process
 * socketpair, so those descriptors and the backend's own share a single table.
 * Darwin refuses to install an arriving SCM_RIGHTS once that table is full and
 * reports the refusal as EMSGSIZE on recvmsg, which ends the connection and
 * takes the guest's GPU with it. Nothing else in this process raises the
 * ceiling, and an iOS application starts with a small one.
 *
 * Returns the soft ceiling in force afterwards.
 */
std::uint64_t raiseDescriptorCeiling() noexcept;

/* Whether `error`, from a write to a socket of a vhost-user connection, shows
 * that the front-end closed its end: EPIPE, ECONNRESET when it left data
 * unread, or ENOTCONN when the write lands between the kernel's disconnect of
 * the socket and its shutdown for sending. */
[[nodiscard]] bool writeFailureMeansPeerClosed(int error) noexcept;

/* A wait that stayed unanswered for longer than the time it was given:
 * thrown by receiveVhostUserMessage() for a frame that stayed incomplete, and
 * by the server for a backend request its front-end left unacknowledged. */
class VhostUserTimeout final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/* Thrown by receiveVhostUserMessage() when the socket it was asked to watch
 * (FrameBound::watch) ended while the receive waited. */
class VhostUserWatchedPeerEnded final {};

/* Whether the peer of `socket` has closed it or shut it down, however much
 * data is still queued for the reader. A poll that asks for POLLPRI alone
 * reports POLLHUP on Darwin then, and nothing for queued data or an idle peer,
 * so a receive can wake for the end of a socket whose data it must leave
 * alone. */
[[nodiscard]] bool vhostUserPeerHasEnded(int socket) noexcept;

/* The time `allowance` as a person reads it: "10 s", or "300 ms" when it is
 * not a whole number of seconds. */
[[nodiscard]] std::string describeWaitTime(
    std::chrono::steady_clock::duration allowance);

/* Time allowed to a wait, spent by the polls the wait makes and by nothing
 * else: no instant is kept, so a clock that jumps costs nothing. A poll costs
 * the time it took, but never more than the interval it asked for plus
 * `slack`, so a stretch in which the whole process could not run (an
 * application the system has suspended, threads the host holds) costs the one
 * poll it interrupted, not its length. A peer that is awake and silent uses the
 * allowance in its own length of real time, plus what the wait does between
 * polls; a worker delayed past the slack at every poll stretches that by the
 * delay of each poll over (interval + slack). */
class WaitBudget final {
public:
    static constexpr std::chrono::milliseconds slack {25};

    WaitBudget(
        std::chrono::steady_clock::duration allowance,
        std::string whenSpent)
        : remaining_(allowance), whenSpent_(std::move(whenSpent))
    {
    }

    [[nodiscard]] bool spent() const noexcept
    {
        return remaining_ <= std::chrono::steady_clock::duration::zero();
    }

    /* `limit` milliseconds, cut so that a poll ends with the budget. */
    [[nodiscard]] int pollTimeout(int limit) const noexcept
    {
        const auto left =
            std::chrono::ceil<std::chrono::milliseconds>(remaining_).count();
        return static_cast<int>(
            left < 0 ? 0 : (left < limit ? left : limit));
    }

    /* A poll that asked for `askedMilliseconds` and took `elapsed`. */
    void charge(
        std::chrono::steady_clock::duration elapsed,
        int askedMilliseconds) noexcept
    {
        const auto most =
            std::chrono::milliseconds {askedMilliseconds} + slack;
        remaining_ -= elapsed < std::chrono::steady_clock::duration::zero()
            ? std::chrono::steady_clock::duration::zero()
            : (elapsed < most ? elapsed : most);
    }

    [[noreturn]] void fail() const { throw VhostUserTimeout(whenSpent_); }

private:
    std::chrono::steady_clock::duration remaining_;
    std::string whenSpent_;
};

/* How long the rest of a frame may take once its first byte has arrived, and
 * what else limits the receive. The frame's allowance is a WaitBudget that
 * starts at that byte, so a peer with nothing to say is not timed. Time is read
 * from `now` (a null `now` reads std::chrono::steady_clock) and only while a
 * budget runs; an unbounded receive reads no clock. */
struct FrameBound final {
    /* The frame's allowance; zero gives the frame none. */
    std::chrono::steady_clock::duration allowance {};
    std::chrono::steady_clock::time_point (*now)() noexcept {};
    /* The longest one poll of the receive waits; zero is the stop interval. */
    int pollIntervalMilliseconds {};
    /* A budget the receive spends too, which fails the receive with its own
     * report when it is spent before the frame is complete. May be null. */
    WaitBudget* wait {};
    /* A socket whose end ends the receive, which then throws
     * VhostUserWatchedPeerEnded, as soon as it has ended and before it takes
     * a byte more. -1 watches none. */
    int watch {-1};
};

/* Returns false only for an orderly peer close or a truncated stream. A frame
 * that stays incomplete beyond `bound` throws VhostUserTimeout; without a
 * bound the receive waits for the rest of a frame as long as the peer leaves
 * the socket open and nobody asks it to stop. A byte that has arrived is read
 * whatever the time: only a poll that found nothing fails a spent budget. */
[[nodiscard]] bool receiveVhostUserMessage(
    int socket,
    VhostUserMessage& message,
    const std::atomic_bool* stopRequested = nullptr,
    const FrameBound* bound = nullptr);

} // namespace uml::gpu
