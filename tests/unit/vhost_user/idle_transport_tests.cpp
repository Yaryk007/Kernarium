// SPDX-License-Identifier: GPL-2.0-or-later
// An in-process vhost-user service blocks while it is idle: it wakes for a
// message, a kick, its device's event descriptor, the request gate and a stop,
// and for nothing else but the retry of a config change its device holds back.
// These tests drive the production service as a front end does, with a
// device that counts the passes of the serving loop and posts work from the
// test's thread, the way a device's own threads do.
// The front end is the GPU tests', so their support header and protocol header
// are on the path.
#include "gpu_test_support.hpp"
#include "in_process_vhost_user_service.hpp"
#include "vhost_user_device.hpp"
#include "watchdog.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace uml::gpu::testing;
using namespace std::chrono_literals;
using uml::gpu::InProcessVhostUserService;
using uml::gpu::UniqueDescriptor;
using uml::testing::require;
using uml::virtio::VhostUserDevice;
using uml::virtio::VhostUserQueueAction;
using uml::virtio::VhostUserQueueCompletion;
using uml::virtio::VhostUserQueueResult;
namespace protocol = uml::gpu::protocol;

constexpr std::uint64_t negotiatedInband = protocol::protocolFeatureReplyAck |
    protocol::protocolFeatureConfig |
    protocol::protocolFeatureInbandNotifications |
    protocol::protocolFeatureSlaveRequest;

/* A device with one request queue, which defers every request until the test
 * finishes it, and an event descriptor the test makes readable after it posts
 * work. The serving loop asks takeQueueCompletion() and configChangePending()
 * once on every pass, so their counts are the loop's passes. */
class PostingDevice final : public VhostUserDevice {
public:
    PostingDevice()
        : wake_(makePipe())
    {
        require(::fcntl(wake_.readEnd.get(), F_SETFL, O_NONBLOCK) == 0);
    }

    [[nodiscard]] const char* name() const noexcept override { return "posting"; }
    [[nodiscard]] std::uint64_t features() const noexcept override { return 0; }
    [[nodiscard]] std::size_t queueCount() const noexcept override { return 1; }
    [[nodiscard]] std::vector<std::uint8_t> configuration() const override
    {
        return {0, 0, 0, 0};
    }
    void writeConfiguration(
        std::uint32_t, std::uint32_t, std::span<const std::uint8_t>) override {}

    [[nodiscard]] VhostUserQueueResult processQueue(
        std::uint32_t,
        std::uint64_t requestId,
        std::span<const std::uint8_t> request,
        std::size_t) override
    {
        const std::lock_guard lock {mutex_};
        ++runs_;
        const std::uint8_t tag = request.empty() ? std::uint8_t {} : request[0];
        if (notReady_)
            return {VhostUserQueueAction::retry, {}};
        if (answerAtOnce_)
            return {VhostUserQueueAction::complete, {tag, static_cast<std::uint8_t>(tag + 1U)}};
        held_.push_back({requestId, tag});
        return {VhostUserQueueAction::defer, {}};
    }
    [[nodiscard]] bool takeQueueCompletion(VhostUserQueueCompletion& completion) override
    {
        passes_.fetch_add(1, std::memory_order_relaxed);
        const std::lock_guard lock {mutex_};
        if (finished_.empty())
            return false;
        completion = std::move(finished_.front());
        finished_.pop_front();
        return true;
    }
    [[nodiscard]] bool configChangePending() const noexcept override
    {
        const std::lock_guard lock {mutex_};
        return configPending_;
    }
    /* Fails while the test withholds the change, except that a partly
     * withheld change announces its other part once, as the GPU announces a
     * memory report while its display is mid-resize. */
    [[nodiscard]] bool claimConfigChange() noexcept override
    {
        const std::lock_guard lock {mutex_};
        ++claims_;
        partialClaim_ = false;
        if (!configPending_)
            return false;
        if (!withheld_)
            return true;
        if (partial_ && !partDelivered_) {
            partDelivered_ = true;
            partialClaim_ = true;
            return true;
        }
        return false;
    }
    void clearConfigChange() noexcept override
    {
        const std::lock_guard lock {mutex_};
        ++announced_;
        if (!partialClaim_)
            configPending_ = false;
    }
    void reset() noexcept override
    {
        const std::lock_guard lock {mutex_};
        held_.clear();
        finished_.clear();
    }
    [[nodiscard]] int eventDescriptor() const noexcept override
    {
        return wake_.readEnd.get();
    }
    void drainEventSource() override
    {
        std::array<char, 16> bytes {};
        while (::read(wake_.readEnd.get(), bytes.data(), bytes.size()) > 0) {}
    }

    /* The calls below are the device's own thread's: here, the test's. */

    /* Finishes the request the guest tagged `tag`, and signals the loop. */
    void finish(std::uint8_t tag)
    {
        {
            const std::lock_guard lock {mutex_};
            const auto held = std::find_if(
                held_.begin(), held_.end(), [tag](const Held& request) {
                    return request.tag == tag;
                });
            require(held != held_.end(), "the device holds no such request");
            finished_.push_back({held->requestId, {tag, static_cast<std::uint8_t>(tag + 1U)}});
            held_.erase(held);
        }
        wake();
    }
    /* Makes the device answer every request it is offered with a retry, as the
     * video decoder's event queue does while it has no event, and then with
     * its answer on the spot. The test's thread calls it before the guest
     * queues a buffer. */
    void holdBack()
    {
        const std::lock_guard lock {mutex_};
        notReady_ = true;
        answerAtOnce_ = true;
    }
    /* Ends the retries, and signals the loop: a device thread's event. */
    void becomeReady()
    {
        {
            const std::lock_guard lock {mutex_};
            notReady_ = false;
        }
        wake();
    }
    /* Raises a config change, and signals the loop. */
    void raiseConfigChange(bool withheld = false)
    {
        {
            const std::lock_guard lock {mutex_};
            configPending_ = true;
            withheld_ = withheld;
            partial_ = false;
            partDelivered_ = false;
        }
        wake();
    }
    /* Lets the next claim announce one part of the withheld change, with no
     * signal: the claim computes it from state the device already holds. */
    void offerPart()
    {
        const std::lock_guard lock {mutex_};
        partial_ = true;
    }
    /* Ends the withholding with no signal, as the GPU's resize lifecycle ends
     * on the UI thread. */
    void release()
    {
        const std::lock_guard lock {mutex_};
        withheld_ = false;
    }

    [[nodiscard]] unsigned passes() const
    {
        return passes_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] unsigned runs() const
    {
        const std::lock_guard lock {mutex_};
        return runs_;
    }
    [[nodiscard]] unsigned claims() const
    {
        const std::lock_guard lock {mutex_};
        return claims_;
    }
    [[nodiscard]] unsigned announced() const
    {
        const std::lock_guard lock {mutex_};
        return announced_;
    }

private:
    struct Held {
        std::uint64_t requestId {};
        std::uint8_t tag {};
    };

    void wake() const
    {
        require(::write(wake_.writeEnd.get(), "x", 1) == 1);
    }

    Pipe wake_;
    mutable std::mutex mutex_;
    std::atomic<unsigned> passes_ {};
    unsigned runs_ {};
    unsigned claims_ {};
    unsigned announced_ {};
    bool notReady_ {};
    bool answerAtOnce_ {};
    bool configPending_ {};
    bool withheld_ {};
    bool partial_ {};
    bool partDelivered_ {};
    bool partialClaim_ {};
    std::deque<Held> held_;
    std::deque<VhostUserQueueCompletion> finished_;
};

constexpr std::size_t requestOffset = 0x300;
constexpr std::size_t responseOffset = 0x380;
constexpr std::uint32_t responseCapacity = 8;

/* The guest of an in-process service, with its queue started the way the
 * guest's driver does: in-band kicks, a call pipe, a backend channel. */
class Session {
public:
    Session()
    {
        service_ = std::make_unique<InProcessVhostUserService>(
            "posting",
            "uml-posting",
            InProcessVhostUserService::DeviceFactory {[this] {
                auto device = std::make_unique<PostingDevice>();
                device_ = device.get();
                return device;
            }});
        socket_.reset(descriptorFromName(service_->takeConnectionName()));
        backend_ = openBackendChannel(socket_.get(), negotiatedInband);
        configureMemory(socket_.get(), ring_.descriptor());
        configureQueue(socket_.get(), ring_, 0);
        call_ = makePipe();
        const std::uint64_t queue = 0;
        const std::array callDescriptors {call_.writeEnd.get()};
        sendRequestAndReadAcknowledgement(
            socket_.get(),
            protocol::VhostRequest::setVringCall,
            bytesOf(queue),
            callDescriptors);
    }

    [[nodiscard]] PostingDevice& device() const { return *device_; }
    [[nodiscard]] InProcessVhostUserService& service() const { return *service_; }
    [[nodiscard]] int backend() const { return backend_.descriptors[0].get(); }
    [[nodiscard]] int callRead() const { return call_.readEnd.get(); }
    [[nodiscard]] const SplitRing& ring() const { return ring_; }

    /* The guest queues the request it tags `tag` and kicks in band, which
     * names no kick pipe; the device holds it once the loop has run it. */
    void postRequest(std::uint8_t tag)
    {
        post(tag);
        awaitCondition([&] { return device_->runs() == 1; });
    }
    /* The same, for a device that may be offered the request again: it waits
     * for the first offer only. */
    void offerBuffer(std::uint8_t tag)
    {
        post(tag);
        awaitCondition([&] { return device_->runs() >= 1; });
    }

    /* Whether the transport answers a message: it is out of whatever it did. */
    void roundTrip() const { (void)readConfig(socket_.get(), 0, 1); }

    /* Stops the service and returns how long that took. */
    [[nodiscard]] std::chrono::steady_clock::duration stopService()
    {
        const uml::testing::Watchdog watchdog {"stopping the service", 5s};
        const auto begin = std::chrono::steady_clock::now();
        service_->stop();
        return std::chrono::steady_clock::now() - begin;
    }

private:
    void post(std::uint8_t tag)
    {
        ring_.memory()[requestOffset] = tag;
        const auto chain = requestChain(
            requestOffset, 1, responseOffset, responseCapacity);
        constexpr std::array<std::uint16_t, 1> heads {0};
        ring_.publish(chain, heads);
        const protocol::VringState kick {.index = 0, .value = 0};
        sendRequest(
            socket_.get(),
            protocol::VhostRequest::vringKick,
            bytesOf(kick),
            {},
            false);
    }

    SplitRing ring_;
    PostingDevice* device_ {};
    std::unique_ptr<InProcessVhostUserService> service_;
    UniqueDescriptor socket_;
    SocketPair backend_;
    Pipe call_;
};

/* A loop that has nothing to do makes no pass: a second of idleness passes at
 * most twice (the loop is still finishing its last message when the window
 * opens), where a loop that wakes every 100 ms passes ten times. */
void idleTransportMakesNoPasses()
{
    Session session;
    session.postRequest(1);
    session.roundTrip();
    std::this_thread::sleep_for(300ms);
    const auto before = session.device().passes();
    std::this_thread::sleep_for(1000ms);
    const auto passes = session.device().passes() - before;
    require(passes <= 2, "the idle transport made " + std::to_string(passes) + " passes in a second");
}

/* A stop wakes a loop that has no timeout to fall back on. Any one stop that
 * returns at once shows the wake works; a wake that fails costs the full
 * timeout when there is one, and hangs when there is none. */
void stopWakesABlockedTransport()
{
    std::chrono::steady_clock::duration fastest = 5s;
    for (int round = 0; round < 5; ++round) {
        Session session;
        session.postRequest(1);
        session.roundTrip();
        std::this_thread::sleep_for(150ms);
        fastest = std::min(fastest, session.stopService());
    }
    require(fastest < 100ms, "no stop of an idle transport returned within 100 ms");
}

/* A request finished by the device's thread is written to the ring and the
 * guest is called, with no kick and no message from the guest. */
void completionFromAnotherThreadReachesTheGuest()
{
    Session session;
    session.postRequest(7);
    session.roundTrip();
    std::this_thread::sleep_for(300ms);

    std::thread device {[&] { session.device().finish(7); }};
    awaitCall(session.callRead());
    device.join();
    require(session.ring().usedIndex() == 1, "the completion was not written to the used ring");
    const auto* response = session.ring().memory().data() + responseOffset;
    require(response[0] == 7 && response[1] == 8, "the response is not the device's");
}

/* A config change raised by the device's thread is announced on the backend
 * channel with no message from the guest. */
void configChangeFromAnotherThreadReachesTheGuest()
{
    Session session;
    session.roundTrip();
    std::this_thread::sleep_for(300ms);

    std::thread device {[&] { session.device().raiseConfigChange(); }};
    const auto request = readBackendRequest(
        session.backend(), protocol::VhostSlaveRequest::configChange);
    device.join();
    replyToBackend(session.backend(), request);
    awaitCondition([&] { return session.device().announced() == 1; });
}

/* A buffer the device cannot use yet stays in the ring, and the device's
 * thread makes it usable by signalling: the video decoder's event queue holds
 * the guest's buffers until the decoder raises an event, and the decoder
 * raises it on its own thread, with no kick and no message from the guest.
 * The transport offers the buffer again for that signal alone. */
void retriedBufferIsOfferedAgainWhenTheDeviceSignals()
{
    Session session;
    session.device().holdBack();
    session.offerBuffer(9);
    session.roundTrip();
    std::this_thread::sleep_for(300ms);
    require(session.ring().usedIndex() == 0, "a buffer the device retried was completed");

    std::thread device {[&] { session.device().becomeReady(); }};
    device.join();
    pollfd called {.fd = session.callRead(), .events = POLLIN};
    require(
        ::poll(&called, 1, 2000) == 1,
        "the buffer the device retried was never offered to it again");
    awaitCall(session.callRead());
    require(session.ring().usedIndex() == 1, "the retried buffer was not completed");
    const auto* response = session.ring().memory().data() + responseOffset;
    require(response[0] == 9 && response[1] == 10, "the response is not the device's");
}

/* A change the device will not yet claim is asked for again, though what the
 * device waits for ends with no event: the GPU's display announces a resize
 * only once the window's resize has ended. The loop idles the rest of the
 * time. */
void withheldConfigChangeIsAskedForAgain()
{
    Session session;
    session.roundTrip();
    std::this_thread::sleep_for(300ms);

    // The event and the pass after it ask twice; the next asks are retries.
    session.device().raiseConfigChange(true);
    awaitCondition([&] { return session.device().claims() >= 4; });
    session.device().release();
    const auto request = readBackendRequest(
        session.backend(), protocol::VhostSlaveRequest::configChange);
    replyToBackend(session.backend(), request);
    awaitCondition([&] { return session.device().announced() == 1; });

    // Announced, the change no longer wakes the loop.
    session.roundTrip();
    std::this_thread::sleep_for(300ms);
    const auto before = session.device().passes();
    std::this_thread::sleep_for(1000ms);
    const auto passes = session.device().passes() - before;
    require(passes <= 2, "the loop kept waking after the change was announced");
}

/* A claim that announces one part of a change while the device holds the
 * other back leaves the change pending, and the rest is asked for again: the
 * GPU announces a memory report while its display's resize is still under way,
 * and that resize may end with no event. The part is announced by a retry, the
 * last ask before the loop would block; the ask that follows an event is not
 * the one that shows it, because the loop asks again after every event. */
void partlyClaimedConfigChangeIsAskedForAgain()
{
    Session session;
    session.roundTrip();
    std::this_thread::sleep_for(300ms);

    session.device().raiseConfigChange(true);
    awaitCondition([&] { return session.device().claims() >= 4; });
    session.device().offerPart();
    auto request = readBackendRequest(
        session.backend(), protocol::VhostSlaveRequest::configChange);
    replyToBackend(session.backend(), request);
    awaitCondition([&] { return session.device().announced() == 1; });

    // The rest follows once the device lets go, with nothing to wake the loop.
    session.device().release();
    pollfd ready {.fd = session.backend(), .events = POLLIN};
    require(
        ::poll(&ready, 1, 2000) == 1,
        "the rest of a partly announced change was never announced");
    request = readBackendRequest(
        session.backend(), protocol::VhostSlaveRequest::configChange);
    replyToBackend(session.backend(), request);
    awaitCondition([&] { return session.device().announced() == 2; });
}

} // namespace

/* With a case's name as its argument, runs that case alone. */
int main(int argc, char** argv)
{
    struct Case {
        const char* name;
        std::function<void()> run;
    };
    const std::vector<Case> cases {
        {"idle", idleTransportMakesNoPasses},
        {"stop", stopWakesABlockedTransport},
        {"completion", completionFromAnotherThreadReachesTheGuest},
        {"retried-buffer", retriedBufferIsOfferedAgainWhenTheDeviceSignals},
        {"config-change", configChangeFromAnotherThreadReachesTheGuest},
        {"withheld-config-change", withheldConfigChangeIsAskedForAgain},
        {"partial-config-change", partlyClaimedConfigChangeIsAskedForAgain},
    };
    try {
        for (const auto& test : cases) {
            if (argc > 1 && std::string {argv[1]} != test.name)
                continue;
            test.run();
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::puts("vhost-user idle transport ok");
    return 0;
}
