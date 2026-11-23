// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "endpoint.hpp"
#include "vhost_user_device.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace uml::gnss {

/* The host end of the guest's Kernarium GNSS receiver (Virtio device 65534).
 * While the guest has the receiver powered, it fills the guest's receive
 * buffers with one NMEA epoch each period and at once on a new fix; while it
 * is off the device sends nothing. */
class VirtioGnssDevice final : public uml::virtio::VhostUserDevice {
public:
    struct Options final {
        /* The time between epochs. */
        std::chrono::milliseconds period {std::chrono::seconds(1)};
        /* UTC in nanoseconds since 1970-01-01; the system clock when empty. */
        std::function<std::int64_t()> clock;
    };

    explicit VirtioGnssDevice(std::shared_ptr<Endpoint> endpoint)
        : VirtioGnssDevice(std::move(endpoint), Options {})
    {
    }
    VirtioGnssDevice(std::shared_ptr<Endpoint> endpoint, Options options);
    ~VirtioGnssDevice() override;

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
    /* A power message sent just before a suspend is not lost: the driver
     * never waits for it, and the latest one decides the state. */
    [[nodiscard]] bool keepsRequestsOnDisabledRing() const noexcept override
    {
        return true;
    }
    [[nodiscard]] int eventDescriptor() const noexcept override;
    void drainEventSource() override;

private:
    [[nodiscard]] std::int64_t now() const;
    void setPower(bool powered);
    void makeEpoch();

    std::shared_ptr<Endpoint> endpoint_;
    std::chrono::milliseconds period_;
    std::function<std::int64_t()> clock_;
    bool powered_ {};
    /* The epoch being delivered, and how much of it the guest has. */
    std::string epoch_;
    std::size_t delivered_ {};
    std::uint64_t seenGeneration_ {};
};

} // namespace uml::gnss
