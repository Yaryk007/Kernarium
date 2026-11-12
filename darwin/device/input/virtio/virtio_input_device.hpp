// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "held_input.hpp"
#include "platform/descriptor.hpp"
#include "protocol.hpp"
#include "vhost_user_device.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>

namespace uml::input::virtio {

enum class DeviceKind {
    touchscreen,
    keyboard,
};

/* The events a host has sent a guest and the guest has not read yet, a
 * bounded number of them. A report that does not fit is dropped whole, and
 * the queue follows what the guest was told is held (HeldInput) so that a
 * dropped lift or key release does not leave a contact or a key down in it:
 * one report ends everything held, ahead of the next report the queue
 * accepts or, when the guest reads the queue empty first, at that read. */
class EventQueue final {
public:
    EventQueue();

    EventQueue(const EventQueue&) = delete;
    EventQueue& operator=(const EventQueue&) = delete;

    /* Host side. Sends `events`, whole reports each ended by a SYN_REPORT. */
    void enqueue(std::span<const protocol::InputEvent> events) noexcept;
    [[nodiscard]] bool take(protocol::InputEvent& event) noexcept;
    [[nodiscard]] int descriptor() const noexcept;
    void drain();
    void clear() noexcept;

private:
    uml::platform::WakeSocket wake_;
    std::mutex mutex_;
    std::deque<protocol::InputEvent> events_;
    /* What the guest was told is held, from the events queued. */
    HeldInput guestHolds_;
    /* A report was dropped since the guest was last released: it may hold
     * something the host has let go. Cleared, with the release queued, by
     * whichever of enqueue() and take() comes first. */
    bool dropped_ {};
    bool reportedOverflow_ {};
};

class VirtioInputDevice final : public uml::virtio::VhostUserDevice {
public:
    VirtioInputDevice(DeviceKind kind, std::shared_ptr<EventQueue> events);

    [[nodiscard]] const char* name() const noexcept override;
    [[nodiscard]] std::uint64_t features() const noexcept override;
    [[nodiscard]] std::size_t queueCount() const noexcept override;
    [[nodiscard]] std::vector<std::uint8_t> configuration() const override;
    void writeConfiguration(
        std::uint32_t offset,
        std::uint32_t flags,
        std::span<const std::uint8_t> bytes) override;
    [[nodiscard]] uml::virtio::VhostUserQueueResult processQueue(
        std::uint32_t index,
        std::uint64_t requestId,
        std::span<const std::uint8_t> request,
        std::size_t responseCapacity) override;
    void reset() noexcept override;
    [[nodiscard]] int eventDescriptor() const noexcept override;
    void drainEventSource() override;

private:
    [[nodiscard]] protocol::InputConfig selectedConfiguration() const;

    DeviceKind kind_;
    std::shared_ptr<EventQueue> events_;
    std::uint8_t select_ {};
    std::uint8_t subselect_ {};
    bool reportedDelivery_ {};
};

} // namespace uml::input::virtio
