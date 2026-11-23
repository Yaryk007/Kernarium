// SPDX-License-Identifier: GPL-2.0-or-later

#include "virtio_gnss_device.hpp"

#include "nmea_writer.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace uml::gnss {

VirtioGnssDevice::VirtioGnssDevice(
    std::shared_ptr<Endpoint> endpoint,
    Options options)
    : endpoint_(std::move(endpoint))
    , period_(options.period)
    , clock_(std::move(options.clock))
{
    if (endpoint_ == nullptr)
        throw std::invalid_argument("Virtio GNSS endpoint is required");
    if (period_ <= std::chrono::milliseconds::zero())
        throw std::invalid_argument("Virtio GNSS epoch period must be positive");
    seenGeneration_ = endpoint_->generation();
}

VirtioGnssDevice::~VirtioGnssDevice()
{
    reset();
}

const char* VirtioGnssDevice::name() const noexcept
{
    return "Virtio Kernarium GNSS";
}

std::uint64_t VirtioGnssDevice::features() const noexcept
{
    return 0;
}

std::size_t VirtioGnssDevice::queueCount() const noexcept
{
    return protocol::queueCount;
}

std::vector<std::uint8_t> VirtioGnssDevice::configuration() const
{
    return {};
}

void VirtioGnssDevice::writeConfiguration(
    std::uint32_t,
    std::uint32_t,
    std::span<const std::uint8_t>)
{
}

int VirtioGnssDevice::eventDescriptor() const noexcept
{
    return endpoint_->eventDescriptor();
}

std::int64_t VirtioGnssDevice::now() const
{
    if (clock_)
        return clock_();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void VirtioGnssDevice::makeEpoch()
{
    const auto time = now();
    epoch_ = nmeaEpoch(endpoint_->latest(time), time);
    delivered_ = 0;
}

void VirtioGnssDevice::setPower(bool powered)
{
    if (powered == powered_)
        return;
    powered_ = powered;
    if (powered) {
        // A report that came while the receiver was off is already in the
        // epoch, so it needs no second one.
        seenGeneration_ = endpoint_->generation();
        endpoint_->setPowered(true, now());
        makeEpoch();
        endpoint_->startTimer(period_);
        // The control queue's kick ran this, so the receive queue's waiting
        // buffers are not tried again unless the transport is told.
        endpoint_->poke();
    } else {
        endpoint_->stopTimer();
        epoch_.clear();
        delivered_ = 0;
        endpoint_->setPowered(false, now());
    }
}

uml::virtio::VhostUserQueueResult VirtioGnssDevice::processQueue(
    std::uint32_t index,
    std::uint64_t,
    std::span<const std::uint8_t> request,
    std::size_t responseCapacity)
{
    const uml::virtio::VhostUserQueueResult complete {
        .action = uml::virtio::VhostUserQueueAction::complete,
        .response = {},
    };
    switch (index) {
    case protocol::receiveQueue: {
        // A buffer the device fills carries no request, and a buffer with no
        // room can never be filled; both are returned empty rather than kept.
        if (!request.empty() || responseCapacity == 0)
            return complete;
        if (!powered_ || delivered_ >= epoch_.size())
            return {};
        const auto count = std::min(responseCapacity, epoch_.size() - delivered_);
        uml::virtio::VhostUserQueueResult result;
        result.action = uml::virtio::VhostUserQueueAction::complete;
        result.response.assign(
            reinterpret_cast<const std::uint8_t*>(epoch_.data()) + delivered_,
            reinterpret_cast<const std::uint8_t*>(epoch_.data()) + delivered_ +
                count);
        delivered_ += count;
        if (delivered_ == epoch_.size()) {
            epoch_.clear();
            delivered_ = 0;
        }
        return result;
    }
    case protocol::controlQueue: {
        if (request.size() == sizeof(protocol::Control)) {
            const auto message =
                uml::virtio::fromBytes<protocol::Control>(request);
            if (message.version == protocol::version)
                setPower(message.power != 0);
        }
        return complete;
    }
    default:
        break;
    }
    throw std::runtime_error("invalid Virtio GNSS queue index");
}

void VirtioGnssDevice::reset() noexcept
{
    try {
        setPower(false);
    } catch (...) {
    }
}

void VirtioGnssDevice::drainEventSource()
{
    const bool timer = endpoint_->drain();
    if (!powered_)
        return;
    const auto generation = endpoint_->generation();
    const bool reported = generation != seenGeneration_;
    if (!timer && !reported)
        return;
    seenGeneration_ = generation;

    // An epoch the guest has not taken any of for a whole period means the
    // guest is not reading: it suspended itself.
    const bool stalled = !epoch_.empty() && delivered_ == 0;
    // An epoch the guest has begun to take is finished before a new one.
    if (delivered_ == 0)
        makeEpoch();
    // A report restarts the period, so the epochs stay a period apart.
    if (reported && !timer)
        endpoint_->startTimer(period_);
    if (timer && stalled)
        endpoint_->requestWake();
}

} // namespace uml::gnss
