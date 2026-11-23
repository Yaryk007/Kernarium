// SPDX-License-Identifier: GPL-2.0-or-later
// A device that keeps its deferred requests across a guest's suspend, behind
// the real transport. The guest suspends by disabling its rings one after
// another and resumes by enabling them in the same order, sending nothing
// else but kicks: none of it may reset such a device, what the device
// completes or raises meanwhile reaches the guest at the ring's enable, and
// the messages that change what the requests refer to still retire them.
// A device that also has a shared-memory region is held to the same rules: the
// GET_FEATURES that would reset it is answered after a map or unmap it waits
// for, never inside the wait, and the messages that end the guest's driver
// still reset it and unmap what it mapped.
// The front end is the GPU tests', so their support header and protocol
// header are on the path.
#include "held_request_harness.hpp"

#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace uml::virtio::held_request_harness;

/* A request the device completes while the guest is suspended, and an event
 * it raises, are written to the rings at each ring's enable, in whichever
 * order the rings are enabled, and the suspend resets nothing. The request
 * completions keep the order the device finished them in. */
void completionsAndEventsWaitForTheEnable(bool enableEventsFirst, bool sharedMemory)
{
    withSuspendedGuest({true, false, sharedMemory}, 2, [&](const HeldSession& session) {
        auto& device = session.device;
        auto& requests = session.requests;
        auto& events = session.events;
        const int socket = session.socket;
        const auto resets = device.resets();
        const auto taken = device.taken();
        const auto drained = device.drained();
        device.finish(2);
        device.finish(1);
        device.raise(0x1234);
        awaitCondition([&] {
            return device.taken() == taken + 2 && device.drained() > drained;
        });
        // The server has handled the wake once a message after it is answered.
        (void)readConfig(socket, 0, 1);
        require(requests.usedIndex() == 0 && events.usedIndex() == 0,
                "a used ring was written while its ring was disabled");
        require(!requests.called() && !events.called(),
                "the guest was called while its rings were disabled");

        const auto resumeRequests = [&] {
            requests.enable(socket, true);
            require(requests.usedIndex() == 2, "the held completions were not written");
            // Request 2 was posted second (slot 1, head 2) and finished first.
            require(requests.used(0).id == 2 && requests.used(0).length == 2);
            require(requests.used(1).id == 0 && requests.used(1).length == 2);
            require(requests.responseAt(1, 2) == responseTo(2));
            require(requests.responseAt(0, 2) == responseTo(1));
            requests.awaitCall();
            require(!requests.called(), "the guest was called more than once");
        };
        const auto resumeEvents = [&] {
            events.enable(socket, true);
            require(events.usedIndex() == 1, "the event did not reach the buffer");
            require(events.used(0).id == 0 && events.used(0).length == sizeof(std::uint64_t));
            require(events.eventAt(0) == 0x1234);
            events.awaitCall();
            require(!events.called(), "the guest was called more than once");
        };
        if (enableEventsFirst) {
            resumeEvents();
            require(requests.usedIndex() == 0, "the request ring was written before its enable");
            resumeRequests();
        } else {
            resumeRequests();
            require(events.usedIndex() == 0, "the event ring was written before its enable");
            resumeEvents();
        }
        require(device.resets() == resets, "the suspend and resume reset the device");
        require(device.held() == 0);
        if (sharedMemory) {
            require(device.mapped() == 2 && device.unmapsCompleted() == 0,
                    "the suspend and resume unmapped what the device mapped");
            require(session.backend->count(protocol::VhostSlaveRequest::sharedMemoryUnmap) == 0);
        }
    });
}

/* A completion that arrives while only one of the rings is disabled is
 * written at once to a ring that is enabled. */
void completionOnAnEnabledRingIsWrittenAtOnce()
{
    withSuspendedGuest({true, false}, 1, [&](const HeldSession& session) {
        auto& device = session.device;
        auto& requests = session.requests;
        auto& events = session.events;
        const int socket = session.socket;
        requests.enable(socket, true);
        const auto resets = device.resets();
        device.finish(1);
        awaitCondition([&] { return requests.usedIndex() == 1; });
        requests.awaitCall();
        require(requests.responseAt(0, 2) == responseTo(1));
        require(events.usedIndex() == 0);
        require(device.resets() == resets);
    });
}

/* A kick across the suspend, with and without the acknowledgement the front
 * end may ask for. A kick for a ring whose enable has not arrived is accepted
 * and does nothing; a kick for an enabled ring runs it while a later ring is
 * still disabled; neither resets the device, and the request a disabled
 * ring's kick announced runs at the ring's enable. */
void kicksAcrossASuspend(bool needReply, bool answersKicksFirst)
{
    auto device = std::make_unique<HoldingDevice>(
        HoldingDevice::Options {true, answersKicksFirst});
    auto* recorder = device.get();
    VhostUserServer server {std::move(device)};
    SplitRing ring;
    TestQueue requests {ring, requestQueue, requestLayout};
    TestQueue events {ring, eventQueue, eventLayout};

    withServerConnection(server, [&](int socket) {
        auto backend = openBackendChannel(socket, negotiatedInband);
        configureMemory(socket, ring.descriptor());
        requests.start(socket, true, false);
        events.start(socket, true, false);
        requests.postRequest(1);
        events.postEventBuffer();
        events.kick(socket, needReply);
        requests.kick(socket, needReply);
        require(recorder->runs(requestQueue) == 1 && recorder->runs(eventQueue) == 1);
        require(recorder->held() == 1);
        const auto resets = recorder->resets();

        // The guest suspends: its rings are disabled, in order.
        requests.enable(socket, false);
        events.enable(socket, false);

        // The guest resumes: its flag clears and it queues and kicks before
        // the host has handled its enable.
        requests.postRequest(2);
        requests.kick(socket, needReply);
        events.kick(socket, needReply);
        require(recorder->runs(requestQueue) == 1 && recorder->runs(eventQueue) == 1,
                "a kick for a disabled ring ran the device");
        require(recorder->held() == 1 && recorder->resets() == resets);
        require(requests.usedIndex() == 0, "a disabled ring's request was consumed and dropped");

        // The request ring is enabled, and runs the request kicked before.
        requests.enable(socket, true);
        require(recorder->runs(requestQueue) == 2 && recorder->held() == 2);

        // A kick for the enabled request ring, with the event ring disabled.
        requests.postRequest(3);
        requests.kick(socket, needReply);
        require(recorder->runs(requestQueue) == 3 && recorder->held() == 3,
                "a kick for an enabled ring was not run beside a disabled one");
        events.kick(socket, needReply);
        require(recorder->runs(eventQueue) == 1);
        require(recorder->resets() == resets, "a kick reset the device");

        // The event ring is enabled and takes the event raised meanwhile.
        const auto drained = recorder->drained();
        recorder->raise(0x77);
        awaitCondition([&] { return recorder->drained() > drained; });
        (void)readConfig(socket, 0, 1);
        require(events.usedIndex() == 0);
        events.enable(socket, true);
        require(events.usedIndex() == 1 && events.eventAt(0) == 0x77);
        events.awaitCall();

        // Every request is answered, each to its own buffer.
        recorder->finish(3);
        recorder->finish(1);
        recorder->finish(2);
        awaitCondition([&] { return requests.usedIndex() == 3; });
        require(requests.used(0).id == 4 && requests.used(1).id == 0 && requests.used(2).id == 2);
        require(recorder->resets() == resets && recorder->held() == 0);
    });
}

/* The messages that change what a held request refers to, or end the
 * driver, reset the device and drop its held requests and completions;
 * the connection goes on. */
void stateMessagesRetireHeldRequests(bool sharedMemory)
{
    struct Message {
        const char* name;
        std::function<void(const HeldSession&)> send;
    };
    const std::vector<Message> messages {
        {"GET_FEATURES", [](const HeldSession& session) { (void)getFeatures(session.socket); }},
        {"SET_FEATURES",
         [](const HeldSession& session) {
             const std::uint64_t features = protocol::vhostFeatureProtocol;
             sendRequestAndReadAcknowledgement(
                 session.socket, protocol::VhostRequest::setFeatures, bytesOf(features));
         }},
        {"SET_PROTOCOL_FEATURES",
         [](const HeldSession& session) {
             const std::uint64_t features = negotiated;
             sendRequestAndReadAcknowledgement(
                 session.socket, protocol::VhostRequest::setProtocolFeatures, bytesOf(features));
         }},
        {"SET_MEM_TABLE",
         [](const HeldSession& session) {
             configureMemory(session.socket, session.ring.descriptor());
         }},
        {"SET_VRING_NUM",
         [](const HeldSession& session) {
             const protocol::VringState size {.index = requestQueue, .value = queueDepth};
             sendRequestAndReadAcknowledgement(
                 session.socket, protocol::VhostRequest::setVringNum, bytesOf(size));
         }},
        {"SET_VRING_ADDR",
         [](const HeldSession& session) {
             const protocol::VringAddress address {
                 .index = requestQueue,
                 .flags = 0,
                 .descriptor = SplitRing::userBase + requestLayout.descriptors,
                 .used = SplitRing::userBase + requestLayout.used,
                 .available = SplitRing::userBase + requestLayout.available,
                 .log = 0,
             };
             sendRequestAndReadAcknowledgement(
                 session.socket, protocol::VhostRequest::setVringAddr, bytesOf(address));
         }},
        {"SET_VRING_BASE",
         [](const HeldSession& session) {
             const protocol::VringState base {.index = requestQueue, .value = 0};
             sendRequestAndReadAcknowledgement(
                 session.socket, protocol::VhostRequest::setVringBase, bytesOf(base));
         }},
        {"GET_VRING_BASE",
         [](const HeldSession& session) {
             const protocol::VringState base {.index = requestQueue, .value = 0};
             sendRequest(session.socket, protocol::VhostRequest::getVringBase, bytesOf(base), {}, false);
             (void)readValueReply<protocol::VringState>(
                 session.socket, protocol::VhostRequest::getVringBase);
         }},
        {"RESET_OWNER",
         [](const HeldSession& session) {
             sendRequestAndReadAcknowledgement(
                 session.socket, protocol::VhostRequest::resetOwner, {});
         }},
    };
    for (const auto& message : messages) {
        withSuspendedGuest({true, false, sharedMemory}, 2, [&](const HeldSession& session) {
            const auto& device = session.device;
            // A completion held for the enable goes with the requests.
            const auto taken = device.taken();
            session.device.finish(1);
            awaitCondition([&] { return device.taken() == taken + 1; });
            (void)readConfig(session.socket, 0, 1);
            const auto resets = device.resets();
            message.send(session);
            const std::string name = message.name;
            require(device.resets() == resets + 1, name + " did not reset the device");
            require(device.held() == 0, name + " left a request held");
            if (sharedMemory) {
                require(device.mapped() == 0 && device.unmapsCompleted() == 2,
                        name + " did not unmap what the device had mapped");
            }
            (void)getFeatures(session.socket);
        });
    }
}

/* The messages a suspend can race with, or that touch neither the requests
 * nor the rings' layout, leave the device alone, and the held request is
 * still answered at the enable. */
void otherMessagesLeaveHeldRequestsAlone(bool sharedMemory)
{
    withSuspendedGuest({true, false, sharedMemory}, 1, [&](const HeldSession& session) {
        auto& device = session.device;
        auto& requests = session.requests;
        auto& events = session.events;
        const int socket = session.socket;
        const auto resets = device.resets();
        const auto unchanged = [&](const char* name) {
            require(device.resets() == resets, std::string {name} + " reset the device");
            require(device.held() == 1, std::string {name} + " dropped a request");
            require(device.mapped() == (sharedMemory ? 1U : 0U),
                    std::string {name} + " unmapped what the device had mapped");
        };

        (void)readConfig(socket, 0, 4);
        unchanged("GET_CONFIG");
        const std::array<std::uint8_t, 1> contents {0};
        const protocol::ConfigHeader write {.offset = 0, .size = 1, .flags = 0};
        sendRequestAndReadAcknowledgement(
            socket, protocol::VhostRequest::setConfig, configPayload(write, contents));
        unchanged("SET_CONFIG");
        sendRequest(socket, protocol::VhostRequest::getProtocolFeatures, {}, {}, false);
        (void)readValueReply<std::uint64_t>(socket, protocol::VhostRequest::getProtocolFeatures);
        unchanged("GET_PROTOCOL_FEATURES");
        sendRequest(socket, protocol::VhostRequest::getQueueNum, {}, {}, false);
        (void)readValueReply<std::uint64_t>(socket, protocol::VhostRequest::getQueueNum);
        unchanged("GET_QUEUE_NUM");
        sendRequestAndReadAcknowledgement(socket, protocol::VhostRequest::setOwner, {});
        unchanged("SET_OWNER");
        requests.attachChannels(socket, true);
        events.attachChannels(socket, true);
        unchanged("SET_VRING_CALL and SET_VRING_KICK");

        requests.enable(socket, true);
        device.finish(1);
        awaitCondition([&] { return requests.usedIndex() == 1; });
        require(requests.responseAt(0, 2) == responseTo(1));
        require(device.resets() == resets);
    });
}

/* A driver's teardown disables its rings in reverse order, asks for the
 * features, and gives its rings up. The request is still held through the
 * disables; the features request ends it, and the calls and kicks the driver
 * then withdraws are accepted. */
void teardownRetiresHeldRequestsAtTheFeaturesRequest(bool sharedMemory)
{
    withSuspendedGuest({true, false, sharedMemory}, 1, [&](const HeldSession& session) {
        auto& device = session.device;
        auto& requests = session.requests;
        auto& events = session.events;
        const int socket = session.socket;
        // The suspend already disabled both rings; a teardown disables them
        // again, in the other order.
        events.enable(socket, false);
        requests.enable(socket, false);
        require(device.held() == 1);
        const auto resets = device.resets();
        (void)getFeatures(socket);
        require(device.resets() == resets + 1 && device.held() == 0,
                "the features request after the disables did not retire the request");
        require(device.mapped() == 0 && device.unmapsCompleted() == (sharedMemory ? 1U : 0U),
                "the features request after the disables did not unmap what the device had mapped");
        if (sharedMemory) {
            require(!session.regions->contains(0, 0, 1),
                    "the features request after the disables left the host index a range");
        }
        for (const std::uint64_t queue : {std::uint64_t {eventQueue}, std::uint64_t {requestQueue}}) {
            const std::uint64_t withdrawn = queue | protocol::vhostVringNoDescriptor;
            sendRequestAndReadAcknowledgement(
                socket, protocol::VhostRequest::setVringCall, bytesOf(withdrawn));
            sendRequestAndReadAcknowledgement(
                socket, protocol::VhostRequest::setVringKick, bytesOf(withdrawn));
        }
        require(device.resets() == resets + 1);
        (void)readConfig(socket, 0, 1);
    });
}

/* A reset drops the completion a ring was holding for its enable: nothing is
 * written when the ring is enabled afterwards. */
void resetDropsAHeldCompletion(bool sharedMemory)
{
    withSuspendedGuest({true, false, sharedMemory}, 1, [&](const HeldSession& session) {
        auto& device = session.device;
        auto& requests = session.requests;
        auto& events = session.events;
        const int socket = session.socket;
        const auto taken = device.taken();
        device.finish(1);
        awaitCondition([&] { return device.taken() == taken + 1; });
        (void)readConfig(socket, 0, 1);
        (void)getFeatures(socket);
        requests.enable(socket, true);
        events.enable(socket, true);
        (void)readConfig(socket, 0, 1);
        require(requests.usedIndex() == 0, "a completion of a reset device was written");
        require(!requests.called(), "the guest was called for a completion that was dropped");
    });
}

/* A device that does not ask to keep its requests is reset by the first
 * disable of a ring while one is held, as before: the request and its answer
 * are gone, and a request posted to the disabled ring is consumed and dropped
 * rather than kept. */
void deviceThatDoesNotOptInIsResetAtTheDisable()
{
    auto device = std::make_unique<HoldingDevice>(HoldingDevice::Options {false, false});
    auto* recorder = device.get();
    VhostUserServer server {std::move(device)};
    SplitRing ring;
    TestQueue requests {ring, requestQueue, requestLayout};
    TestQueue events {ring, eventQueue, eventLayout};

    withServerConnection(server, [&](int socket) {
        negotiateProtocolFeatures(socket, negotiated);
        configureMemory(socket, ring.descriptor());
        requests.start(socket, true, true);
        events.start(socket, true, true);
        requests.postRequest(1);
        requests.writeKickPipe();
        awaitCondition([&] { return recorder->runs(requestQueue) == 1; });
        require(recorder->held() == 1);
        const auto resets = recorder->resets();

        const auto said = captureStderr([&] { requests.enable(socket, false); });
        require(recorder->resets() == resets + 1, "the disable did not reset a device that holds a request");
        require(recorder->held() == 0);
        require(said == "Vhost-user: device reset at ring disable: device=holding ring=0 deferred=1\n",
                "the reset at the disable was not reported as expected: " + said);

        requests.postRequest(2);
        requests.writeKickPipe();
        awaitCondition([&] { return requests.usedIndex() == 1; });
        require(requests.used(0).id == 2 && requests.used(0).length == 0,
                "a request on a disabled ring was kept");
        require(recorder->runs(requestQueue) == 1);

        // With nothing held, a disable resets nothing further, and says nothing.
        const auto quiet = captureStderr([&] { events.enable(socket, false); });
        require(recorder->resets() == resets + 1);
        require(quiet.empty(), "a disable that reset nothing was reported: " + quiet);
    });
}

/* A guest that suspends over and over with a request held, to a device that
 * does not keep it, is told of every one of the first 256 resets, one line
 * each, and then only of each one whose count doubles: the 512th, and not the
 * 257th to the 511th or the 513th. */
void theResetAtADisableIsReportedLessOften()
{
    auto device = std::make_unique<HoldingDevice>(HoldingDevice::Options {false, false});
    auto* recorder = device.get();
    VhostUserServer server {std::move(device)};
    constexpr unsigned suspends = 513;
    constexpr auto line = "Vhost-user: device reset at ring disable: device=holding ring=0 deferred=1\n";
    unsigned lines = 0;
    for (unsigned suspend = 1; suspend <= suspends; ++suspend) {
        SplitRing ring;
        TestQueue requests {ring, requestQueue, requestLayout};
        TestQueue events {ring, eventQueue, eventLayout};
        withServerConnection(server, [&](int socket) {
            negotiateProtocolFeatures(socket, negotiated);
            configureMemory(socket, ring.descriptor());
            requests.start(socket, true, true);
            events.start(socket, true, true);
            requests.postRequest(1);
            requests.writeKickPipe();
            awaitCondition([&] { return recorder->held() == 1; });
            const auto said = captureStderr([&] { requests.enable(socket, false); });
            const bool reported = suspend <= 256 || (suspend & (suspend - 1U)) == 0;
            const auto count = countLines(said, line);
            require(count == (reported ? 1U : 0U) && said.size() == count * std::strlen(line),
                    "reset " + std::to_string(suspend) + " was reported " +
                        std::to_string(count) + " times: " + said);
            lines += count;
        });
    }
    require(lines == 257, "the resets were reported " + std::to_string(lines) + " times, not 257");
}

/* A teardown's GET_FEATURES that arrives while the server waits for the
 * acknowledgement of a map the device asked for is not answered inside the
 * wait: the reset it brings would send the unmaps of the device's ranges while
 * the map is still owed an answer, and clear the requests under the request
 * being run. It is answered once the map has been acknowledged and the
 * request that raised it has been taken, and then resets the device, whose
 * every range is unmapped. */
void getFeaturesInsideAMapWaitIsAnsweredAfterIt()
{
    withSuspendedGuest({true, false, true}, 1, [&](const HeldSession& session) {
        auto& device = session.device;
        auto& requests = session.requests;
        auto& backend = *session.backend;
        const int socket = session.socket;
        constexpr auto map = protocol::VhostSlaveRequest::sharedMemoryMap;
        constexpr auto unmap = protocol::VhostSlaveRequest::sharedMemoryUnmap;
        const auto resets = device.resets();
        require(backend.count(map) == 1);

        /* The guest resumes its request ring, which holds a request that maps
         * a range, while its event ring is still disabled. */
        requests.postRequest(2);
        backend.hold(true);
        requests.sendEnable(socket, true);
        awaitCondition([&] { return backend.count(map) == 2; });

        sendRequest(socket, protocol::VhostRequest::getFeatures, {}, {}, false);
        pollfd answered {.fd = socket, .events = POLLIN};
        require(::poll(&answered, 1, 100) == 0, "a request was answered inside the backend wait");
        require(!backend.unreadFrame(), "the device was reset inside the backend wait");
        require(device.resets() == resets && backend.count(unmap) == 0);

        backend.hold(false);
        readAcknowledgement(socket, protocol::VhostRequest::setVringEnable);
        (void)readValueReply<std::uint64_t>(socket, protocol::VhostRequest::getFeatures);
        require(device.resets() == resets + 1 && device.held() == 0,
                "the features request did not reset the device once the wait was over");
        require(device.mapped() == 0 && device.unmapsCompleted() == 2 && backend.count(unmap) == 2,
                "the reset did not unmap what the device had mapped");
        (void)readConfig(socket, 0, 1);
    });
}

/* A driver's teardown sends its ring disables without asking for a reply and
 * ends with the GET_FEATURES it waits for, so that reply tells it the disables
 * have taken effect. When they arrive while the server waits for the
 * acknowledgement of a map, they are queued behind it, and the GET_FEATURES
 * stays queued behind them: it is not answered in the wait, and the answer of
 * the request that raised the map, which the device completes at once, is
 * written to its ring before that reply, never after the driver may free the
 * ring. */
void getFeaturesFollowsDisablesQueuedInAMapWait()
{
    auto device = std::make_unique<HoldingDevice>(HoldingDevice::Options {true, false, true, true});
    auto* recorder = device.get();
    VhostUserServer server {std::move(device)};
    SplitRing ring;
    TestQueue requests {ring, requestQueue, requestLayout};
    TestQueue events {ring, eventQueue, eventLayout};
    std::optional<SocketPair> backendChannel;
    std::optional<BackendAnswerer> answerer;

    withServerConnection(server, [&](int socket) {
        constexpr auto map = protocol::VhostSlaveRequest::sharedMemoryMap;
        backendChannel = openBackendChannel(socket, negotiatedSharedMemory);
        answerer.emplace(backendChannel->descriptors[0].get());
        configureMemory(socket, ring.descriptor());
        requests.start(socket, true, true);
        events.start(socket, true, true);
        answerer->hold(true);
        requests.postRequest(1);
        requests.writeKickPipe();
        awaitCondition([&] { return answerer->count(map) == 1; });
        const auto resets = recorder->resets();

        events.sendEnable(socket, false, false);
        requests.sendEnable(socket, false, false);
        sendRequest(socket, protocol::VhostRequest::getFeatures, {}, {}, false);
        pollfd answered {.fd = socket, .events = POLLIN};
        require(::poll(&answered, 1, 100) == 0,
                "a GET_FEATURES overtook the ring disables queued before it");
        require(requests.usedIndex() == 0);

        answerer->hold(false);
        (void)readValueReply<std::uint64_t>(socket, protocol::VhostRequest::getFeatures);
        require(requests.usedIndex() == 1 && requests.responseAt(0, 2) == responseTo(1),
                "the request's answer was not written before the GET_FEATURES reply");
        require(recorder->resets() == resets, "the queued disables reset a device with nothing held");
        (void)readConfig(socket, 0, 1);
        require(requests.usedIndex() == 1, "an answer was written after the GET_FEATURES reply");
    });
}

/* A reset whose first unmap the front end refuses fails closed: the server
 * ends the backend channel, which makes the front end revoke every map, and
 * the host index drops the range it had not yet asked to unmap along with the
 * one it had, so no host mapping outlives the guest's. */
void failedUnmapDropsEveryRangeOfTheBackend()
{
    withSuspendedGuest({true, false, true}, 2, [&](const HeldSession& session) {
        auto& device = session.device;
        auto& backend = *session.backend;
        const auto& regions = *session.regions;
        const int socket = session.socket;
        constexpr auto unmap = protocol::VhostSlaveRequest::sharedMemoryUnmap;
        require(regions.contains(0, 0, 1) && regions.contains(0, HoldingDevice::rangeLength, 1),
                "the device's ranges were not published to the host");
        const auto resets = device.resets();

        backend.refuseUnmaps(1);
        (void)getFeatures(socket);
        require(device.resets() == resets + 1 && device.held() == 0 && device.mapped() == 0,
                "the features request did not reset the device whose unmap failed");
        require(backend.count(unmap) == 1, "the reset went on asking after a refused unmap");
        require(!regions.contains(0, 0, 1) && !regions.contains(0, HoldingDevice::rangeLength, 1),
                "a range stayed in the host index after the reset");
        (void)readConfig(socket, 0, 1);
    });
}

/* A guest that suspends and never comes back, whose connection ends instead,
 * leaves a device that keeps its requests reset, with its ranges unmapped, and
 * nothing written to a ring it did not enable again. */
void endOfTheConnectionResetsAKeepingDevice(bool sharedMemory)
{
    unsigned resetsAtSuspend = 0;
    withSuspendedGuest(
        {true, false, sharedMemory},
        2,
        [&](const HeldSession& session) {
            auto& device = session.device;
            resetsAtSuspend = device.resets();
            const auto taken = device.taken();
            device.finish(1);
            awaitCondition([&] { return device.taken() == taken + 1; });
            (void)readConfig(session.socket, 0, 1);
            require(device.resets() == resetsAtSuspend && device.held() == 1,
                    "the suspend reset the device");
        },
        [&](const HeldSession& session) {
            const auto& device = session.device;
            require(device.resets() == resetsAtSuspend + 1 && device.held() == 0,
                    "the end of the connection did not reset the device");
            require(session.requests.usedIndex() == 0,
                    "an answer was written to a ring the guest never enabled again");
            if (sharedMemory) {
                require(device.mapped() == 0 && device.unmapsCompleted() == 2 &&
                            session.backend->count(protocol::VhostSlaveRequest::sharedMemoryUnmap) == 2,
                        "the end of the connection did not unmap what the device had mapped");
            }
        });
}

} // namespace

/* With a case's name as its argument, runs that case alone. */
int main(int argc, char** argv)
{
    struct Case {
        std::string name;
        std::function<void()> run;
    };
    std::vector<Case> cases;
    /* Each rule holds for a device with a shared-memory region too, whose
     * resets also unmap what it mapped. */
    for (const bool sharedMemory : {false, true}) {
        const std::string suffix = sharedMemory ? "-shared-memory" : "";
        for (const bool eventsFirst : {false, true}) {
            cases.push_back({std::string {eventsFirst ? "completions-events-first" : "completions-requests-first"} + suffix,
                             [=] { completionsAndEventsWaitForTheEnable(eventsFirst, sharedMemory); }});
        }
        cases.push_back({"state-messages" + suffix, [=] { stateMessagesRetireHeldRequests(sharedMemory); }});
        cases.push_back({"other-messages" + suffix, [=] { otherMessagesLeaveHeldRequestsAlone(sharedMemory); }});
        cases.push_back({"teardown" + suffix, [=] { teardownRetiresHeldRequestsAtTheFeaturesRequest(sharedMemory); }});
        cases.push_back({"reset-drops-completion" + suffix, [=] { resetDropsAHeldCompletion(sharedMemory); }});
        cases.push_back({"connection-end" + suffix, [=] { endOfTheConnectionResetsAKeepingDevice(sharedMemory); }});
    }
    cases.push_back({"completion-on-enabled-ring", completionOnAnEnabledRingIsWrittenAtOnce});
    for (const bool needReply : {false, true}) {
        for (const bool answersFirst : {false, true}) {
            cases.push_back({needReply ? (answersFirst ? "kicks-reply-answered-first" : "kicks-reply")
                                       : (answersFirst ? "kicks-answered-first" : "kicks"),
                             [=] { kicksAcrossASuspend(needReply, answersFirst); }});
        }
    }
    cases.push_back({"no-opt-in", deviceThatDoesNotOptInIsResetAtTheDisable});
    cases.push_back({"reset-report-rate", theResetAtADisableIsReportedLessOften});
    cases.push_back({"features-in-a-map-wait", getFeaturesInsideAMapWaitIsAnsweredAfterIt});
    cases.push_back({"features-behind-queued-disables", getFeaturesFollowsDisablesQueuedInAMapWait});
    cases.push_back({"failed-unmap-drops-ranges", failedUnmapDropsEveryRangeOfTheBackend});
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
    std::puts("vhost-user held requests ok");
    return 0;
}
