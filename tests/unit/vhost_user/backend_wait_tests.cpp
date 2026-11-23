// SPDX-License-Identifier: GPL-2.0-or-later
// The server's wait for the front-end's acknowledgement of a backend request
// (a shared-memory map or unmap, an in-band completion, a configuration
// change), behind the real transport, in time a test moves.
//
// The wait ends with the acknowledgement. A budget runs only while a reply the
// front-end waits for depends on it; the wait's own polls spend it, so a clock
// that jumps, as when the whole process is suspended, costs one poll. A spent
// budget fails the wait and closes the backend channel; a wait nothing is
// behind, such as a notification a suspended guest has not read, has none. The
// end of the main socket aborts the operation before anything the
// acknowledgement would release is written, even while the acknowledgement
// arrives, and a frame received during the wait is bound the same way. A
// service stop ends every wait as it always has.
#include "held_request_harness.hpp"
#include "vhost_user_server_testing.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace uml::virtio::held_request_harness;
using uml::gpu::VhostUserServer;
using std::chrono::milliseconds;

/* The budget and the poll interval the server's waits have while a test runs,
 * so that a wait is spent in half a second: fifty polls of ten milliseconds,
 * where the server's budget is a hundred polls of a hundred. */
constexpr auto testBound = milliseconds {500};
constexpr int testInterval = 10;
/* What the clock advances by at every read once it ticks: the wait reads it
 * twice for each poll, so one poll is charged one tick, and the budget lasts
 * `turns` polls. */
constexpr auto tick = milliseconds {10};
constexpr std::uint64_t turns = testBound / tick;
static_assert(turns * tick == testBound && tick.count() == testInterval);
constexpr auto map = protocol::VhostSlaveRequest::sharedMemoryMap;
constexpr auto unmap = protocol::VhostSlaveRequest::sharedMemoryUnmap;

/* The budget and the poll interval of the waits of every server that runs
 * while this lives. */
class ShortWait {
public:
    ShortWait() { uml::gpu::testing::setBackendWaitTiming(testBound, testInterval); }
    ~ShortWait() { uml::gpu::testing::setBackendWaitTiming({}, 0); }
    ShortWait(const ShortWait&) = delete;
    ShortWait& operator=(const ShortWait&) = delete;
};

/* The clock that times the polls of every wait the server makes while it
 * lives. It stands still, and moves only when the test says: advance() jumps it
 * and startTicking() makes every read move it. The wait reads it twice for
 * each poll it spends, first and last, so the reads are counted from 1 and read
 * 2k is the end of poll k, and atRead() runs a test's action inside one read, on
 * the server's thread, at a known point of the wait. Only a wait that spends a
 * budget reads the clock: a read count of zero shows one that does not. */
class FakeClock : private ShortWait {
public:
    FakeClock()
    {
        base().store(std::chrono::steady_clock::now().time_since_epoch().count());
        offset().store(0);
        reads().store(0);
        tickLength().store(0);
        {
            const std::lock_guard lock {gate().mutex};
            gate().actions.clear();
        }
        uml::gpu::testing::setBackendWaitClock(&FakeClock::now);
    }
    ~FakeClock() { uml::gpu::testing::setBackendWaitClock(nullptr); }
    FakeClock(const FakeClock&) = delete;
    FakeClock& operator=(const FakeClock&) = delete;

    void advance(milliseconds by) { offset().fetch_add(std::chrono::nanoseconds {by}.count()); }
    /* From now every read moves the clock by `by`, and the count starts again.
     * Call it while no wait that spends a budget is running: the server reads
     * the clock in pairs. */
    void startTicking(milliseconds by)
    {
        tickLength().store(std::chrono::nanoseconds {by}.count());
        reads().store(0);
    }
    /* `action` runs inside read number `index`, before it returns. */
    void atRead(std::uint64_t index, std::function<void()> action)
    {
        const std::lock_guard lock {gate().mutex};
        gate().actions[index] = std::move(action);
    }
    /* The server's next read of the clock does not return until release(), so
     * that the test can change what it will find, with the server stopped at a
     * known point of its loop. awaitHeld() returns once it is stopped there. */
    void holdNextRead()
    {
        const std::lock_guard lock {gate().mutex};
        gate().hold = true;
    }
    void awaitHeld() const
    {
        std::unique_lock lock {gate().mutex};
        require(gate().changed.wait_for(
                    lock, std::chrono::seconds {20}, [] { return gate().blocked; }),
                "the server did not read the clock");
    }
    void release()
    {
        {
            const std::lock_guard lock {gate().mutex};
            gate().hold = false;
        }
        gate().changed.notify_all();
    }
    [[nodiscard]] std::uint64_t readCount() const { return reads().load(); }
    /* The server reads the clock `count` more times. */
    void awaitReads(std::uint64_t count) const
    {
        const auto target = readCount() + count;
        require(uml::testing::waitUntil(
                    [&] { return readCount() >= target; }, std::chrono::seconds {20}),
                "the server did not look at the clock");
    }
    /* The server has read the clock at all since `before`, which a wait does
     * once it spends a budget. */
    void awaitFirstRead(std::uint64_t before) const
    {
        require(uml::testing::waitUntil(
                    [&] { return readCount() > before; }, std::chrono::seconds {20}),
                "the server did not start a budget");
    }

private:
    static std::atomic<std::int64_t>& base() { static std::atomic<std::int64_t> value; return value; }
    static std::atomic<std::int64_t>& offset() { static std::atomic<std::int64_t> value; return value; }
    static std::atomic<std::int64_t>& tickLength() { static std::atomic<std::int64_t> value; return value; }
    static std::atomic<std::uint64_t>& reads() { static std::atomic<std::uint64_t> value; return value; }
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        bool hold {};
        bool blocked {};
        std::map<std::uint64_t, std::function<void()>> actions;
    };
    static Gate& gate() { static Gate value; return value; }
    static std::chrono::steady_clock::time_point now() noexcept
    {
        {
            std::unique_lock lock {gate().mutex};
            if (gate().hold) {
                gate().blocked = true;
                gate().changed.notify_all();
                gate().changed.wait(lock, [] { return !gate().hold; });
                gate().blocked = false;
            }
        }
        const auto index = reads().fetch_add(1) + 1;
        std::function<void()> action;
        {
            const std::lock_guard lock {gate().mutex};
            if (const auto found = gate().actions.find(index); found != gate().actions.end()) {
                action = std::move(found->second);
                gate().actions.erase(found);
            }
        }
        if (action)
            action();
        // Taken after the action, so that a jump it makes is in this reading.
        const auto at = offset().fetch_add(tickLength().load());
        return std::chrono::steady_clock::time_point {
            std::chrono::steady_clock::duration {base().load() + at}};
    }
};

/* How many bytes the server sent on `socket` before it closed it, or -1 if it
 * has not within `timeoutMilliseconds`. */
[[nodiscard]] long bytesBeforeEnd(int socket, int timeoutMilliseconds = 20000)
{
    long bytes = 0;
    const auto deadline = std::chrono::steady_clock::now() + milliseconds {timeoutMilliseconds};
    for (;;) {
        const auto left = std::chrono::duration_cast<milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0)
            return -1;
        pollfd ready {.fd = socket, .events = POLLIN};
        if (::poll(&ready, 1, static_cast<int>(left)) != 1)
            return -1;
        std::array<char, 64> chunk {};
        const auto count = ::read(socket, chunk.data(), chunk.size());
        if (count == 0)
            return bytes;
        if (count < 0)
            return -1;
        bytes += count;
    }
}

/* Whether the socket has nothing to read and has not ended. */
[[nodiscard]] bool connectionOpen(int socket)
{
    pollfd ready {.fd = socket, .events = POLLIN};
    return ::poll(&ready, 1, 0) == 0;
}

/* A server on its own thread, whose connection the test can end by closing the
 * main socket and whose stop flag it holds, so that it shows when and why the
 * thread ended. */
class ServerThread {
public:
    explicit ServerThread(VhostUserServer& server)
    {
        auto sockets = makeSocketPair();
        socket_ = std::move(sockets.descriptors[0]);
        worker_ = std::thread {
            [this, &server, connection = sockets.descriptors[1].release()] {
                server.runConnected(connection, stop_);
                exited_.store(true);
            }};
    }
    ~ServerThread()
    {
        stop_.store(true);
        if (worker_.joinable())
            worker_.join();
    }
    ServerThread(const ServerThread&) = delete;
    ServerThread& operator=(const ServerThread&) = delete;

    [[nodiscard]] int socket() const { return socket_.get(); }
    /* The guest lets the device go. */
    void closeSocket()
    {
        (void)::shutdown(socket_.get(), SHUT_RDWR);
        socket_.reset();
    }
    void stop() { stop_.store(true); }
    [[nodiscard]] bool exited() const { return exited_.load(); }
    [[nodiscard]] bool waitForExit(milliseconds timeout = milliseconds {2000}) const
    {
        return uml::testing::waitUntil([&] { return exited_.load(); }, timeout);
    }

private:
    uml::gpu::UniqueDescriptor socket_;
    std::atomic_bool stop_ {};
    std::atomic_bool exited_ {};
    std::thread worker_;
};

/* A connection whose guest has suspended with one request held and mapped, as
 * in the held-request tests, then resumed its request ring and kicked a second
 * request, whose map the front end has not acknowledged. The server is waiting
 * for it, with nothing owed to the front-end: only the event ring is disabled
 * still. The test's copy of the backend channel is closed, so that the
 * server's closing it shows. */
void withPendingMap(const std::function<void(const HeldSession&)>& body)
{
    withSuspendedGuest(
        {true, false, true},
        1,
        [&](const HeldSession& session) {
            auto& backend = *session.backend;
            backend.tolerateClosedChannel();
            session.requests.enable(session.socket, true);
            require(backend.count(map) == 1);
            backend.hold(true);
            session.requests.postRequest(2);
            session.requests.writeKickPipe();
            awaitCondition([&] { return backend.count(map) == 2; });
            body(session);
        },
        {},
        true);
}

/* The front-end's teardown asks for the features while the map is unanswered.
 * The query waits behind it, and a reply the front-end waits for now depends
 * on the wait: the server has begun to count. A request that the wait answers
 * and that follows the query shows that the server has read it. */
void askForTheFeaturesInsideTheWait(int socket)
{
    sendRequest(socket, protocol::VhostRequest::getFeatures, {}, {}, false);
    (void)readConfig(socket, 0, 1);
}

/* The wait ended with a failure, as the probe of a missing acknowledgement
 * showed it never did: the reply the query waited for was not made up, the
 * connection ended, the backend channel is closed, the device was reset, and
 * the host index holds nothing the guest does not. */
void requireWaitFailed(const HeldSession& session, unsigned resetsBefore)
{
    auto& device = session.device;
    awaitCondition([&] { return device.resets() == resetsBefore + 1; });
    require(device.held() == 0 && device.mapped() == 0, "the failed wait left the device state");
    require(session.backend->channelEnded(), "the backend channel was left open");
    require(!session.regions->contains(0, 0, 1) &&
                !session.regions->contains(0, HoldingDevice::rangeLength, 1),
            "a range stayed in the host index");
    require(session.requests.usedIndex() == 0, "an answer was written by a failed wait");
}

/* The backend acknowledges the request it holds, and has done so when this
 * returns: the acknowledgement is on the channel. */
void releaseAcknowledgement(BackendAnswerer& backend)
{
    const auto before = backend.answered();
    backend.hold(false);
    awaitCondition([&] { return backend.answered() == before + 1; });
}

/* The failure of an armed wait that nothing answers. */
constexpr const char* missingAcknowledgementReport =
    "holding vhost-user connection failed: vhost-user shared-memory "
    "acknowledgement missing for 500 ms\n";
constexpr const char* incompleteFrameReport =
    "holding vhost-user connection failed: vhost-user frame incomplete: "
    "its remaining bytes did not arrive within 500 ms of its first byte\n";

/* A missing acknowledgement no longer strands the teardown: once the query has
 * waited the whole budget, the wait fails with a report, the query is not
 * answered, and the connection and the backend channel end. An acknowledgement
 * that arrives afterwards completes nothing. Every poll is charged one tick, so
 * the budget lasts exactly `turns` polls and the failure comes on the poll after
 * them, the last chance of an acknowledgement that is there. */
void aMissingAcknowledgementEndsTheWait()
{
    FakeClock clock;
    withPendingMap([&](const HeldSession& session) {
        auto& device = session.device;
        auto& backend = *session.backend;
        const int socket = session.socket;
        const auto resets = device.resets();

        clock.startTicking(tick);
        askForTheFeaturesInsideTheWait(socket);

        long sent = 0;
        const auto said = captureStderr([&] { sent = bytesBeforeEnd(socket); });
        require(sent == 0, "the wait did not fail, or a reply was made up");
        require(said == missingAcknowledgementReport, "the failure was not reported as expected: " + said);
        requireWaitFailed(session, resets);
        require(clock.readCount() == 2 * turns + 2,
                "the wait failed after " + std::to_string(clock.readCount()) + " reads");

        const auto answered = backend.answered();
        backend.hold(false);
        awaitCondition([&] { return backend.answered() == answered + 1; });
        require(session.requests.usedIndex() == 0, "a late acknowledgement completed a request");
        require(device.resets() == resets + 1);
    });
}

/* The wait is armed by the query and by nothing else, and from the start of the
 * wait the budget holds the whole time the real clock keeps: an awake front-end
 * that stays silent fails the wait after the budget and not before, in the
 * time the wait's polls took. */
void anAwakeSilentFrontEndFailsAfterTheBudget()
{
    ShortWait timing;
    withPendingMap([&](const HeldSession& session) {
        const int socket = session.socket;
        const auto resets = session.device.resets();

        const auto began = std::chrono::steady_clock::now();
        askForTheFeaturesInsideTheWait(socket);
        long sent = 0;
        (void)captureStderr([&] { sent = bytesBeforeEnd(socket); });
        const auto took = std::chrono::steady_clock::now() - began;
        require(sent == 0, "the wait did not fail");
        require(took >= testBound, "the wait failed before its budget could be spent");
        requireWaitFailed(session, resets);
    });
}

/* Where an acknowledgement falls against the last poll the budget pays for:
 * the wait reads the clock before the poll and after it, and the failure comes
 * from a poll that finds nothing after the budget is spent. A read of the clock
 * is the place a test puts the acknowledgement. */
void anAcknowledgementAtReadIsAccepted(std::uint64_t index, bool accepted)
{
    FakeClock clock;
    withPendingMap([&](const HeldSession& session) {
        auto& device = session.device;
        auto& backend = *session.backend;
        const int socket = session.socket;
        const auto resets = device.resets();

        clock.startTicking(tick);
        clock.atRead(index, [&] { releaseAcknowledgement(backend); });
        askForTheFeaturesInsideTheWait(socket);
        if (accepted) {
            bool replied = false;
            const auto said = captureStderr([&] {
                pollfd ready {.fd = socket, .events = POLLIN};
                replied = ::poll(&ready, 1, 20000) == 1;
            });
            require(said.empty(), "the wait reported a failure: " + said);
            require(replied, "the features request was not answered");
            (void)readValueReply<std::uint64_t>(socket, protocol::VhostRequest::getFeatures);
            require(device.resets() == resets + 1 && device.held() == 0 && device.mapped() == 0,
                    "the features request did not reset the device once the wait was over");
            require(!backend.channelEnded(100), "the backend channel was closed");
            (void)readConfig(socket, 0, 1);
        } else {
            long sent = 0;
            const auto said = captureStderr([&] { sent = bytesBeforeEnd(socket); });
            require(sent == 0, "the wait did not fail, or a reply was made up");
            require(said == missingAcknowledgementReport, "the failure was not reported as expected: " + said);
            requireWaitFailed(session, resets);
        }
    });
}

/* A request that asks for an acknowledgement is a reply the front-end waits
 * for, as a query is: the enable of a ring that starts a map arms the budget as
 * the wait begins, and no query is needed. */
void aRequestThatAsksForAReplyStartsTheCount()
{
    FakeClock clock;
    withSuspendedGuest(
        {true, false, true},
        1,
        [&](const HeldSession& session) {
            auto& backend = *session.backend;
            backend.tolerateClosedChannel();
            const int socket = session.socket;
            const auto resets = session.device.resets();
            backend.hold(true);
            session.requests.postRequest(2);
            clock.startTicking(tick);
            session.requests.sendEnable(socket, true);
            awaitCondition([&] { return backend.count(map) == 2; });
            // Answered inside the wait the enable began, so the count has begun.
            (void)readConfig(socket, 0, 1);

            long sent = 0;
            (void)captureStderr([&] { sent = bytesBeforeEnd(socket); });
            // The server releases the sender of the request it could not finish.
            require(sent >= 0, "the wait did not fail");
            requireWaitFailed(session, resets);
            require(clock.readCount() == 2 * turns + 2,
                    "the wait failed after " + std::to_string(clock.readCount()) + " reads");
        },
        {},
        true);
}

/* The whole process cannot run for a while, as when the system suspends an
 * application with a wait in flight: the clock jumps, and the poll that spans
 * the jump is charged what a poll costs, not the jump. The acknowledgement is
 * not queued when the process runs again, because its sender stopped too, and
 * the wait goes on; without it, it fails the poll later than a wait that was
 * never stopped by no more than the slack of one poll. */
void aStretchTheProcessDidNotRunCostsOnePoll(bool acknowledged)
{
    FakeClock clock;
    withPendingMap([&](const HeldSession& session) {
        auto& device = session.device;
        auto& backend = *session.backend;
        const int socket = session.socket;
        const auto resets = device.resets();

        clock.startTicking(tick);
        // The end of poll 10; a year later.
        clock.atRead(20, [&] { clock.advance(milliseconds {365LL * 24 * 3600 * 1000}); });
        if (acknowledged)
            clock.atRead(2 * turns - 12, [&] { releaseAcknowledgement(backend); });
        askForTheFeaturesInsideTheWait(socket);

        if (acknowledged) {
            bool replied = false;
            const auto said = captureStderr([&] {
                pollfd ready {.fd = socket, .events = POLLIN};
                replied = ::poll(&ready, 1, 20000) == 1;
            });
            require(said.empty(), "the wait reported a failure: " + said);
            require(replied, "the features request was not answered");
            (void)readValueReply<std::uint64_t>(socket, protocol::VhostRequest::getFeatures);
            require(device.resets() == resets + 1 && !backend.channelEnded(100));
            (void)readConfig(socket, 0, 1);
        } else {
            long sent = 0;
            const auto said = captureStderr([&] { sent = bytesBeforeEnd(socket); });
            require(sent == 0 && said == missingAcknowledgementReport, "the wait did not fail as it should: " + said);
            requireWaitFailed(session, resets);
            // The jump took 35 ms of the budget, a tick and the slack; a charge of
            // the year would have failed the wait at poll 11.
            constexpr auto slack = std::chrono::duration_cast<milliseconds>(uml::gpu::WaitBudget::slack);
            constexpr std::uint64_t cost = (slack / tick) + 1;
            require(clock.readCount() >= 2 * (turns - cost) + 2 && clock.readCount() <= 2 * turns + 2,
                    "the wait failed after " + std::to_string(clock.readCount()) + " reads");
        }
    });
}

/* The reviewer's second probe: the main socket closes while the map is
 * unanswered, and the request the map came from completes at once. Nothing
 * waits for the clock: the server leaves the wait, writes no answer when the
 * acknowledgement comes late, and ends. */
void theEndOfTheMainSocketAbortsTheWait()
{
    FakeClock clock;
    auto device = std::make_unique<HoldingDevice>(HoldingDevice::Options {true, false, true, true});
    auto* recorder = device.get();
    VhostUserServer server {std::move(device)};
    auto regions = std::make_shared<uml::virtio::SharedMemoryRegions>();
    server.setSharedMemoryRegions(regions);
    SplitRing ring;
    TestQueue requests {ring, requestQueue, requestLayout};
    TestQueue events {ring, eventQueue, eventLayout};
    ServerThread connection {server};
    const int socket = connection.socket();

    auto backendChannel = openBackendChannel(socket, negotiatedSharedMemory);
    backendChannel.descriptors[1].reset();
    BackendAnswerer backend {backendChannel.descriptors[0].get()};
    backend.tolerateClosedChannel();
    configureMemory(socket, ring.descriptor());
    requests.start(socket, true, true);
    events.start(socket, true, true);
    backend.hold(true);
    requests.postRequest(1);
    requests.writeKickPipe();
    awaitCondition([&] { return backend.count(map) == 1; });
    const auto resets = recorder->resets();

    bool exited = false;
    const auto said = captureStderr([&] {
        connection.closeSocket();
        exited = connection.waitForExit();
    });
    require(exited, "the end of the main socket left the wait running");
    require(said.empty(), "the end of the connection was reported as a failure: " + said);
    require(uml::testing::waitUntil([&] { return recorder->resets() == resets + 1; },
                                    std::chrono::seconds {2}),
            "the device was not reset");
    require(backend.channelEnded(), "the backend channel was left open");

    const auto answered = backend.answered();
    backend.hold(false);
    awaitCondition([&] { return backend.answered() == answered + 1; });
    require(requests.usedIndex() == 0, "an answer was written after the main socket closed");
    require(ring.memory()[responseBufferOffset] == 0 && ring.memory()[responseBufferOffset + 1] == 0,
            "response bytes were written after the main socket closed");
    require(recorder->resets() == resets + 1 && recorder->held() == 0 && recorder->mapped() == 0);
    require(!regions->contains(0, 0, 1), "a range stayed in the host index");
    require(clock.readCount() == 0, "the abort spent a budget");
}

/* The end of the main socket while the acknowledgement is being received,
 * which the reviewer's probe showed releasing the completion: nothing the
 * acknowledgement would have released is written. `when` says where the end
 * falls in the reception of one acknowledgement: 0 after the first bytes of a
 * frame that is sent in two parts, 1 after the header of one sent whole, as the
 * receiver is about to poll for the rest, and 2 after the last byte, with no
 * poll left. */
void theEndOfTheMainSocketDuringAnAcknowledgementAbortsTheWait(int when)
{
    FakeClock clock;
    auto device = std::make_unique<HoldingDevice>(HoldingDevice::Options {true, false, true, true});
    auto* recorder = device.get();
    VhostUserServer server {std::move(device)};
    auto regions = std::make_shared<uml::virtio::SharedMemoryRegions>();
    server.setSharedMemoryRegions(regions);
    SplitRing ring;
    TestQueue requests {ring, requestQueue, requestLayout};
    TestQueue events {ring, eventQueue, eventLayout};
    ServerThread connection {server};
    const int socket = connection.socket();

    auto backendChannel = openBackendChannel(socket, negotiatedSharedMemory);
    backendChannel.descriptors[1].reset();
    BackendAnswerer backend {backendChannel.descriptors[0].get()};
    backend.tolerateClosedChannel();
    configureMemory(socket, ring.descriptor());
    requests.start(socket, true, true);
    events.start(socket, true, true);
    backend.hold(true);
    requests.postRequest(1);
    requests.writeKickPipe();
    awaitCondition([&] { return backend.count(map) == 1; });
    const auto resets = recorder->resets();

    // The wait reads the clock once a frame has begun: the first poll after the
    // header is read 1, and its end read 2.
    clock.startTicking(tick);
    if (when != 0)
        clock.atRead(when, [&] { connection.closeSocket(); });
    else
        backend.splitNextReplyAt(5);
    bool exited = false;
    const auto said = captureStderr([&] {
        backend.hold(false);
        if (when == 0) {
            awaitCondition([&] { return backend.answered() == 1; });
            clock.awaitFirstRead(0);
            connection.closeSocket();
        }
        exited = connection.waitForExit();
    });
    require(exited, "the end of the main socket left the wait running");
    require(said.empty(), "the end of the connection was reported as a failure: " + said);
    if (when == 0)
        backend.finishSplitReply();
    awaitCondition([&] { return backend.answered() == 1; });
    require(requests.usedIndex() == 0, "an answer was written after the main socket closed");
    require(ring.memory()[responseBufferOffset] == 0 && ring.memory()[responseBufferOffset + 1] == 0,
            "response bytes were written after the main socket closed");
    require(recorder->mapsCompleted() == 0, "the map was completed after the main socket closed");
    require(recorder->resets() == resets + 1 && recorder->held() == 0 && recorder->mapped() == 0);
    require(!regions->contains(0, 0, 1), "a range stayed in the host index");
    require(backend.channelEnded(), "the backend channel was left open");
}

/* A guest that suspends with a backend notification unread leaves the server
 * waiting for it, and nothing is behind that wait: no time, however long the
 * guest is held, fails it. The guest's queries are answered meanwhile, and the
 * acknowledgement that comes when it wakes ends the wait and the work goes on. */
void aNotificationPendingAcrossASuspendIsNotAFailure()
{
    FakeClock clock;
    auto device = std::make_unique<HoldingDevice>(HoldingDevice::Options {true, false, false, true});
    auto* recorder = device.get();
    VhostUserServer server {std::move(device)};
    SplitRing ring;
    TestQueue requests {ring, requestQueue, requestLayout};
    TestQueue events {ring, eventQueue, eventLayout};

    withServerConnection(server, [&](int socket) {
        auto backendChannel = openBackendChannel(socket, negotiatedInband);
        backendChannel.descriptors[1].reset();
        BackendAnswerer backend {backendChannel.descriptors[0].get()};
        configureMemory(socket, ring.descriptor());
        // The request queue has no call descriptor: the server calls in-band.
        requests.start(socket, true, true, false);
        events.start(socket, true, true);
        backend.hold(true);
        requests.postRequest(1);
        requests.writeKickPipe();
        constexpr auto call = protocol::VhostSlaveRequest::vringCall;
        awaitCondition([&] { return backend.count(call) == 1; });
        require(requests.usedIndex() == 1, "the request was not completed before its notification");
        const auto resets = recorder->resets();

        // The guest is suspended: a year passes, and its queries are answered.
        clock.startTicking(tick);
        clock.advance(milliseconds {365LL * 24 * 3600 * 1000});
        (void)readConfig(socket, 0, 1);
        bool stillWaiting = false;
        // A budget would be spent within the time it takes to run out.
        const auto said = captureStderr([&] {
            stillWaiting = connectionOpen(socket) && !backend.channelEnded(2 * testBound.count());
        });
        require(stillWaiting, "a pending notification was failed while the guest was suspended");
        require(said.empty(), "the wait reported a failure: " + said);

        // It wakes and acknowledges.
        const auto answered = backend.answered();
        backend.hold(false);
        awaitCondition([&] { return backend.answered() == answered + 1; });
        (void)getFeatures(socket);
        require(connectionOpen(socket) && recorder->resets() == resets);
        requests.postRequest(2);
        requests.writeKickPipe();
        awaitCondition([&] { return requests.usedIndex() == 2; });
        (void)readConfig(socket, 0, 1);
    });
}

/* The same guest, suspending as the maintained front-end does: it disables its
 * rings and waits for each reply while it serves the backend channel, so its
 * acknowledgement comes at once, and an ordinary suspend is not turned into a
 * failure by the budget the replies it waits for arm. */
void aSuspendWithANotificationPendingAnswersInTime()
{
    FakeClock clock;
    auto device = std::make_unique<HoldingDevice>(HoldingDevice::Options {true, false, false, true});
    auto* recorder = device.get();
    VhostUserServer server {std::move(device)};
    SplitRing ring;
    TestQueue requests {ring, requestQueue, requestLayout};
    TestQueue events {ring, eventQueue, eventLayout};

    withServerConnection(server, [&](int socket) {
        auto backendChannel = openBackendChannel(socket, negotiatedInband);
        backendChannel.descriptors[1].reset();
        BackendAnswerer backend {backendChannel.descriptors[0].get()};
        configureMemory(socket, ring.descriptor());
        requests.start(socket, true, true, false);
        events.start(socket, true, true);
        backend.hold(true);
        requests.postRequest(1);
        requests.writeKickPipe();
        awaitCondition([&] { return backend.count(protocol::VhostSlaveRequest::vringCall) == 1; });
        const auto resets = recorder->resets();

        clock.startTicking(tick);
        events.sendEnable(socket, false);
        requests.sendEnable(socket, false);
        (void)readConfig(socket, 0, 1);
        // Armed now: a few polls have been spent, and the front-end drains the
        // channel and answers.
        clock.awaitReads(4);
        require(connectionOpen(socket) && !backend.channelEnded(100));

        backend.hold(false);
        readAcknowledgement(socket, protocol::VhostRequest::setVringEnable);
        readAcknowledgement(socket, protocol::VhostRequest::setVringEnable);
        require(recorder->resets() == resets, "the suspend reset the device");
        require(connectionOpen(socket) && !backend.channelEnded(100), "the suspend was failed");
        (void)readConfig(socket, 0, 1);
    });
}

/* A partial acknowledgement that never completes: the first bytes of the frame
 * arrive and the rest does not, and nothing is owed to the front-end. The frame
 * has its own allowance from its first byte, spent by the polls that wait for
 * the rest. */
void aPartialAcknowledgementThatNeverCompletesFails()
{
    FakeClock clock;
    withPendingMap([&](const HeldSession& session) {
        auto& backend = *session.backend;
        const int socket = session.socket;
        const auto resets = session.device.resets();

        clock.startTicking(tick);
        backend.splitNextReplyAt(5);
        long sent = 0;
        const auto said = captureStderr([&] {
            backend.hold(false);
            sent = bytesBeforeEnd(socket);
        });
        require(sent == 0, "the incomplete frame did not fail the wait");
        require(said == incompleteFrameReport, "the failure was not reported as expected: " + said);
        requireWaitFailed(session, resets);
        require(clock.readCount() == 2 * turns + 2,
                "the frame failed after " + std::to_string(clock.readCount()) + " reads");
    });
}

/* A frame that completes before its allowance is spent is accepted however
 * long its first bytes were alone, and a clock that jumps while it is partly
 * there costs one poll, as in the wait: the allowance counts from the first
 * byte, and a late peer is not a silent one. The wait is armed or not. */
void aFrameThatCompletesInTimeIsAccepted(bool armed)
{
    FakeClock clock;
    withPendingMap([&](const HeldSession& session) {
        auto& backend = *session.backend;
        const int socket = session.socket;
        const auto resets = session.device.resets();

        clock.startTicking(tick);
        backend.splitNextReplyAt(5);
        // The end of poll 5 inside the frame, and the rest of it at poll 40.
        clock.atRead(10, [&] { clock.advance(milliseconds {365LL * 24 * 3600 * 1000}); });
        clock.atRead(80, [&] { backend.finishSplitReply(); });
        if (armed) {
            // The first bytes are sent at poll 3 of the armed wait.
            clock.atRead(6, [&] { backend.hold(false); });
            askForTheFeaturesInsideTheWait(socket);
        } else {
            backend.hold(false);
            awaitCondition([&] { return backend.answered() == 2; });
        }
        const auto said = captureStderr([&] {
            // The map was acknowledged: the request ring's request is held, and
            // the connection goes on.
            awaitCondition([&] { return session.device.mapsCompleted() == 2; });
            if (armed)
                (void)readValueReply<std::uint64_t>(socket, protocol::VhostRequest::getFeatures);
            (void)readConfig(socket, 0, 1);
        });
        require(said.empty(), "the late frame was reported as a failure: " + said);
        require(armed || session.device.resets() == resets);
    });
}

/* A partial request from the front-end, in the wait, is bound the same way,
 * and a fragment that arrives later does not start the allowance again. */
void aPartialRequestFromTheFrontEndFails()
{
    FakeClock clock;
    withPendingMap([&](const HeldSession& session) {
        const int socket = session.socket;
        const auto resets = session.device.resets();

        clock.startTicking(tick);
        const std::array<std::uint8_t, 5> beginning {};
        // More of the header comes at poll 20, which does not renew the
        // allowance: it is counted from the first byte.
        clock.atRead(40, [&] {
            require(::write(socket, beginning.data(), 2) == 2);
        });
        long sent = 0;
        const auto said = captureStderr([&] {
            require(::write(socket, beginning.data(), beginning.size()) ==
                    static_cast<ssize_t>(beginning.size()));
            sent = bytesBeforeEnd(socket);
        });
        require(sent == 0, "the incomplete request did not fail the wait");
        require(said == incompleteFrameReport, "the failure was not reported as expected: " + said);
        requireWaitFailed(session, resets);
        require(clock.readCount() == 2 * turns + 2,
                "the frame failed after " + std::to_string(clock.readCount()) + " reads");
    });
}

/* The wait is armed at the start, and a frame that begins late in it does not
 * get a fresh allowance: the wait's budget runs on, spent by the polls the
 * frame makes too, and the wait fails with the report of the acknowledgement
 * that is missing, as soon as the budget is spent and not an allowance after
 * the frame began. */
void anArmedBudgetCoversAFrameThatBeginsInIt()
{
    FakeClock clock;
    withPendingMap([&](const HeldSession& session) {
        auto& backend = *session.backend;
        const int socket = session.socket;
        const auto resets = session.device.resets();

        clock.startTicking(tick);
        backend.splitNextReplyAt(5);
        // The first bytes arrive after poll 20, and the rest never.
        clock.atRead(40, [&] { backend.hold(false); });
        askForTheFeaturesInsideTheWait(socket);
        long sent = 0;
        const auto said = captureStderr([&] { sent = bytesBeforeEnd(socket); });
        require(sent == 0, "the wait did not fail");
        require(said == missingAcknowledgementReport, "the failure was not reported as expected: " + said);
        requireWaitFailed(session, resets);
        // The frame polls twice per poll too: the budget was spent by the poll
        // the frame began in, and the failure is the one after it, give or take
        // the poll the frame's first bytes interrupted.
        require(clock.readCount() >= 2 * turns && clock.readCount() <= 2 * turns + 6,
                "the wait failed after " + std::to_string(clock.readCount()) + " reads");
    });
}

/* A reset's unmaps are a wait the guest no longer has a connection behind:
 * once the main socket is gone the teardown has only the acknowledgements to
 * wait for, and fails them when the budget is spent, with the device reset and
 * every range dropped from the host index. */
void aTeardownUnmapThatIsNeverAcknowledgedFails()
{
    FakeClock clock;
    auto device = std::make_unique<HoldingDevice>(HoldingDevice::Options {true, false, true});
    auto* recorder = device.get();
    VhostUserServer server {std::move(device)};
    auto regions = std::make_shared<uml::virtio::SharedMemoryRegions>();
    server.setSharedMemoryRegions(regions);
    SplitRing ring;
    TestQueue requests {ring, requestQueue, requestLayout};
    TestQueue events {ring, eventQueue, eventLayout};
    ServerThread connection {server};
    const int socket = connection.socket();

    auto backendChannel = openBackendChannel(socket, negotiatedSharedMemory);
    backendChannel.descriptors[1].reset();
    BackendAnswerer backend {backendChannel.descriptors[0].get()};
    backend.tolerateClosedChannel();
    configureMemory(socket, ring.descriptor());
    requests.start(socket, true, true);
    events.start(socket, true, true);
    requests.postRequest(1);
    requests.writeKickPipe();
    awaitCondition([&] { return recorder->mapsCompleted() == 1; });
    require(regions->contains(0, 0, 1));

    // The guest goes without a word to the device, and the unmap is not answered.
    backend.hold(true);
    const auto resets = recorder->resets();
    clock.startTicking(tick);
    bool exited = false;
    const auto said = captureStderr([&] {
        connection.closeSocket();
        awaitCondition([&] { return backend.count(unmap) == 1; });
        exited = connection.waitForExit(milliseconds {20000});
    });
    require(exited, "the teardown went on waiting for the unmap");
    require(said == "holding vhost-user reset: vhost-user shared-memory acknowledgement missing for "
                    "500 ms\n",
            "the failure was not reported as expected: " + said);
    require(clock.readCount() == 2 * turns + 2,
            "the teardown gave up after " + std::to_string(clock.readCount()) + " reads");
    require(recorder->resets() == resets + 1,
            "the device was reset " + std::to_string(recorder->resets() - resets) + " times");
    require(recorder->held() == 0 && recorder->mapped() == 0, "the reset left state");
    require(!regions->contains(0, 0, 1), "a range stayed in the host index");
    require(backend.channelEnded(), "the backend channel was left open");
}

/* What a budget charges: a poll costs what it took, never more than the
 * interval it asked for and the slack, and nothing for a clock that ran
 * backwards; the budget is spent exactly when the polls' charges add up to it. */
void aBudgetIsSpentByThePollsItIsChargedFor()
{
    using uml::gpu::WaitBudget;
    constexpr auto year = milliseconds {365LL * 24 * 3600 * 1000};
    WaitBudget budget {testBound, "spent"};
    require(!budget.spent() && budget.pollTimeout(1000) == testBound.count());
    require(budget.pollTimeout(testInterval) == testInterval, "a poll was asked longer than its interval");

    budget.charge(year, testInterval);
    require(budget.pollTimeout(1000) == testBound.count() - testInterval - WaitBudget::slack.count(),
            "a stretch the process did not run cost more than one poll");
    budget.charge(-year, testInterval);
    require(budget.pollTimeout(1000) == testBound.count() - testInterval - WaitBudget::slack.count(),
            "a clock that ran backwards was charged");
    budget.charge(milliseconds {3}, testInterval);
    require(budget.pollTimeout(1000) == testBound.count() - testInterval - WaitBudget::slack.count() - 3,
            "a poll was not charged what it took");

    WaitBudget exact {testBound, "spent"};
    for (std::uint64_t poll = 1; poll < turns; ++poll) {
        exact.charge(tick, testInterval);
        require(!exact.spent(), "the budget was spent before its polls were");
    }
    require(exact.pollTimeout(testInterval) == testInterval);
    exact.charge(tick, testInterval);
    require(exact.spent() && exact.pollTimeout(testInterval) == 0, "the budget outlived its polls");
    bool failed = false;
    try {
        exact.fail();
    } catch (const uml::gpu::VhostUserTimeout& timeout) {
        failed = std::string {timeout.what()} == "spent";
    }
    require(failed, "a spent budget did not fail with its report");
}

/* A poll for POLLPRI alone reports the end of a socket, with data queued or
 * not, and reports nothing for data or an idle peer: the signal a receive
 * wakes for without taking what the front-end has sent. */
void thePeerOfASocketIsSeenToEndBehindQueuedData()
{
    using uml::gpu::vhostUserPeerHasEnded;
    auto pair = makeSocketPair();
    const int ours = pair.descriptors[0].get();
    require(!vhostUserPeerHasEnded(ours), "an idle peer was seen to end");
    const std::array<std::uint8_t, 4> data {1, 2, 3, 4};
    require(::write(pair.descriptors[1].get(), data.data(), data.size()) == 4);
    require(!vhostUserPeerHasEnded(ours), "queued data was seen as the end of the peer");
    pair.descriptors[1].reset();
    require(vhostUserPeerHasEnded(ours), "the end of the peer behind queued data was not seen");

    auto shut = makeSocketPair();
    require(!vhostUserPeerHasEnded(shut.descriptors[0].get()));
    require(::shutdown(shut.descriptors[1].get(), SHUT_RDWR) == 0);
    require(vhostUserPeerHasEnded(shut.descriptors[0].get()), "a shut down peer was not seen to end");
}

/* A service stop still ends the wait, with or without a reply owed behind it,
 * at once and without a report. */
void aStopEndsTheWait(bool replyOwed)
{
    FakeClock clock;
    auto device = std::make_unique<HoldingDevice>(HoldingDevice::Options {true, false, true});
    auto* recorder = device.get();
    VhostUserServer server {std::move(device)};
    SplitRing ring;
    TestQueue requests {ring, requestQueue, requestLayout};
    TestQueue events {ring, eventQueue, eventLayout};
    ServerThread connection {server};
    const int socket = connection.socket();

    auto backendChannel = openBackendChannel(socket, negotiatedSharedMemory);
    BackendAnswerer backend {backendChannel.descriptors[0].get()};
    configureMemory(socket, ring.descriptor());
    requests.start(socket, true, true);
    events.start(socket, true, true);
    requests.postRequest(1);
    requests.writeKickPipe();
    awaitCondition([&] { return recorder->mapsCompleted() == 1; });
    backend.hold(true);
    requests.postRequest(2);
    requests.writeKickPipe();
    awaitCondition([&] { return backend.count(map) == 2; });
    if (replyOwed) {
        // A ring disable that asks for an acknowledgement waits behind the map.
        events.sendEnable(socket, false);
        (void)readConfig(socket, 0, 1);
    }
    require(!connection.exited());
    const auto resets = recorder->resets();

    bool exited = false;
    const auto said = captureStderr([&] {
        connection.stop();
        exited = connection.waitForExit();
    });
    require(exited, "a stop did not end the wait");
    require(said.empty(), "a stop was reported as a failure: " + said);
    require(uml::testing::waitUntil([&] { return recorder->resets() == resets + 1; },
                                    std::chrono::seconds {2}),
            "the stop did not reset the device");
    require(recorder->held() == 0 && recorder->mapped() == 0);
}

} // namespace

/* With a case's name as its argument, runs that case alone. */
int main(int argc, char** argv)
{
    // A reply to a channel the server has closed is dropped, not fatal.
    std::signal(SIGPIPE, SIG_IGN);
    struct Case {
        std::string name;
        std::function<void()> run;
    };
    const std::vector<Case> cases {
        {"missing-acknowledgement", aMissingAcknowledgementEndsTheWait},
        {"awake-silent-front-end", anAwakeSilentFrontEndFailsAfterTheBudget},
        {"acknowledgement-before-the-bound", [] { anAcknowledgementAtReadIsAccepted(2 * turns - 1, true); }},
        {"acknowledgement-at-the-bound", [] { anAcknowledgementAtReadIsAccepted(2 * turns + 1, true); }},
        {"acknowledgement-after-the-bound", [] { anAcknowledgementAtReadIsAccepted(2 * turns + 2, false); }},
        {"reply-request-starts-the-count", aRequestThatAsksForAReplyStartsTheCount},
        {"stretch-not-run-acknowledged", [] { aStretchTheProcessDidNotRunCostsOnePoll(true); }},
        {"stretch-not-run-unanswered", [] { aStretchTheProcessDidNotRunCostsOnePoll(false); }},
        {"main-socket-closed", theEndOfTheMainSocketAbortsTheWait},
        {"main-socket-closed-in-a-split-acknowledgement",
         [] { theEndOfTheMainSocketDuringAnAcknowledgementAbortsTheWait(0); }},
        {"main-socket-closed-after-the-header",
         [] { theEndOfTheMainSocketDuringAnAcknowledgementAbortsTheWait(1); }},
        {"main-socket-closed-after-the-last-byte",
         [] { theEndOfTheMainSocketDuringAnAcknowledgementAbortsTheWait(2); }},
        {"notification-across-suspend", aNotificationPendingAcrossASuspendIsNotAFailure},
        {"suspend-with-notification", aSuspendWithANotificationPendingAnswersInTime},
        {"partial-acknowledgement", aPartialAcknowledgementThatNeverCompletesFails},
        {"partial-frame-completes", [] { aFrameThatCompletesInTimeIsAccepted(false); }},
        {"partial-frame-completes-armed", [] { aFrameThatCompletesInTimeIsAccepted(true); }},
        {"partial-request", aPartialRequestFromTheFrontEndFails},
        {"armed-budget-covers-a-frame", anArmedBudgetCoversAFrameThatBeginsInIt},
        {"teardown-unmap", aTeardownUnmapThatIsNeverAcknowledgedFails},
        {"stop", [] { aStopEndsTheWait(false); }},
        {"stop-with-a-reply-owed", [] { aStopEndsTheWait(true); }},
        {"budget-arithmetic", aBudgetIsSpentByThePollsItIsChargedFor},
        {"peer-end-behind-data", thePeerOfASocketIsSeenToEndBehindQueuedData},
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
    std::puts("vhost-user backend wait ok");
    return 0;
}
