// SPDX-License-Identifier: GPL-2.0-or-later

#include "forwarder.hpp"
#include "named_tests.hpp"
#include "vsock_test_guest.hpp"
#include "watchdog.hpp"

#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <vector>

#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>

/* A seam under the forwarder's accept(). This executable defines accept()
 * below, so the forwarder, linked in as a library, reaches it before the
 * system's, and it is the system's unless a test says otherwise: it fails with
 * an errno without touching the queue (`failure`), lets a client arrive before
 * each accept of the thread that watches (`arrivals`), leaves the queue to the
 * thread that watches (`holdOthers`), or gives the thread that watches one
 * descriptor at a time (`oneSlot`). */
struct AcceptSeam final {
    std::atomic<int> failure {};
    std::atomic_bool holdOthers {};
    std::atomic_bool oneSlot {};
    std::atomic<unsigned> arrivals {};
    std::atomic<unsigned> watchedAccepts {};
    std::atomic<std::uint16_t> port {};
    std::mutex mutex;
    std::vector<int> clients;
};

AcceptSeam acceptSeam;
thread_local bool watchingAccepts = false;
thread_local int lastGivenDescriptor = -1;

void arriveAtTheSeam();

namespace {

using namespace uml::vsock;
using namespace uml::vsock::testing;

std::optional<ForwardSpecification> parse(std::string_view text)
{
    std::string error;
    auto result = parseForward(text, error);
    require(result.has_value() == error.empty());
    return result;
}

/* The guest's endpoint, borrowed for a forwarder that must not own it. */
std::shared_ptr<Endpoint> borrowed(Guest& guest)
{
    return std::shared_ptr<Endpoint>(&guest.endpoint(), [](Endpoint*) {});
}

ForwardSpecification forward(std::uint16_t hostPort, std::uint32_t guestPort)
{
    return {
        .hostAddress = "127.0.0.1",
        .hostPort = hostPort,
        .guestPort = guestPort,
    };
}

void parsesTheForwardGrammar()
{
    const auto plain = parse("5555-5000");
    require(plain.has_value());
    require(plain->hostAddress == "127.0.0.1" && plain->hostPort == 5555 &&
        plain->guestPort == 5000);
    const auto chosen = parse("127.0.0.1:0-5555");
    require(chosen.has_value() && chosen->hostPort == 0 && chosen->guestPort == 5555);
    const auto other = parse("127.0.0.2:8000-1");
    require(other.has_value() && other->hostAddress == "127.0.0.2");
    const auto high = parse("65535-4294967294");
    require(high.has_value() && high->guestPort == 4294967294U);
}

void refusesWhatIsNotLoopback()
{
    for (const char* text : {
             "0.0.0.0:1-2", "192.0.2.1:1-2", "203.0.113.9:1-2", "localhost:1-2",
             "[::1]:1-2", "::1:1-2", "8.8.8.8:0-5555"}) {
        std::string error;
        require(!parseForward(text, error).has_value(), text);
        require(error.find("loopback") != std::string::npos, text);
    }
}

void refusesMalformedForwards()
{
    for (const char* text : {
             "", "abc", "5555", "5555-", "-5555", "70000-1", "1-0", "1-4294967295",
             "1-4294967296", "x-1", "1-x", "1:2-3-4", "127.0.0.1:-5", "-1-5"}) {
        std::string error;
        require(!parseForward(text, error).has_value(), text);
        require(!error.empty());
    }
}

sockaddr_in loopback(std::uint16_t port)
{
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_len = sizeof(address);
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

void connectSocket(int descriptor, std::uint16_t port)
{
    const auto address = loopback(port);
    require(::connect(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
}

int connectLoopback(std::uint16_t port)
{
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    require(descriptor >= 0);
    connectSocket(descriptor, port);
    return descriptor;
}

bool readable(int descriptor, int milliseconds)
{
    pollfd waiting {.fd = descriptor, .events = POLLIN, .revents = 0};
    return ::poll(&waiting, 1, milliseconds) > 0;
}

/* Whether the forwarder ended the client's stream within `milliseconds`. */
bool endsWithin(int descriptor, int milliseconds)
{
    if (!readable(descriptor, milliseconds))
        return false;
    char byte = 0;
    return ::recv(descriptor, &byte, 1, 0) <= 0;
}

/* The next packet the device sends the guest whose header `wanted` accepts.
 * The others, such as the requests of connections nobody answers, are dropped. */
template<typename Wanted>
Packet expectWhere(Guest& guest, Wanted wanted)
{
    for (;;) {
        auto packet = guest.expect();
        if (wanted(packet.header))
            return packet;
    }
}

/* A connection the guest accepts, one request and its reply over it, and the
 * client's close, which the guest hears as a shutdown. */
void exchangeWith(Guest& guest, std::uint16_t hostPort, std::uint32_t guestPort)
{
    const int client = connectLoopback(hostPort);
    const auto request = expectWhere(guest, [&](const protocol::Header& header) {
        return header.op == protocol::opRequest && header.destinationPort == guestPort;
    });
    const auto connection = [&](const protocol::Header& header) {
        return header.sourcePort == request.header.sourcePort;
    };
    guest.transmit(guestHeader(
        protocol::opResponse, guestPort, request.header.sourcePort));
    const char hello[] = "hello guest";
    require(::send(client, hello, sizeof(hello), 0) == static_cast<ssize_t>(sizeof(hello)));
    const auto data = expectWhere(guest, [&](const protocol::Header& header) {
        return connection(header) && header.op == protocol::opReadWrite;
    });
    require(data.payload.size() == sizeof(hello));
    const char reply[] = "hello host";
    guest.transmit(
        guestHeader(
            protocol::opReadWrite, guestPort, request.header.sourcePort,
            sizeof(reply)),
        std::span(reinterpret_cast<const std::uint8_t*>(reply), sizeof(reply)));
    char received[64] {};
    require(readable(client, 5000));
    require(::recv(client, received, sizeof(received), 0) == static_cast<ssize_t>(sizeof(reply)));
    require(std::strcmp(received, reply) == 0);
    ::close(client);
    (void)expectWhere(guest, [&](const protocol::Header& header) {
        return connection(header) && header.op == protocol::opShutdown;
    });
}

/* Waits until the connection `client` made has been completed by the
 * listener's host: one byte sent over it is acknowledged, which only a
 * connection in the accept queue does, or the forwarder has already ended the
 * stream. */
void awaitCompleted(int client)
{
    const char byte = 'x';
    (void)::send(client, &byte, 1, 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        int unacknowledged = 1;
        socklen_t length = sizeof(unacknowledged);
        if (::getsockopt(client, SOL_SOCKET, SO_NWRITE, &unacknowledged, &length) != 0 ||
            unacknowledged == 0 || readable(client, 0))
            return;
        require(std::chrono::steady_clock::now() < deadline, "the connection was never completed");
    }
}

int connectCompleted(std::uint16_t port)
{
    const int client = connectLoopback(port);
    awaitCompleted(client);
    return client;
}

void forwardsAConnectionToTheGuestPort()
{
    uml::testing::Watchdog watchdog("forwarder", std::chrono::seconds(60));
    Guest guest;
    Forwarder forwarder(borrowed(guest), {forward(0, 5555)});
    require(forwarder.port(0) != 0);
    const int client = connectLoopback(forwarder.port(0));
    const auto request = guest.expect();
    require(request.header.op == protocol::opRequest);
    require(request.header.destinationPort == 5555);
    guest.transmit(guestHeader(
        protocol::opResponse, 5555, request.header.sourcePort));
    const char hello[] = "hello guest";
    require(::send(client, hello, sizeof(hello), 0) == static_cast<ssize_t>(sizeof(hello)));
    const auto data = guest.expect();
    require(data.header.op == protocol::opReadWrite);
    require(data.payload.size() == sizeof(hello));
    const char reply[] = "hello host";
    guest.transmit(
        guestHeader(
            protocol::opReadWrite, 5555, request.header.sourcePort, sizeof(reply)),
        std::span(reinterpret_cast<const std::uint8_t*>(reply), sizeof(reply)));
    char received[64] {};
    require(readable(client, 5000));
    require(::recv(client, received, sizeof(received), 0) == static_cast<ssize_t>(sizeof(reply)));
    require(std::strcmp(received, reply) == 0);
    /* The client closes, and the guest is told the stream ended. */
    ::close(client);
    const auto shutdown = guest.expectNotCredit();
    require(shutdown.header.op == protocol::opShutdown);
}

void closesAClientTheGuestRefuses()
{
    uml::testing::Watchdog watchdog("forwarder refusal", std::chrono::seconds(60));
    Guest guest;
    Forwarder forwarder(borrowed(guest), {forward(0, 7)});
    const int client = connectLoopback(forwarder.port(0));
    const auto request = guest.expect();
    guest.transmit(guestHeader(protocol::opReset, 7, request.header.sourcePort));
    guest.pump();
    require(readable(client, 5000));
    char byte = 0;
    require(::recv(client, &byte, 1, 0) <= 0);
    ::close(client);
}

void stopsPromptlyWhileAConnectionWaitsForTheGuest()
{
    uml::testing::Watchdog watchdog("forwarder stop", std::chrono::seconds(60));
    Guest guest;
    const auto started = std::chrono::steady_clock::now();
    int client = -1;
    {
        Forwarder forwarder(borrowed(guest), {forward(0, 9)});
        client = connectLoopback(forwarder.port(0));
        /* The guest never answers. */
        const auto request = guest.expect();
        require(request.header.op == protocol::opRequest);
    }
    require(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
    ::close(client);
}

void servesASecondGuestOnTheSamePortAfterDetach()
{
    uml::testing::Watchdog watchdog("forwarder reattach", std::chrono::seconds(60));
    Guest first;
    Forwarder forwarder(borrowed(first), {forward(0, 5555)});
    const auto port = forwarder.port(0);
    exchangeWith(first, port, 5555);
    forwarder.detach();
    Guest second;
    forwarder.attach(borrowed(second));
    require(forwarder.port(0) == port);
    exchangeWith(second, port, 5555);
}

void closesAConnectionThatArrivesWhileNoGuestIsAttached()
{
    uml::testing::Watchdog watchdog("forwarder gap", std::chrono::seconds(60));
    std::optional<Guest> first;
    first.emplace();
    Forwarder forwarder(borrowed(*first), {forward(0, 5555)});
    const auto port = forwarder.port(0);
    exchangeWith(*first, port, 5555);
    forwarder.detach();
    /* The listener stays bound, accepts, and closes the connection at once;
     * nothing reaches the guest that was detached. */
    int client = connectLoopback(port);
    require(endsWithin(client, 5000));
    ::close(client);
    require(first->quiet());
    /* The detached guest may go; nothing of it is kept. */
    first.reset();
    client = connectLoopback(port);
    require(endsWithin(client, 5000));
    ::close(client);
    Guest second;
    forwarder.attach(borrowed(second));
    require(forwarder.port(0) == port);
    exchangeWith(second, port, 5555);
}

void aConnectionFromTheGapNeverReachesTheNextGuest()
{
    uml::testing::Watchdog watchdog("forwarder gap client", std::chrono::seconds(60));
    Guest guest;
    /* Detached from the start. */
    Forwarder forwarder(nullptr, {forward(0, 5555)});
    const auto port = forwarder.port(0);
    /* A client that connected before a guest was attached belongs to the gap,
     * however late the forwarder gets to it: it is closed, and the guest
     * attached next never hears of it. The next guest is attached before the
     * client is waited for, so that the forwarder may not have looked at the
     * connection yet. */
    for (int round = 0; round < 300; ++round) {
        const int gap = connectCompleted(port);
        forwarder.attach(borrowed(guest));
        require(endsWithin(gap, 5000));
        ::close(gap);
        require(guest.quiet());
        forwarder.detach();
    }
    forwarder.attach(borrowed(guest));
    exchangeWith(guest, port, 5555);
}

void anAttachThatCannotTakeTheQueuedConnectionsFails()
{
    uml::testing::Watchdog watchdog("forwarder drain", std::chrono::seconds(60));
    Guest guest;
    Forwarder forwarder(nullptr, {forward(0, 5555)});
    const auto port = forwarder.port(0);
    std::array<int, 3> clients {};
    bool refused = false;
    {
        /* While accept() fails without taking anything, the clients' connections
         * stay queued, and so does the first byte each sends. */
        acceptSeam.failure.store(EMFILE);
        for (auto& client : clients)
            client = connectCompleted(port);
        try {
            forwarder.attach(borrowed(guest));
        } catch (const std::runtime_error&) {
            refused = true;
        }
        acceptSeam.failure.store(0);
    }
    /* An attach that could not take them must not offer them to the guest. */
    for (const int client : clients) {
        require(endsWithin(client, 5000));
        ::close(client);
    }
    require(refused);
    require(guest.quiet());
    /* Once accept() works, the same attach goes through. */
    forwarder.attach(borrowed(guest));
    exchangeWith(guest, port, 5555);
}

void anAttachClosesEachQueuedConnectionBeforeTakingTheNext()
{
    uml::testing::Watchdog watchdog("forwarder drain slots", std::chrono::seconds(60));
    Guest guest;
    Forwarder forwarder(nullptr, {forward(0, 5555)});
    const auto port = forwarder.port(0);
    std::array<int, 3> clients {};
    /* The queue is left to the attach, which is given one descriptor at a
     * time: it can take three connections only by closing each before the
     * next. */
    acceptSeam.holdOthers.store(true);
    for (auto& client : clients)
        client = connectCompleted(port);
    acceptSeam.oneSlot.store(true);
    watchingAccepts = true;
    bool refused = false;
    try {
        forwarder.attach(borrowed(guest));
    } catch (const std::runtime_error&) {
        refused = true;
    }
    watchingAccepts = false;
    acceptSeam.oneSlot.store(false);
    acceptSeam.holdOthers.store(false);
    require(!refused, "the attach kept a descriptor the next connection needed");
    for (const int client : clients) {
        require(endsWithin(client, 5000));
        ::close(client);
    }
    require(guest.quiet());
    exchangeWith(guest, port, 5555);
}

void anAttachThatCannotAccountForTheQueueFails()
{
    uml::testing::Watchdog watchdog("forwarder drain interrupted", std::chrono::seconds(60));
    Guest guest;
    Forwarder forwarder(nullptr, {forward(0, 5555)});
    const auto port = forwarder.port(0);
    bool refused = false;
    int client = -1;
    {
        /* Every accept is interrupted and takes nothing, so the attach neither
         * takes the queued connection nor sees the queue end. */
        acceptSeam.failure.store(EINTR);
        client = connectCompleted(port);
        try {
            forwarder.attach(borrowed(guest));
        } catch (const std::runtime_error&) {
            refused = true;
        }
        acceptSeam.failure.store(0);
    }
    require(refused, "the attach went through without seeing the queue's end");
    require(endsWithin(client, 5000));
    ::close(client);
    require(guest.quiet());
    forwarder.attach(borrowed(guest));
    exchangeWith(guest, port, 5555);
}

void anAttachUnderSteadyArrivalsDoesBoundedWork()
{
    uml::testing::Watchdog watchdog("forwarder attach under arrivals", std::chrono::seconds(60));
    Guest guest;
    Forwarder forwarder(nullptr, {forward(0, 5555)});
    const auto port = forwarder.port(0);
    /* A client is queued before every accept the attach makes, 500 times over:
     * the queue is never empty for as long as that lasts. */
    acceptSeam.port.store(port);
    acceptSeam.watchedAccepts.store(0);
    acceptSeam.arrivals.store(500);
    watchingAccepts = true;
    forwarder.attach(borrowed(guest));
    watchingAccepts = false;
    const auto accepts = acceptSeam.watchedAccepts.load();
    acceptSeam.arrivals.store(0);
    /* The attach takes what was queued when it began, a few times the backlog
     * at most, and does not follow the arrivals. */
    require(accepts > 0, "the seam saw no accept of the attach");
    require(accepts <= 64, "the attach took connections for as long as they arrived");
    /* A client that arrives after the attach is served. */
    exchangeWith(guest, port, 5555);
    std::lock_guard lock(acceptSeam.mutex);
    for (const int client : acceptSeam.clients)
        ::close(client);
    acceptSeam.clients.clear();
}

void detachEndsAnOpeningTheGuestNeverAnswers()
{
    uml::testing::Watchdog watchdog("forwarder detach", std::chrono::seconds(60));
    Guest guest;
    Forwarder forwarder(borrowed(guest), {forward(0, 9)});
    const int client = connectLoopback(forwarder.port(0));
    /* The guest never answers. */
    const auto request = guest.expect();
    require(request.header.op == protocol::opRequest);
    const auto started = std::chrono::steady_clock::now();
    forwarder.detach();
    require(std::chrono::steady_clock::now() - started < Forwarder::acceptTimeout / 2);
    /* The device gives the open up, and the client reads its end. */
    guest.pump();
    require(endsWithin(client, 5000));
    require(guest.expectNotCredit().header.op == protocol::opReset);
    ::close(client);
}

void refusesAPortInUse()
{
    Guest guest;
    Forwarder first(borrowed(guest), {forward(0, 1)});
    bool refused = false;
    try {
        Forwarder second(borrowed(guest), {forward(first.port(0), 2)});
    } catch (const std::runtime_error&) {
        refused = true;
    }
    require(refused);
}

} // namespace

/* A client that has arrived and is queued, as the seam's port says. */
void arriveAtTheSeam()
{
    const int client = connectLoopback(acceptSeam.port.load());
    awaitCompleted(client);
    std::lock_guard lock(acceptSeam.mutex);
    acceptSeam.clients.push_back(client);
}

extern "C" int accept(int descriptor, sockaddr* address, socklen_t* length)
{
    static const auto system = reinterpret_cast<int (*)(int, sockaddr*, socklen_t*)>(
        ::dlsym(RTLD_NEXT, "accept"));
    if (const int failure = acceptSeam.failure.load(); failure != 0) {
        errno = failure;
        return -1;
    }
    if (!watchingAccepts) {
        if (acceptSeam.holdOthers.load()) {
            errno = EAGAIN;
            return -1;
        }
        return system(descriptor, address, length);
    }
    acceptSeam.watchedAccepts.fetch_add(1);
    /* One descriptor slot: the next is refused while the last one given is
     * still open. */
    if (acceptSeam.oneSlot.load() && lastGivenDescriptor >= 0 &&
        ::fcntl(lastGivenDescriptor, F_GETFD) >= 0) {
        errno = EMFILE;
        return -1;
    }
    unsigned left = acceptSeam.arrivals.load();
    while (left != 0 && !acceptSeam.arrivals.compare_exchange_weak(left, left - 1)) {
    }
    if (left != 0)
        arriveAtTheSeam();
    const int accepted = system(descriptor, address, length);
    if (accepted >= 0)
        lastGivenDescriptor = accepted;
    return accepted;
}

int main()
{
    static constexpr uml::testing::NamedTest tests[] = {
        {"parses-the-forward-grammar", parsesTheForwardGrammar},
        {"refuses-what-is-not-loopback", refusesWhatIsNotLoopback},
        {"refuses-malformed-forwards", refusesMalformedForwards},
        {"forwards-a-connection-to-the-guest-port", forwardsAConnectionToTheGuestPort},
        {"closes-a-client-the-guest-refuses", closesAClientTheGuestRefuses},
        {"stops-promptly-while-a-connection-waits-for-the-guest",
         stopsPromptlyWhileAConnectionWaitsForTheGuest},
        {"serves-a-second-guest-on-the-same-port-after-detach",
         servesASecondGuestOnTheSamePortAfterDetach},
        {"closes-a-connection-that-arrives-while-no-guest-is-attached",
         closesAConnectionThatArrivesWhileNoGuestIsAttached},
        {"a-connection-from-the-gap-never-reaches-the-next-guest",
         aConnectionFromTheGapNeverReachesTheNextGuest},
        {"an-attach-that-cannot-take-the-queued-connections-fails",
         anAttachThatCannotTakeTheQueuedConnectionsFails},
        {"an-attach-closes-each-queued-connection-before-taking-the-next",
         anAttachClosesEachQueuedConnectionBeforeTakingTheNext},
        {"an-attach-that-cannot-account-for-the-queue-fails",
         anAttachThatCannotAccountForTheQueueFails},
        {"an-attach-under-steady-arrivals-does-bounded-work",
         anAttachUnderSteadyArrivalsDoesBoundedWork},
        {"detach-ends-an-opening-the-guest-never-answers",
         detachEndsAnOpeningTheGuestNeverAnswers},
        {"refuses-a-port-in-use", refusesAPortInUse},
    };
    return uml::testing::runNamedTests(tests);
}
