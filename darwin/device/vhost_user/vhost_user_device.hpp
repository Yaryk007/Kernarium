// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "shared_memory_regions.hpp"
#include "unique_descriptor.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace uml::virtio {

enum class VhostUserQueueAction : std::uint8_t {
    retry,
    complete,
    defer,
};

struct VhostUserQueueResult final {
    VhostUserQueueAction action {VhostUserQueueAction::retry};
    std::vector<std::uint8_t> response;
};

struct VhostUserQueueCompletion final {
    std::uint64_t requestId {};
    std::vector<std::uint8_t> response;
};

/* The bytes of trivially copyable values laid end to end, as a response or
 * a configuration carries them. */
template<typename... Values>
[[nodiscard]] std::vector<std::uint8_t> toBytes(const Values&... values)
{
    static_assert((std::is_trivially_copyable_v<Values> && ...));
    std::vector<std::uint8_t> result((sizeof(Values) + ... + 0U));
    std::size_t offset = 0;
    ((std::memcpy(result.data() + offset, &values, sizeof(Values)),
      offset += sizeof(Values)),
     ...);
    return result;
}

/* The trivially copyable value at the start of `bytes`. The caller has
 * checked that `bytes` holds at least sizeof(Value). */
template<typename Value>
[[nodiscard]] Value fromBytes(std::span<const std::uint8_t> bytes)
{
    static_assert(std::is_trivially_copyable_v<Value>);
    Value value {};
    std::memcpy(&value, bytes.data(), sizeof(value));
    return value;
}

struct VhostUserSharedMemoryRegion final {
    std::uint8_t id {};
    std::uint64_t size {};
};

struct VhostUserSharedMemoryAction final {
    enum class Type : std::uint8_t {
        map,
        unmap,
    };

    Type type {};
    std::uint64_t actionId {};
    std::uint8_t regionId {};
    std::uint64_t descriptorOffset {};
    std::uint64_t regionOffset {};
    std::uint64_t length {};
    bool writable {};
    uml::gpu::UniqueDescriptor descriptor;
};

class VhostUserDevice {
public:
    using GuestMapper = std::function<std::span<std::uint8_t>(std::uint64_t, std::size_t)>;
    virtual ~VhostUserDevice() = default;

    [[nodiscard]] virtual const char* name() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t features() const noexcept = 0;
    [[nodiscard]] virtual std::size_t queueCount() const noexcept = 0;
    [[nodiscard]] virtual std::vector<std::uint8_t> configuration() const = 0;
    virtual void configurationRead(
        std::uint32_t,
        std::uint32_t) noexcept
    {
    }
    virtual void writeConfiguration(
        std::uint32_t offset,
        std::uint32_t flags,
        std::span<const std::uint8_t> bytes) = 0;
    [[nodiscard]] virtual VhostUserQueueResult processQueue(
        std::uint32_t index,
        std::uint64_t requestId,
        std::span<const std::uint8_t> request,
        std::size_t responseCapacity) = 0;
    /* The answer to a request that is not run because the guest that sent
     * it is ending and its gate has been told to discard (request_gate.hpp).
     * The guest's own shutdown waits for these completions, so the answer is
     * the one a guest cannot tell from success while nothing has happened:
     * the bytes of a response that reports no error, carrying whatever the
     * request needs echoed for its sender to retire it, a fence above all.
     * Empty for a request whose queue returns no response. The device is not
     * called otherwise; this reads the request and changes nothing. */
    [[nodiscard]] virtual std::vector<std::uint8_t> discardedResponse(
        std::uint32_t /* queueIndex */,
        std::span<const std::uint8_t> /* request */) const
    {
        return {};
    }
    virtual void reset() noexcept = 0;
    virtual void prepareReset() noexcept {}
    /* Whether a request buffer on a disabled ring stays there until the ring
     * is enabled again, rather than being completed empty at once. A guest
     * suspends by disabling its rings and sends nothing after, so a device
     * whose requests are asynchronous, like the GPU's, keeps what a late
     * submission or a held request left behind and runs it in order after
     * the resume. A device whose guest can wait for a request synchronously,
     * as a console's output waits with interrupts off, must not: a buffer it
     * never completes would hang the guest's suspend. */
    [[nodiscard]] virtual bool keepsRequestsOnDisabledRing() const noexcept
    {
        return false;
    }
    /* Whether a request the device deferred survives the disable of its
     * ring. A guest suspends by disabling its rings one after another and
     * resumes by enabling them in the same order, sending nothing else but
     * kicks, which keep or drop a disabled ring's request buffers as below,
     * so a device that holds a request across the suspend, a codec stream's
     * queued buffer, is not reset by the disable: the transport keeps what
     * the device completes meanwhile and writes it to the ring when the ring
     * is enabled again. A message that changes the memory, the features, a
     * ring's layout or base, or ends the connection resets such a device
     * first, as does the GET_FEATURES that closes a driver's teardown.
     * Without it, disabling a ring while the device holds a request resets
     * the device. Setting it also keeps the request buffers on a disabled
     * ring, as keepsRequestsOnDisabledRing() does, since the guest can post
     * one before the ring's enable reaches the transport. A device that
     * names a shared-memory region may set it: the GET_FEATURES that would
     * reset the device is not answered while the transport waits for the
     * acknowledgement of a map or unmap, but once the wait is over, behind
     * the messages queued before it. The wait ends with the acknowledgement,
     * the end of the backend-request channel, the end of the main socket or a
     * service stop, and fails when backendAcknowledgementBound of its polls
     * is spent once a reply the front-end waits for is behind it. */
    [[nodiscard]] virtual bool keepsDeferredRequestsWhileDisabled() const noexcept
    {
        return false;
    }
    /* Whether the transport runs a kicked queue with the guest's
     * notifications off: it tells the guest not to kick
     * (VRING_USED_F_NO_NOTIFY) while the queue runs, looks for the buffers
     * the guest adds meanwhile before it finishes, and answers a kick that
     * asks for an answer before it runs the queue rather than after. Only a
     * console's kick asks for one; every other driver's asks for none, so its
     * vCPU never waits for the backend. A device whose requests can run long,
     * or whose guest waits for their completions anyway, sets it, so the
     * guest's kicks do not pile up behind the work. */
    [[nodiscard]] virtual bool answersKicksBeforeRunning() const noexcept
    {
        return false;
    }
    // Called only on the transport worker. Returned spans are borrowed until
    // reset; devices must release all retained guest state in reset().
    virtual void setGuestMapper(GuestMapper) {}
    /* The shared-memory ranges published to this guest, for a device whose
     * guest names buffers by region and offset instead of by guest-physical
     * address. Shared with the backend that owns the region. */
    virtual void setSharedMemoryRegions(
        std::shared_ptr<const SharedMemoryRegions>)
    {
    }

    [[nodiscard]] virtual std::optional<VhostUserSharedMemoryRegion>
        sharedMemoryRegion() const noexcept
    {
        return std::nullopt;
    }
    [[nodiscard]] virtual bool takeSharedMemoryAction(
        VhostUserSharedMemoryAction&)
    {
        return false;
    }
    virtual void completeSharedMemoryAction(
        std::uint64_t,
        bool) noexcept
    {
    }

    // A device with asynchronous device-to-driver data can expose one
    // readiness descriptor. The transport drains it on its worker and then
    // retries queues which may previously have had no device data available.
    // An in-process transport blocks while it is idle, so a device must make
    // this descriptor readable whenever it has work that no message from the
    // guest starts: data for a buffer the guest queued, a completion for
    // takeQueueCompletion(), a config change for configChangePending(), or a
    // timer. A device with no such work returns -1.
    [[nodiscard]] virtual int eventDescriptor() const noexcept { return -1; }
    virtual void drainEventSource() {}
    [[nodiscard]] virtual bool takeQueueCompletion(
        VhostUserQueueCompletion&)
    {
        return false;
    }
    /* A device event may also require a transport-level Virtio config
     * interrupt rather than a virtqueue retry or completion. */
    [[nodiscard]] virtual bool configChangePending() const noexcept
    {
        return false;
    }
    /* Whether the change is announced now. A claim that fails while the change
     * stays pending, or that announces part of it and leaves the rest pending
     * after clearConfigChange(), is made again every 100 ms, for a device
     * whose wait ends without an event on its descriptor. */
    [[nodiscard]] virtual bool claimConfigChange() noexcept
    {
        return configChangePending();
    }
    virtual void releaseConfigChange() noexcept {}
    virtual void clearConfigChange() noexcept {}
    virtual void setStopRequest(const std::atomic_bool*) noexcept {}
    /* Printed after the transport's own readiness line when the device has
     * more to announce, such as where its display can be reached. */
    [[nodiscard]] virtual std::string readinessDescription() const
    {
        return {};
    }
};

} // namespace uml::virtio
