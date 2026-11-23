// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "in_process_vhost_user_service.hpp"
#include "request_gate.hpp"
#include "unique_descriptor.hpp"
#include "vhost_user_device.hpp"
#include "vhost_user_message.hpp"
#include "vhost_user_protocol.hpp"
#include "../../memory/shared_backing.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace uml::gpu {

/* The time the server waits for the front-end to acknowledge a backend
 * request once a reply the front-end itself waits for depends on that: a
 * query, or a request that asks for an acknowledgement, which the server
 * answers only after the backend request is, or the end of the connection,
 * when nothing else will. A front-end blocked on such a reply drains the
 * backend channel meanwhile and answers within microseconds, so this is far
 * beyond what an answer takes, and it is below one 15 second window of the
 * GPU's fence timer (DeferredFences::maximumFenceWait), which the thread that
 * waits here cannot run meanwhile.
 *
 * The wait's own polls spend it, each charged at most the interval it asked
 * for plus a small slack (WaitBudget), not the clock: an interval in which the
 * whole process could not run costs one poll. An awake front-end that stays
 * silent therefore gets this much charged poll time; real time is longer by
 * the work between polls and by an overrun beyond a poll's cap. Only the
 * frames received during such a wait are timed, from their first byte, and an
 * armed wait's budget covers them too; ordinary
 * request reception is not. A wait that nothing waits behind, such as an
 * acknowledgement a suspended guest has not yet sent, has no budget. */
inline constexpr std::chrono::seconds backendAcknowledgementBound {10};

class VhostUserServer {
public:
    explicit VhostUserServer(
        std::unique_ptr<uml::virtio::VhostUserDevice> device);
    ~VhostUserServer();

    VhostUserServer(const VhostUserServer&) = delete;
    VhostUserServer& operator=(const VhostUserServer&) = delete;

    void runConnected(
        int connection,
        const std::atomic_bool& stopRequested,
        const std::function<void()>& ready = {});

    /* Shares one shared-memory index with the other backends of the same
     * guest. The backend that owns a region publishes its ranges here; a
     * device whose guest names buffers inside a region resolves them here.
     * Call before runConnected(). */
    void setSharedMemoryRegions(
        std::shared_ptr<uml::virtio::SharedMemoryRegions> regions);

    /* Lets another thread hold back this server's requests: while the gate
     * is closed none is handed to the device, and each stays in its ring
     * until the gate opens, unless the gate discards them, in which case the
     * server completes each with the device's answer for a request that did
     * not run. Call before runConnected(). */
    void setRequestGate(std::shared_ptr<RequestGate> gate);

    /* Declares that whoever sets the stop flag that runConnected() is given
     * also shuts down the connection's socket, which wakes a poll of the main
     * socket. An idle loop then blocks until a message, a kick, an event of
     * the device (a device signals eventDescriptor() for work that no message
     * starts; see vhost_user_device.hpp), the request gate or the stop.
     * Otherwise it also wakes every UML_GPU_STOP_POLL_TIMEOUT_MILLISECONDS to
     * look at the flag. Call before runConnected(). */
    void setStopShutsDownConnection() noexcept;

private:
    struct Mapping {
        std::uint64_t guestAddress {};
        std::uint64_t userAddress {};
        std::uint64_t size {};
        std::uint64_t mmapOffset {};
        UniqueDescriptor descriptor;
        uml::memory::SharedBackingView backing;
    };

    struct Queue {
        std::uint32_t size {};
        std::uint16_t lastAvailable {};
        std::uint64_t descriptorAddress {};
        std::uint64_t availableAddress {};
        std::uint64_t usedAddress {};
        UniqueDescriptor kickDescriptor;
        UniqueDescriptor callDescriptor;
        bool enabled {};
        bool started {};
        bool polling {};
        bool callPolling {};
    };

    struct DescriptorSegment {
        std::uint64_t address {};
        std::uint32_t length {};
        bool writable {};
    };

    struct PendingRequest {
        std::uint64_t requestId {};
        std::uint32_t queueIndex {};
        std::uint16_t head {};
        std::size_t responseCapacity {};
        std::vector<DescriptorSegment> segments;
    };

    /* A completion of a deferred request whose ring was disabled when the
     * device finished it, kept until the ring is enabled again. */
    struct HeldCompletion {
        std::uint64_t requestId {};
        std::vector<std::uint8_t> response;
    };

    void resetConnection() noexcept;
    void resetDeviceState() noexcept;
    /* Drops every shared-memory range this backend published from the host
     * index, when its device owns a region. */
    void revokeOwnedSharedMemory() noexcept;
    /* Says on stderr that a ring disable is resetting the device under
     * deferred requests. */
    void reportDisableReset(std::uint32_t ring) noexcept;
    /* Whether the device holds deferred requests across a ring disable that
     * has happened: it opts in, a request is pending and a configured ring is
     * disabled. */
    [[nodiscard]] bool requestsAreHeld() const noexcept;
    /* Resets the device first when requestsAreHeld(): the caller is about to
     * change what the held requests refer to, or the guest is tearing down. */
    void retireHeldRequests() noexcept;
    /* Stops reading every ring that is disabled: the driver that disabled it
     * has ended and frees its memory. Only a SET_VRING_KICK or an in-band
     * VRING_KICK starts it again. */
    void retireDisabledRings() noexcept;
    [[nodiscard]] bool everyConfiguredRingIsEnabled() const noexcept;
    /* Writes the held completions of queue `index` to its used ring, in the
     * order the device finished them, and notifies the guest once. */
    void publishHeldCompletions(std::uint32_t index);
    /* The queues have been enabled again: whatever the device finished or
     * raised while one could not run is published and run. */
    void resumeQueues();
    void resetQueues() noexcept;
    void clearMappings() noexcept;
    void handleMessage(int socket, VhostUserMessage& message);
    void sendReply(
        int socket,
        const protocol::VhostHeader& request,
        std::span<const std::uint8_t> payload);
    void sendIntegerReply(
        int socket,
        const protocol::VhostHeader& request,
        std::uint64_t value);
    void sendAcknowledgement(
        int socket,
        const protocol::VhostHeader& request,
        std::uint64_t status);
    void configureMemory(VhostUserMessage& message);
    void configureKick(VhostUserMessage& message);
    void drainKick(std::uint32_t index);
    void processQueue(std::uint32_t index);
    /* Runs queue `index` after its kick was answered, with the guest's kicks
     * suppressed meanwhile, until no buffer is waiting that the guest added
     * while kicks were off. */
    void runAnsweredKick(std::uint32_t index);
    void suppressKicks(std::uint32_t index, bool suppress) noexcept;
    /* How many buffers the guest has made available on queue `index` that the
     * server has not yet taken, or zero if the queue is not running. */
    [[nodiscard]] std::size_t waitingBuffers(std::uint32_t index);
    void dispatchSharedMemoryActions();
    void drainQueueCompletions();
    void signalPendingConfigChange();
    /* How long the main poll of serveConnection() may wait: until a stop
     * when nothing can need the loop but an event, and otherwise for the
     * time a stop check or a retry takes. */
    [[nodiscard]] int mainPollTimeout(bool activePolling) const noexcept;
    void completeRequest(
        const PendingRequest& request,
        std::span<const std::uint8_t> response);
    void notifyQueue(std::uint32_t index);
    void signalQueue(std::uint32_t index);
    /* Waits for the front-end's acknowledgement of the backend request
     * `request` while still answering every front-end request whose reply
     * it awaits without reading the backend channel; other requests are
     * deferred, as is a GET_FEATURES that retires held requests or follows
     * a message already deferred. Throws unless the reply acknowledges
     * `request` with a zero status. It also throws when the front-end closes
     * the main socket, even while the acknowledgement is being received and
     * before anything it would have released is done, and, once a reply the
     * front-end waits for depends on the wait (frontendWaitsForReply()), when
     * the budget (backendAcknowledgementBound) is spent without the
     * acknowledgement. A throw other than a stop closes the backend channel:
     * the request is unanswered, so no later reply on the channel can be paired
     * with it or may complete anything. */
    void awaitBackendAcknowledgement(
        const protocol::VhostHeader& request,
        const char* channel);
    void waitForBackendAcknowledgement(
        const protocol::VhostHeader& request,
        const char* channel);
    /* Whether the front-end is blocked on a reply the server has yet to send,
     * or has no connection left for the server to wait on: a message being
     * handled or deferred that asks for a reply, a teardown. */
    [[nodiscard]] bool frontendWaitsForReply() const noexcept;
    void handleDeferredFrontendMessages(int socket);
    void serveConnection(
        int connection,
        const std::atomic_bool& stopRequested);

    [[nodiscard]] std::uint8_t* translate(
        std::uint64_t address,
        std::size_t length,
        bool userAddress);
    [[nodiscard]] bool readGuest(
        std::uint64_t address,
        std::span<std::uint8_t> destination);
    [[nodiscard]] std::vector<DescriptorSegment> descriptorChain(
        const Queue& queue,
        std::uint16_t head);
    /* The device-readable bytes of a chain, `requestSize` of them. */
    [[nodiscard]] std::vector<std::uint8_t> readRequest(
        std::span<const DescriptorSegment> segments,
        std::size_t requestSize);
    [[nodiscard]] std::uint64_t allocateRequestId();

    std::unique_ptr<uml::virtio::VhostUserDevice> device_;
    UniqueDescriptor slaveRequestDescriptor_;
    std::shared_ptr<uml::virtio::SharedMemoryRegions> sharedMemoryRegions_;
    std::shared_ptr<RequestGate> requestGate_;
    std::vector<Mapping> mappings_;
    std::vector<Queue> queues_;
    std::unordered_map<std::uint64_t, PendingRequest> pendingRequests_;
    std::deque<HeldCompletion> heldCompletions_;
    std::uint64_t nextRequestId_ {1};
    std::uint64_t negotiatedFeatures_ {};
    std::uint64_t negotiatedProtocolFeatures_ {};
    bool featuresConfigured_ {};
    /* How many ring disables reset the device under deferred requests. */
    unsigned disableResets_ {};
    bool stopShutsDownConnection_ {};
    /* The device has a config change pending that it would not claim at the
     * last attempt, or that the last claim left partly pending (the GPU's
     * display is mid-resize), and signals no edge when it will. The loop asks
     * again until it does. */
    bool configChangeWithheld_ {};
    const std::atomic_bool* connectionStopRequested_ {};
    /* The connection's main socket, while one is being served. */
    int frontendDescriptor_ {-1};
    /* Front-end requests read during a backend wait, in arrival order. */
    std::deque<VhostUserMessage> deferredFrontendMessages_;

    /* A front-end request being handled, and whether its reply is still owed,
     * innermost last: a wait inside the handling of it blocks the front-end. */
    struct HandledMessage {
        bool replyOwed {};
        HandledMessage* outer {};
    };
    HandledMessage* handling_ {};
};

} // namespace uml::gpu
