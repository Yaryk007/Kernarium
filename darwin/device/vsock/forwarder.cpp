// SPDX-License-Identifier: GPL-2.0-or-later

#include "forwarder.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

namespace uml::vsock {
namespace {

constexpr std::uint32_t anyGuestPort = 0xFFFFFFFFU;

bool parseNumber(std::string_view text, std::uint64_t maximum, std::uint64_t& value)
{
    if (text.empty() || text.size() > 10)
        return false;
    std::uint64_t parsed = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc {} || end != text.data() + text.size() ||
        parsed > maximum)
        return false;
    value = parsed;
    return true;
}

/* The backlog of every listener. The kernel queues no more connections than
 * this on one, so a replacement that takes twice as many has taken all that
 * were queued when it began, however fast others arrive meanwhile. */
constexpr int listenBacklog = 16;
constexpr unsigned queuedLimit = 2U * listenBacklog;

/* What taking a connection from a listener found. */
enum class Taken : std::uint8_t {
    connection,
    /* Nothing is queued: the only answer that says so. */
    none,
    /* A connection reset in the queue, or a signal: look again. */
    again,
    failed,
};

/* Takes the next connection queued on `listener` into `accepted`. The
 * listener does not block. `error` is the errno of a failure. */
Taken takeQueued(int listener, uml::gpu::UniqueDescriptor& accepted, int& error)
{
    const int taken = ::accept(listener, nullptr, nullptr);
    error = errno;
    if (taken >= 0) {
        accepted.reset(taken);
        return Taken::connection;
    }
    if (error == EAGAIN || error == EWOULDBLOCK)
        return Taken::none;
    if (error == EINTR || error == ECONNABORTED)
        return Taken::again;
    return Taken::failed;
}

} // namespace

std::optional<ForwardSpecification> parseForward(
    std::string_view text,
    std::string& error)
{
    const auto fail = [&](std::string reason) -> std::optional<ForwardSpecification> {
        error = "invalid vsock forward '" + std::string(text) + "': " + reason;
        return std::nullopt;
    };
    const auto dash = text.rfind('-');
    if (dash == std::string_view::npos)
        return fail("expected [127.0.0.1:]HOSTPORT-GUESTPORT");
    auto host = text.substr(0, dash);
    const auto guest = text.substr(dash + 1);
    ForwardSpecification specification;
    if (const auto colon = host.find(':'); colon != std::string_view::npos) {
        specification.hostAddress = std::string(host.substr(0, colon));
        host = host.substr(colon + 1);
        in_addr address {};
        if (inet_pton(AF_INET, specification.hostAddress.c_str(), &address) != 1 ||
            (ntohl(address.s_addr) >> 24) != 127U)
            return fail("only a loopback address is accepted");
    }
    std::uint64_t hostPort = 0;
    std::uint64_t guestPort = 0;
    if (!parseNumber(host, 65535, hostPort))
        return fail("the host port must be 0 to 65535");
    if (!parseNumber(guest, anyGuestPort - 1U, guestPort) || guestPort == 0)
        return fail("the guest port must be a positive port number");
    specification.hostPort = static_cast<std::uint16_t>(hostPort);
    specification.guestPort = static_cast<std::uint32_t>(guestPort);
    return specification;
}

struct Forwarder::Listener final {
    ForwardSpecification specification;
    uml::gpu::UniqueDescriptor descriptor;
    std::uint16_t port {};
};

/* One accepted connection on its way to the guest. */
struct Forwarder::Worker final {
    /* The endpoint the connection was accepted for, set before `thread`
     * starts and never changed. */
    std::shared_ptr<Endpoint> endpoint;
    std::thread thread;
    std::atomic_bool done {};
    /* Guards `opening` and `cancelled`, which the worker and `end` share. */
    std::mutex mutex;
    std::shared_ptr<Opening> opening;
    bool cancelled {};
};

Forwarder::Forwarder(
    std::shared_ptr<Endpoint> endpoint,
    std::vector<ForwardSpecification> forwards)
    : endpoint_(std::move(endpoint))
    , wake_(std::make_unique<uml::platform::WakeSocket>("vsock forwarder wake"))
{
    for (auto& specification : forwards) {
        auto listener = std::make_unique<Listener>();
        listener->specification = std::move(specification);
        const auto describe = [&] {
            return listener->specification.hostAddress + ":" +
                std::to_string(listener->specification.hostPort);
        };
        listener->descriptor.reset(::socket(AF_INET, SOCK_STREAM, 0));
        if (listener->descriptor.get() < 0) {
            throw std::runtime_error(
                "cannot create the vsock forward socket: " +
                std::string(std::strerror(errno)));
        }
        const int enabled = 1;
        (void)::setsockopt(
            listener->descriptor.get(), SOL_SOCKET, SO_REUSEADDR, &enabled,
            sizeof(enabled));
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_len = sizeof(address);
        address.sin_port = htons(listener->specification.hostPort);
        (void)inet_pton(
            AF_INET, listener->specification.hostAddress.c_str(),
            &address.sin_addr);
        if (!uml::platform::configureDescriptor(
                listener->descriptor.get(),
                {.closeOnExec = true, .nonblocking = true}) ||
            ::bind(
                listener->descriptor.get(),
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0 ||
            ::listen(listener->descriptor.get(), listenBacklog) != 0) {
            throw std::runtime_error(
                "cannot listen on " + describe() + " for the vsock forward: " +
                std::strerror(errno));
        }
        socklen_t length = sizeof(address);
        if (::getsockname(
                listener->descriptor.get(),
                reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            throw std::runtime_error(
                "cannot read the port of the vsock forward on " + describe());
        }
        listener->port = ntohs(address.sin_port);
        listeners_.push_back(std::move(listener));
    }
    thread_ = std::thread([this] { run(); });
}

Forwarder::~Forwarder()
{
    stopping_.store(true, std::memory_order_release);
    (void)wake_->notify();
    if (thread_.joinable())
        thread_.join();
    /* Connections still waiting for the guest are given up. */
    std::list<std::unique_ptr<Worker>> workers;
    {
        std::lock_guard lock(mutex_);
        workers.swap(workers_);
    }
    end(workers);
}

std::uint16_t Forwarder::port(std::size_t index) const
{
    return listeners_.at(index)->port;
}

std::unique_lock<std::mutex> Forwarder::lockForTransition()
{
    /* The accept thread lets a transition that waits go first. */
    waiting_.fetch_add(1, std::memory_order_acq_rel);
    std::unique_lock lock(mutex_);
    waiting_.fetch_sub(1, std::memory_order_acq_rel);
    return lock;
}

void Forwarder::attach(std::shared_ptr<Endpoint> endpoint)
{
    std::shared_ptr<Endpoint> previous;
    std::list<std::unique_ptr<Worker>> workers;
    std::string failure;
    {
        auto lock = lockForTransition();
        previous = std::move(endpoint_);
        /* Every worker listed now was accepted for the endpoint just
         * replaced: a later accept takes the new one under this lock. */
        workers.swap(workers_);
        /* A connection already queued on a listener arrived before this
         * call, so it belongs to the guest replaced or to the gap between
         * guests, and the next guest is not offered it. The accept thread
         * takes a connection and chooses its endpoint under this lock, so
         * none is in hand between the two. An empty queue is told by
         * nothing but EAGAIN; any other answer leaves the forwarder
         * detached, whose accept thread closes what is left. */
        for (const auto& listener : listeners_) {
            uml::gpu::UniqueDescriptor queued;
            int error = 0;
            unsigned dequeued = 0;
            bool accounted = false;
            for (unsigned attempts = 0; attempts < queuedLimit && !accounted;
                 ++attempts) {
                /* A taken connection is closed before the next is asked for,
                 * so that it does not keep a descriptor the next one needs. */
                queued.reset();
                const auto result =
                    takeQueued(listener->descriptor.get(), queued, error);
                if (result == Taken::none) {
                    accounted = true;
                } else if (result == Taken::connection) {
                    /* As many connections as the kernel queues: whatever
                     * arrives after them arrived after the call began. */
                    accounted = ++dequeued == queuedLimit;
                } else if (result == Taken::failed) {
                    break;
                }
            }
            if (!accounted) {
                /* The queue was not seen to the end, or to the bound: an
                 * interrupted or reset attempt is no connection taken. */
                failure = "cannot account for the connections queued on " +
                    listener->specification.hostAddress + ":" +
                    std::to_string(listener->port) +
                    " before attaching the vsock forward: " +
                    std::strerror(error);
                break;
            }
        }
        if (failure.empty())
            endpoint_ = std::move(endpoint);
    }
    end(workers);
    if (!failure.empty())
        throw std::runtime_error(failure);
}

void Forwarder::detach() noexcept
{
    std::shared_ptr<Endpoint> previous;
    std::list<std::unique_ptr<Worker>> workers;
    {
        auto lock = lockForTransition();
        previous = std::move(endpoint_);
        workers.swap(workers_);
    }
    end(workers);
}

void Forwarder::end(std::list<std::unique_ptr<Worker>>& workers) noexcept
{
    for (auto& worker : workers) {
        {
            std::lock_guard lock(worker->mutex);
            worker->cancelled = true;
            if (worker->opening)
                worker->opening->cancel();
        }
        if (worker->thread.joinable())
            worker->thread.join();
    }
}

void Forwarder::run()
{
    std::vector<pollfd> descriptors(listeners_.size() + 1U);
    while (!stopping_.load(std::memory_order_acquire)) {
        for (std::size_t index = 0; index < listeners_.size(); ++index) {
            descriptors[index] = {
                .fd = listeners_[index]->descriptor.get(),
                .events = POLLIN,
                .revents = 0,
            };
        }
        descriptors.back() = {
            .fd = wake_->descriptor(),
            .events = POLLIN,
            .revents = 0,
        };
        if (::poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), 1000) < 0 &&
            errno != EINTR)
            break;
        reap();
        if (stopping_.load(std::memory_order_acquire))
            break;
        for (std::size_t index = 0; index < listeners_.size(); ++index) {
            if ((descriptors[index].revents & POLLIN) == 0)
                continue;
            while (acceptOne(index)) {
            }
        }
    }
}

/* Takes one connection from listener `index` and serves it, or closes it when
 * no guest is attached. A connection is taken from the listener, and the
 * endpoint it is served through chosen, under the lock a transition takes, so
 * that a connection taken before a transition is never handed to the endpoint
 * it installs. The worker is listed with its endpoint, and its thread started,
 * under the same lock, so that a transition finds it and has a thread to join.
 * The lock is held for one connection only, and not at all while a transition
 * waits or the forwarder ends. False when there is nothing more to do now. */
bool Forwarder::acceptOne(std::size_t index)
{
    std::lock_guard lock(mutex_);
    if (waiting_.load(std::memory_order_acquire) != 0 ||
        stopping_.load(std::memory_order_acquire))
        return false;
    uml::gpu::UniqueDescriptor accepted;
    int error = 0;
    switch (takeQueued(listeners_[index]->descriptor.get(), accepted, error)) {
    case Taken::connection:
        break;
    case Taken::again:
        return true;
    case Taken::none:
    case Taken::failed:
        return false;
    }
    /* No guest is attached: the connection is closed, quietly. */
    if (!endpoint_)
        return true;
    const auto guestPort = listeners_[index]->specification.guestPort;
    auto worker = std::make_unique<Worker>();
    auto* raw = worker.get();
    worker->endpoint = endpoint_;
    workers_.push_back(std::move(worker));
    raw->thread = std::thread(
        [this, raw, stream = accepted.release(), guestPort] {
            serve(*raw, stream, guestPort);
        });
    return true;
}

void Forwarder::serve(Worker& worker, int accepted, std::uint32_t guestPort)
{
    uml::gpu::UniqueDescriptor stream {accepted};
    std::shared_ptr<Opening> opening;
    const int started =
        worker.endpoint->beginAttach(std::move(stream), guestPort, opening);
    if (started == 0) {
        {
            std::lock_guard lock(worker.mutex);
            if (worker.cancelled)
                opening->cancel();
            worker.opening = opening;
        }
        const int result = opening->wait(acceptTimeout);
        if (result != 0 && result != -ECANCELED) {
            std::fprintf(
                stderr, "vsock forward to guest port %u: %s\n", guestPort,
                std::strerror(-result));
        }
    } else {
        std::fprintf(
            stderr, "vsock forward to guest port %u: %s\n", guestPort,
            std::strerror(-started));
    }
    worker.done.store(true, std::memory_order_release);
}

void Forwarder::reap()
{
    std::list<std::unique_ptr<Worker>> finished;
    {
        std::lock_guard lock(mutex_);
        for (auto it = workers_.begin(); it != workers_.end();) {
            if ((*it)->done.load(std::memory_order_acquire)) {
                finished.push_back(std::move(*it));
                it = workers_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& worker : finished) {
        if (worker->thread.joinable())
            worker->thread.join();
    }
}

} // namespace uml::vsock
