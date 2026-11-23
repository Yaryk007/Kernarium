// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The fixtures of the tests of a device that keeps its deferred requests
// across a guest's suspend, behind the real transport: a device with a request
// queue and an event queue, the front end's end of both rings and of the
// backend channel, and a connection whose guest has suspended with requests
// held. The front end is the GPU tests', so their support header and protocol
// header are on the path.
#include "gpu_test_support.hpp"
#include "shared_memory_regions.hpp"
#include "vhost_user_device.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace uml::virtio::held_request_harness {

using namespace uml::gpu::testing;
using uml::gpu::receiveVhostUserMessage;
using uml::gpu::VhostUserMessage;
using uml::gpu::VhostUserServer;
using uml::testing::require;
using uml::virtio::VhostUserDevice;
using uml::virtio::VhostUserQueueAction;
using uml::virtio::VhostUserQueueCompletion;
using uml::virtio::VhostUserQueueResult;
namespace protocol = uml::gpu::protocol;

constexpr std::uint32_t requestQueue = 0;
constexpr std::uint32_t eventQueue = 1;
constexpr std::uint16_t queueDepth = 8;
constexpr std::uint64_t negotiated = protocol::protocolFeatureReplyAck |
    protocol::protocolFeatureConfig;
constexpr std::uint64_t negotiatedInband = negotiated |
    protocol::protocolFeatureInbandNotifications |
    protocol::protocolFeatureSlaveRequest;
constexpr std::uint64_t negotiatedSharedMemory = negotiated |
    protocol::protocolFeatureSlaveRequest |
    protocol::protocolFeatureSharedMemoryMap;

/* The device's tag for a request is the one byte the guest sends; its
 * response to the request is that byte and the next. */
[[nodiscard]] inline std::vector<std::uint8_t> responseTo(std::uint8_t tag)
{
    return {tag, static_cast<std::uint8_t>(tag + 1U)};
}

/* A device with a request queue and an event queue, like a codec. The request
 * queue defers every request until the test finishes it; the event queue
 * answers a buffer with the oldest event the test raised, and keeps the buffer
 * while there is none. With a shared-memory region, each request also maps one
 * sixteenth-of-the-region range of its own (the request tagged `tag` maps range
 * `tag - 1`), and a reset unmaps every range still mapped. With
 * `completesAtOnce` a request is answered by the call that maps its range, as
 * the GPU's blob map is, instead of being held. */
class HoldingDevice final : public VhostUserDevice {
public:
    struct Options {
        bool keepsRequests {};
        bool answersKicksFirst {};
        bool sharedMemory {};
        bool completesAtOnce {};
    };

    explicit HoldingDevice(Options options) : options_(options)
    {
        wake_ = makePipe();
        require(::fcntl(wake_.readEnd.get(), F_SETFL, O_NONBLOCK) == 0);
    }

    static constexpr std::uint64_t rangeLength = 0x4000;
    static constexpr std::uint64_t regionSize = 4 * rangeLength;

    [[nodiscard]] const char* name() const noexcept override { return "holding"; }
    [[nodiscard]] std::uint64_t features() const noexcept override { return 0; }
    [[nodiscard]] std::size_t queueCount() const noexcept override { return 2; }
    [[nodiscard]] std::vector<std::uint8_t> configuration() const override
    {
        return {0, 0, 0, 0};
    }
    void writeConfiguration(
        std::uint32_t, std::uint32_t, std::span<const std::uint8_t>) override {}
    [[nodiscard]] bool keepsDeferredRequestsWhileDisabled() const noexcept override
    {
        return options_.keepsRequests;
    }
    [[nodiscard]] bool keepsRequestsOnDisabledRing() const noexcept override
    {
        return options_.keepsRequests;
    }
    [[nodiscard]] bool answersKicksBeforeRunning() const noexcept override
    {
        return options_.answersKicksFirst;
    }
    [[nodiscard]] std::optional<uml::virtio::VhostUserSharedMemoryRegion>
        sharedMemoryRegion() const noexcept override
    {
        if (!options_.sharedMemory)
            return std::nullopt;
        return uml::virtio::VhostUserSharedMemoryRegion {.id = 0, .size = regionSize};
    }
    [[nodiscard]] bool takeSharedMemoryAction(
        uml::virtio::VhostUserSharedMemoryAction& action) override
    {
        const std::lock_guard lock {mutex_};
        if (actions_.empty())
            return false;
        action = std::move(actions_.front());
        actions_.pop_front();
        return true;
    }
    void completeSharedMemoryAction(std::uint64_t actionId, bool succeeded) noexcept override
    {
        const std::lock_guard lock {mutex_};
        const auto found = issued_.find(actionId);
        if (found == issued_.end() || !succeeded)
            return;
        if (found->second.map) {
            mapped_.push_back(found->second.offset);
            ++mapsCompleted_;
        } else {
            std::erase(mapped_, found->second.offset);
            ++unmapsCompleted_;
        }
    }
    void prepareReset() noexcept override
    {
        const std::lock_guard lock {mutex_};
        for (const auto offset : mapped_)
            queueAction(false, offset, {});
    }

    [[nodiscard]] VhostUserQueueResult processQueue(
        std::uint32_t index,
        std::uint64_t requestId,
        std::span<const std::uint8_t> request,
        std::size_t) override
    {
        const std::lock_guard lock {mutex_};
        ++runs_[index];
        if (index == requestQueue) {
            const auto tag = request.empty() ? std::uint8_t {} : request[0];
            if (options_.sharedMemory) {
                const int duplicate = ::dup(backing_.descriptor());
                require(duplicate >= 0);
                queueAction(true, (tag - 1U) * rangeLength, uml::gpu::UniqueDescriptor {duplicate});
            }
            if (options_.completesAtOnce)
                return {VhostUserQueueAction::complete, responseTo(tag)};
            held_.push_back({requestId, tag});
            return {VhostUserQueueAction::defer, {}};
        }
        if (events_.empty())
            return {VhostUserQueueAction::retry, {}};
        const auto event = events_.front();
        events_.pop_front();
        return {VhostUserQueueAction::complete, uml::virtio::toBytes(event)};
    }
    [[nodiscard]] bool takeQueueCompletion(VhostUserQueueCompletion& completion) override
    {
        const std::lock_guard lock {mutex_};
        if (finished_.empty())
            return false;
        completion = std::move(finished_.front());
        finished_.pop_front();
        ++taken_;
        return true;
    }
    void reset() noexcept override
    {
        const std::lock_guard lock {mutex_};
        ++resets_;
        held_.clear();
        finished_.clear();
        events_.clear();
        actions_.clear();
        mapped_.clear();
    }
    [[nodiscard]] int eventDescriptor() const noexcept override
    {
        return wake_.readEnd.get();
    }
    void drainEventSource() override
    {
        std::array<char, 16> bytes {};
        while (::read(wake_.readEnd.get(), bytes.data(), bytes.size()) > 0) {}
        const std::lock_guard lock {mutex_};
        ++drained_;
    }

    /* The device finishes the request the guest tagged `tag`. */
    void finish(std::uint8_t tag)
    {
        {
            const std::lock_guard lock {mutex_};
            const auto held = std::find_if(
                held_.begin(), held_.end(), [tag](const Held& request) {
                    return request.tag == tag;
                });
            require(held != held_.end(), "the device holds no such request");
            finished_.push_back({held->requestId, responseTo(tag)});
            held_.erase(held);
        }
        wake();
    }
    /* The device raises an event for the event queue's next buffer. */
    void raise(std::uint64_t event)
    {
        {
            const std::lock_guard lock {mutex_};
            events_.push_back(event);
        }
        wake();
    }

    [[nodiscard]] unsigned runs(std::uint32_t queue) const
    {
        const std::lock_guard lock {mutex_};
        return runs_[queue];
    }
    [[nodiscard]] unsigned resets() const
    {
        const std::lock_guard lock {mutex_};
        return resets_;
    }
    [[nodiscard]] std::size_t held() const
    {
        const std::lock_guard lock {mutex_};
        return held_.size();
    }
    [[nodiscard]] unsigned taken() const
    {
        const std::lock_guard lock {mutex_};
        return taken_;
    }
    [[nodiscard]] unsigned drained() const
    {
        const std::lock_guard lock {mutex_};
        return drained_;
    }
    /* The ranges the front end acknowledged mapping and unmapping. */
    [[nodiscard]] unsigned mapsCompleted() const
    {
        const std::lock_guard lock {mutex_};
        return mapsCompleted_;
    }
    [[nodiscard]] unsigned unmapsCompleted() const
    {
        const std::lock_guard lock {mutex_};
        return unmapsCompleted_;
    }
    [[nodiscard]] std::size_t mapped() const
    {
        const std::lock_guard lock {mutex_};
        return mapped_.size();
    }

private:
    struct Held {
        std::uint64_t requestId {};
        std::uint8_t tag {};
    };
    struct Issued {
        bool map {};
        std::uint64_t offset {};
    };

    /* Called with the mutex held. */
    void queueAction(bool map, std::uint64_t offset, uml::gpu::UniqueDescriptor descriptor)
    {
        const auto id = nextActionId_++;
        issued_[id] = {map, offset};
        actions_.push_back(uml::virtio::VhostUserSharedMemoryAction {
            .type = map ? uml::virtio::VhostUserSharedMemoryAction::Type::map
                        : uml::virtio::VhostUserSharedMemoryAction::Type::unmap,
            .actionId = id,
            .regionId = 0,
            .descriptorOffset = 0,
            .regionOffset = offset,
            .length = rangeLength,
            .writable = map,
            .descriptor = std::move(descriptor),
        });
    }

    void wake() const
    {
        require(::write(wake_.writeEnd.get(), "x", 1) == 1);
    }

    Options options_;
    Pipe wake_;
    mutable std::mutex mutex_;
    std::array<unsigned, 2> runs_ {};
    unsigned resets_ {};
    unsigned taken_ {};
    unsigned drained_ {};
    std::deque<Held> held_;
    std::deque<VhostUserQueueCompletion> finished_;
    std::deque<std::uint64_t> events_;
    /* The file the mapped ranges come from. */
    ScratchBacking backing_ {memoryBackingName, regionSize};
    std::deque<uml::virtio::VhostUserSharedMemoryAction> actions_;
    std::map<std::uint64_t, Issued> issued_;
    std::vector<std::uint64_t> mapped_;
    std::uint64_t nextActionId_ {1};
    unsigned mapsCompleted_ {};
    unsigned unmapsCompleted_ {};
};

/* Where a queue's rings lie in the ring's one page: SplitRing places a single
 * queue, so the second takes the upper half. The buffers follow. */
struct Layout {
    std::size_t descriptors;
    std::size_t available;
    std::size_t used;
};
constexpr Layout eventLayout {0x000, 0x100, 0x200};
constexpr Layout requestLayout {0x400, 0x500, 0x600};
constexpr std::size_t eventBufferOffset = 0x300;
constexpr std::size_t requestBufferOffset = 0x700;
constexpr std::size_t responseBufferOffset = 0x780;
constexpr std::size_t bufferStride = 0x10;
constexpr std::uint32_t responseCapacity = 8;

/* One queue of the device in `ring`'s page, driven the way a front end does:
 * placed and enabled, given a call pipe and, when asked, a kick pipe, and fed
 * buffers by the test. */
class TestQueue {
public:
    TestQueue(const SplitRing& ring, std::uint32_t index, Layout layout)
        : ring_(ring), index_(index), layout_(layout) {}

    void start(int socket, bool enable, bool withKickPipe, bool withCallPipe = true)
    {
        const protocol::VringState size {.index = index_, .value = queueDepth};
        sendRequestAndReadAcknowledgement(
            socket, protocol::VhostRequest::setVringNum, bytesOf(size));
        const protocol::VringAddress address {
            .index = index_,
            .flags = 0,
            .descriptor = SplitRing::userBase + layout_.descriptors,
            .used = SplitRing::userBase + layout_.used,
            .available = SplitRing::userBase + layout_.available,
            .log = 0,
        };
        sendRequestAndReadAcknowledgement(
            socket, protocol::VhostRequest::setVringAddr, bytesOf(address));
        if (enable)
            this->enable(socket, true);
        attachChannels(socket, withKickPipe, withCallPipe);
    }
    /* A new call pipe, unless told not to give one (the server then notifies
     * the guest in-band), and a kick pipe when asked. The pipes the server had
     * stay open here: closing the write end of its kick pipe would end the
     * connection before the new one reached it. */
    void attachChannels(int socket, bool withKickPipe, bool withCallPipe = true)
    {
        retired_.push_back(std::move(call_));
        retired_.push_back(std::move(kick_));
        const std::uint64_t queue = index_;
        if (withCallPipe) {
            call_ = makePipe();
            const std::array callDescriptors {call_.writeEnd.get()};
            sendRequestAndReadAcknowledgement(
                socket,
                protocol::VhostRequest::setVringCall,
                bytesOf(queue),
                callDescriptors);
        }
        if (!withKickPipe)
            return;
        kick_ = makePipe();
        const std::array kickDescriptors {kick_.readEnd.get()};
        sendRequestAndReadAcknowledgement(
            socket,
            protocol::VhostRequest::setVringKick,
            bytesOf(queue),
            kickDescriptors);
    }
    void enable(int socket, bool on) const
    {
        sendEnable(socket, on);
        readAcknowledgement(socket, protocol::VhostRequest::setVringEnable);
    }
    /* The enable alone: its acknowledgement comes when the server has
     * handled it, which can be after a backend request it raised. Without
     * `needReply` it asks for none, as a request that is not acknowledged
     * does. */
    void sendEnable(int socket, bool on, bool needReply = true) const
    {
        const protocol::VringState state {.index = index_, .value = on ? 1U : 0U};
        sendRequest(
            socket, protocol::VhostRequest::setVringEnable, bytesOf(state), {}, needReply);
    }
    void writeKickPipe() const { writeKick(kick_); }
    /* An in-band kick. Whatever the kick asks for, it returns once the server
     * has handled it, which a configuration read, answered after it, shows;
     * a reply the kick should not have got would come before that one. */
    void kick(int socket, bool needReply) const
    {
        const protocol::VringState state {.index = index_, .value = 0};
        sendRequest(
            socket, protocol::VhostRequest::vringKick, bytesOf(state), {}, needReply);
        if (needReply)
            readAcknowledgement(socket, protocol::VhostRequest::vringKick);
        (void)readConfig(socket, 0, 1);
    }

    /* The guest queues the request it tags `tag`: one byte for the device and
     * room for its answer. */
    void postRequest(std::uint8_t tag)
    {
        const auto slot = posted_;
        require(slot < 4, "the buffer area holds four requests");
        const auto requestOffset = requestBufferOffset + slot * bufferStride;
        const auto responseOffset = responseBufferOffset + slot * bufferStride;
        ring_.memory()[requestOffset] = tag;
        const auto head = static_cast<std::uint16_t>(2U * slot);
        const auto chain = requestChain(
            requestOffset, 1, responseOffset, responseCapacity, head);
        publish(chain, head);
    }
    /* The guest queues a buffer for the device's next event. */
    void postEventBuffer()
    {
        const std::array<protocol::VirtqueueDescriptor, 1> buffer {{
            {.address = SplitRing::guestBase + eventBufferOffset + posted_ * bufferStride,
             .length = responseCapacity,
             .flags = protocol::descriptorWrite,
             .next = 0}}};
        publish(buffer, static_cast<std::uint16_t>(posted_));
    }

    [[nodiscard]] std::uint16_t usedIndex() const
    {
        return std::atomic_ref<std::uint16_t>(
                   *reinterpret_cast<std::uint16_t*>(
                       ring_.memory().data() + layout_.used + sizeof(std::uint16_t)))
            .load(std::memory_order_acquire);
    }
    [[nodiscard]] protocol::VirtqueueUsedElement used(std::size_t index) const
    {
        protocol::VirtqueueUsedElement element {};
        std::memcpy(
            &element,
            ring_.memory().data() + layout_.used + protocol::splitRingHeaderSize +
                index * sizeof(element),
            sizeof(element));
        return element;
    }
    /* The response the device wrote for request number `slot` of this queue
     * (counting from zero). */
    [[nodiscard]] std::vector<std::uint8_t> responseAt(std::size_t slot, std::size_t size) const
    {
        const auto* bytes = ring_.memory().data() + responseBufferOffset + slot * bufferStride;
        return {bytes, bytes + size};
    }
    [[nodiscard]] std::uint64_t eventAt(std::size_t slot) const
    {
        std::uint64_t event {};
        std::memcpy(
            &event, ring_.memory().data() + eventBufferOffset + slot * bufferStride, sizeof(event));
        return event;
    }

    /* Whether the guest has been called since it last was. */
    [[nodiscard]] bool called(int timeoutMilliseconds = 100) const
    {
        pollfd call {.fd = call_.readEnd.get(), .events = POLLIN};
        if (::poll(&call, 1, timeoutMilliseconds) != 1)
            return false;
        std::uint64_t count {};
        readExact(call_.readEnd.get(), &count, sizeof(count));
        return true;
    }
    void awaitCall() const
    {
        uml::gpu::testing::awaitCall(call_.readEnd.get());
    }

private:
    void publish(
        std::span<const protocol::VirtqueueDescriptor> chain, std::uint16_t head)
    {
        auto* memory = ring_.memory().data();
        std::memcpy(
            memory + layout_.descriptors + head * sizeof(protocol::VirtqueueDescriptor),
            chain.data(),
            chain.size_bytes());
        std::memcpy(
            memory + layout_.available + protocol::splitRingHeaderSize +
                (posted_ % queueDepth) * sizeof(std::uint16_t),
            &head,
            sizeof(head));
        ++posted_;
        std::atomic_ref<std::uint16_t>(
            *reinterpret_cast<std::uint16_t*>(
                memory + layout_.available + sizeof(std::uint16_t)))
            .store(static_cast<std::uint16_t>(posted_), std::memory_order_release);
    }

    const SplitRing& ring_;
    std::uint32_t index_;
    Layout layout_;
    std::size_t posted_ {};
    Pipe call_;
    Pipe kick_;
    std::vector<Pipe> retired_;
};

/* What the process writes to stderr while `run` runs. */
[[nodiscard]] inline std::string captureStderr(const std::function<void()>& run)
{
    std::fflush(stderr);
    std::FILE* file = std::tmpfile();
    require(file != nullptr);
    const int saved = ::dup(STDERR_FILENO);
    require(saved >= 0 && ::dup2(::fileno(file), STDERR_FILENO) >= 0);
    const auto restore = [&] {
        std::fflush(stderr);
        require(::dup2(saved, STDERR_FILENO) >= 0);
        ::close(saved);
    };
    try {
        run();
    } catch (...) {
        restore();
        std::fclose(file);
        throw;
    }
    restore();
    std::rewind(file);
    std::string text;
    std::array<char, 256> chunk {};
    while (const auto count = std::fread(chunk.data(), 1, chunk.size(), file))
        text.append(chunk.data(), count);
    std::fclose(file);
    return text;
}

[[nodiscard]] inline std::size_t countLines(const std::string& text, const std::string& needle)
{
    std::size_t lines = 0;
    for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
        ++lines;
    return lines;
}

/* The front end's end of the backend channel: it reads each request the
 * server sends there and acknowledges it, once the acknowledgements are not
 * held back, and counts the requests by kind. */
class BackendAnswerer {
public:
    explicit BackendAnswerer(int client) : client_(client), thread_([this] { run(); }) {}
    ~BackendAnswerer()
    {
        {
            const std::lock_guard lock {mutex_};
            stopping_ = true;
            held_ = false;
        }
        changed_.notify_all();
        thread_.join();
    }
    BackendAnswerer(const BackendAnswerer&) = delete;
    BackendAnswerer& operator=(const BackendAnswerer&) = delete;

    /* While held, requests are read and counted but not acknowledged. */
    void hold(bool held)
    {
        {
            const std::lock_guard lock {mutex_};
            held_ = held;
        }
        changed_.notify_all();
    }
    /* The next `unmaps` shared-memory unmap requests are answered with a
     * failure status. */
    void refuseUnmaps(unsigned unmaps)
    {
        const std::lock_guard lock {mutex_};
        refusals_ = unmaps;
    }
    /* A reply sent after the server has closed the channel is dropped, as a
     * front end's is, instead of failing the test. */
    void tolerateClosedChannel()
    {
        const int noSignal = 1;
        require(::setsockopt(client_, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal)) == 0);
        const std::lock_guard lock {mutex_};
        tolerant_ = true;
    }
    /* The next reply goes out as its first `bytes` bytes only, and the rest
     * when finishSplitReply() sends it: a frame that is partly there. */
    void splitNextReplyAt(std::size_t bytes)
    {
        const std::lock_guard lock {mutex_};
        splitAt_ = bytes;
    }
    void finishSplitReply()
    {
        std::vector<std::uint8_t> tail;
        {
            const std::lock_guard lock {mutex_};
            tail = std::move(tail_);
            tail_.clear();
        }
        require(!tail.empty(), "no reply is split");
        (void)::send(client_, tail.data(), tail.size(), 0);
    }
    /* How many requests this answerer has replied to, or tried to. */
    [[nodiscard]] unsigned answered() const
    {
        const std::lock_guard lock {mutex_};
        return answered_;
    }
    /* Whether the server has closed its end of the channel, and nothing is
     * left to read on it. */
    [[nodiscard]] bool channelEnded(int timeoutMilliseconds = 2000) const
    {
        pollfd waiting {.fd = client_, .events = POLLIN};
        if (::poll(&waiting, 1, timeoutMilliseconds) != 1)
            return false;
        char byte {};
        return ::recv(client_, &byte, 1, MSG_PEEK | MSG_DONTWAIT) == 0;
    }
    [[nodiscard]] unsigned count(protocol::VhostSlaveRequest request) const
    {
        const std::lock_guard lock {mutex_};
        return counts_[static_cast<std::uint32_t>(request)];
    }
    /* Whether a frame is waiting on the channel that this answerer has not
     * read, which is how a request sent while it holds one shows. */
    [[nodiscard]] bool unreadFrame() const
    {
        pollfd waiting {.fd = client_, .events = POLLIN};
        return ::poll(&waiting, 1, 100) == 1;
    }

private:
    void run()
    {
        for (;;) {
            {
                const std::lock_guard lock {mutex_};
                if (stopping_)
                    return;
            }
            pollfd ready {.fd = client_, .events = POLLIN};
            if (::poll(&ready, 1, 20) != 1)
                continue;
            VhostUserMessage message;
            if (!receiveVhostUserMessage(client_, message))
                return;
            std::unique_lock lock {mutex_};
            require(message.header.request < counts_.size());
            ++counts_[message.header.request];
            changed_.wait(lock, [this] { return !held_; });
            if (stopping_)
                return;
            std::uint64_t status = 0;
            if (message.header.request ==
                    static_cast<std::uint32_t>(protocol::VhostSlaveRequest::sharedMemoryUnmap) &&
                refusals_ != 0) {
                --refusals_;
                status = 1;
            }
            const bool plain = !tolerant_ && splitAt_ == 0;
            const auto splitAt = std::exchange(splitAt_, 0);
            lock.unlock();
            if (plain) {
                replyToBackend(client_, message.header, status);
            } else {
                const protocol::VhostHeader reply {
                    .request = message.header.request,
                    .flags = protocol::vhostUserVersion | protocol::vhostUserReply,
                    .size = sizeof(status),
                };
                std::array<std::uint8_t, sizeof(reply) + sizeof(status)> bytes {};
                std::memcpy(bytes.data(), &reply, sizeof(reply));
                std::memcpy(bytes.data() + sizeof(reply), &status, sizeof(status));
                const auto first = splitAt == 0 ? bytes.size() : std::min(splitAt, bytes.size());
                (void)::send(client_, bytes.data(), first, 0);
                if (first < bytes.size()) {
                    const std::lock_guard tailLock {mutex_};
                    tail_.assign(bytes.begin() + static_cast<std::ptrdiff_t>(first), bytes.end());
                }
            }
            const std::lock_guard answeredLock {mutex_};
            ++answered_;
        }
    }

    int client_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool held_ {};
    bool stopping_ {};
    bool tolerant_ {};
    std::size_t splitAt_ {};
    std::vector<std::uint8_t> tail_;
    unsigned refusals_ {};
    unsigned answered_ {};
    std::array<unsigned, 16> counts_ {};
    std::thread thread_;
};

/* The state of the guest's two queues once the device holds requests: both
 * started and enabled, the request queue holding `requests` requests the
 * device deferred and the event queue holding one buffer. `backend` is the
 * answerer of a device with shared memory. */
struct HeldSession {
    HoldingDevice& device;
    SplitRing& ring;
    TestQueue& requests;
    TestQueue& events;
    int socket;
    BackendAnswerer* backend {};
    /* The host index of the ranges the device's backend published. */
    uml::virtio::SharedMemoryRegions* regions {};
};

/* Runs `body` on a connection whose guest has posted `postedRequests`
 * requests (tags 1, 2, ...) and one event buffer, all of which the device
 * holds (and, with shared memory, has mapped a range for each), and then
 * suspended: both rings disabled. `afterEnd` runs once the connection has
 * ended, with the socket gone. With `releasesBackendEnd` the test keeps no
 * copy of the server's end of the backend channel, so that the end of the
 * channel shows on the front end's. */
inline void withSuspendedGuest(
    HoldingDevice::Options options,
    unsigned postedRequests,
    const std::function<void(const HeldSession&)>& body,
    const std::function<void(const HeldSession&)>& afterEnd = {},
    bool releasesBackendEnd = false)
{
    auto device = std::make_unique<HoldingDevice>(options);
    auto* recorder = device.get();
    VhostUserServer server {std::move(device)};
    auto regions = std::make_shared<uml::virtio::SharedMemoryRegions>();
    if (options.sharedMemory)
        server.setSharedMemoryRegions(regions);
    SplitRing ring;
    TestQueue requests {ring, requestQueue, requestLayout};
    TestQueue events {ring, eventQueue, eventLayout};
    /* They outlive the connection: its end resets the device, and with
     * shared memory unmaps, which the front end must still acknowledge. */
    std::optional<SocketPair> backendChannel;
    std::optional<BackendAnswerer> answerer;

    withServerConnection(server, [&](int socket) {
        if (options.sharedMemory) {
            backendChannel = openBackendChannel(socket, negotiatedSharedMemory);
            if (releasesBackendEnd)
                backendChannel->descriptors[1].reset();
            answerer.emplace(backendChannel->descriptors[0].get());
        } else {
            negotiateProtocolFeatures(socket, negotiated);
        }
        configureMemory(socket, ring.descriptor());
        requests.start(socket, true, true);
        events.start(socket, true, true);
        for (unsigned tag = 1; tag <= postedRequests; ++tag)
            requests.postRequest(static_cast<std::uint8_t>(tag));
        events.postEventBuffer();
        requests.writeKickPipe();
        events.writeKickPipe();
        awaitCondition([&] {
            return recorder->runs(requestQueue) >= postedRequests &&
                recorder->runs(eventQueue) >= 1;
        });
        require(recorder->held() == postedRequests);
        if (options.sharedMemory) {
            awaitCondition([&] { return recorder->mapsCompleted() == postedRequests; });
            require(recorder->mapped() == postedRequests);
        }
        requests.enable(socket, false);
        events.enable(socket, false);
        body({*recorder, ring, requests, events, socket,
              answerer ? &*answerer : nullptr, regions.get()});
    });
    if (afterEnd)
        afterEnd({*recorder, ring, requests, events, -1,
                  answerer ? &*answerer : nullptr, regions.get()});
}

} // namespace uml::virtio::held_request_harness
