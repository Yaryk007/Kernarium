// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "fix.hpp"
#include "unique_descriptor.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace uml::gnss {

/* The receiver's two ends. The host side reports fixes, and learns when the
 * guest powers the receiver on and off; the device reads the fix, and owns the
 * epoch timer. Every call of the host side is safe from any thread. */
class Endpoint final {
public:
    using PowerObserver = std::function<void(bool powered)>;
    using WakeHook = std::function<void()>;

    /* With a `source`, the fixes come from it and report() is ignored. */
    explicit Endpoint(std::unique_ptr<FixSource> source = {});
    ~Endpoint();

    Endpoint(const Endpoint&) = delete;
    Endpoint& operator=(const Endpoint&) = delete;

    /* The host's newest fix. The receiver repeats it for the hold limit of its
     * class (fix.hpp), and a new fix is reported to the guest at once. */
    void report(const Fix& fix);
    /* Drops the held fix: the receiver reports no fix until the next one. */
    void clear();

    /* Called with true when the guest powers its receiver on and with false
     * when it powers it off or resets the device, on the device's thread, so it
     * must return quickly. It is also called with the current state when it is
     * set, if the receiver is on. Null removes it. */
    void setPowerObserver(PowerObserver observer);
    /* Called, on the device's thread and at most once a period, when the
     * guest has powered the receiver and takes none of its output: the guest
     * is suspended. It must return quickly. Null removes it. */
    void setWakeHook(WakeHook hook);
    [[nodiscard]] bool powered() const;

    /* The device's side. Every call below belongs to its thread. */

    /* A kqueue the transport polls: the epoch timer and the host's reports. */
    [[nodiscard]] int eventDescriptor() const noexcept
    {
        return queue_.get();
    }
    /* Consumes what fired and returns whether the timer was among it. Never
     * blocks. */
    [[nodiscard]] bool drain();
    /* Makes the descriptor readable, so the transport runs the device's queues
     * again. */
    void poke() noexcept;
    /* Starts the periodic timer, or restarts its period from now. */
    void startTimer(std::chrono::milliseconds period);
    void stopTimer() noexcept;

    /* Counts every report() and clear(). */
    [[nodiscard]] std::uint64_t generation() const;
    [[nodiscard]] std::optional<Fix> latest(std::int64_t nowUtcNs);
    /* Records the guest's power state and tells the observer. */
    void setPowered(bool powered, std::int64_t nowUtcNs);
    void requestWake();

private:
    uml::gpu::UniqueDescriptor queue_;
    mutable std::mutex mutex_;
    std::unique_ptr<FixSource> source_;
    std::optional<Fix> reported_;
    std::uint64_t generation_ {};
    bool powered_ {};
    PowerObserver observer_;
    WakeHook wakeHook_;
};

} // namespace uml::gnss
