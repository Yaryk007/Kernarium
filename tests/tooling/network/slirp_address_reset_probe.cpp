/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A guest TCP connection whose host socket is bound to an address the host
 * has lost is reset, once the path monitor says another interface carries the
 * path and a hold has passed (address_reset.hpp). The probe drives the
 * provider's test variant with a guest it writes by hand. A stand-in takes
 * the place of the system's list of addresses, of the path monitor's report
 * (the state's own report()), of the clock the hold is timed by and, in the
 * scenarios that need flows bound to different addresses, of the local
 * address of a flow's host socket. libslirp never lets a guest reach the
 * host's loopback, so those flows run through Unix-domain guest forwards, one
 * listener each, and the stand-in names the address each is bound to. The last
 * scenario asks the sockets themselves, with flows to the host's own LAN
 * address where it has one.
 *
 * What it requires, each a scenario:
 *  - the address is gone and an interface carries the path: after the hold
 *    (not a moment before) the flow bound to it gets a reset, the host socket
 *    closes, a flow bound to an address the host still has is untouched, and
 *    nothing is counted as a connection attempt;
 *  - the address is back before the hold ends, reported or not: nothing is
 *    reset;
 *  - a plain outage (no interface carries the path) resets nothing, however
 *    long it lasts; an interface that then carries the path with the address
 *    still gone resets after the hold;
 *  - an outage that begins and ends between two looks of the event loop, or
 *    during the read that ends the hold, starts the hold again: nothing is
 *    reset when the old one would have ended;
 *  - an address that goes missing without a report while the hold runs is not
 *    reset when the hold ends: the hold starts again for it;
 *  - the event loop learns of a report and of the end of a hold from the
 *    report's own wake and the hold's own deadline, not from its periodic
 *    pass: with the passes a second apart and a wake or a hold half a second
 *    off their beat, a late look shows (the clock of that scenario is the
 *    real one);
 *  - the table the read at the end of the hold returns is moved into storage:
 *    a copy of it would allocate, and the allocation is made to fail;
 *  - the provider stopped and opened again while a hold was pending: the hold
 *    does not call into the new instance;
 *  - asked of the real sockets: a connection to the host's LAN address is
 *    reset, while a connection through a Unix-domain forward, a UDP flow and
 *    a connection a host client made to a forward are not.
 */
#include "network_counters.hpp"
#include "provider.hpp"
#include "slirp_tcp_harness.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

/* An allocation of one size that fails once, for the scenario that shows the
 * event loop keeps a table it read without allocating: the whole process's
 * allocator, with nothing else changed. */
namespace allocation_trap {
std::atomic<std::size_t> bytes{0};
std::atomic<bool> fired{false};
} // namespace allocation_trap

void *operator new(std::size_t size)
{
	if (size != 0 &&
	    size == allocation_trap::bytes.load(std::memory_order_relaxed)) {
		allocation_trap::bytes.store(0, std::memory_order_relaxed);
		allocation_trap::fired.store(true, std::memory_order_relaxed);
		throw std::bad_alloc{};
	}
	if (void *memory = std::malloc(size != 0 ? size : 1))
		return memory;
	throw std::bad_alloc{};
}
void *operator new[](std::size_t size) { return operator new(size); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept
{
	std::free(memory);
}

namespace {

using namespace slirp_harness;
using uml::network::HostAddresses;
using uml::network::HostIpv4;
using uml::network::SlirpProvider;
using Address = std::array<std::uint8_t, 4>;
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;

/* TEST-NET-1: addresses no host holds; they stand for interface addresses. */
constexpr Address lost_address{192, 0, 2, 10};
constexpr Address kept_address{192, 0, 2, 20};
constexpr std::uint16_t forward_guest_port = 7;
constexpr std::uint16_t guest_udp_port = 5000;

/* How long a result that must come may take, and how long the probe watches
 * for one that must not. */
constexpr int prompt_ms = 3000;
constexpr int quiet_ms = 400;

int failures;

void fail(const char *scenario, const std::string &message)
{
	std::fprintf(stderr, "%s: %s\n", scenario, message.c_str());
	failures++;
}

void check(const char *scenario, bool condition, const std::string &message)
{
	if (!condition)
		fail(scenario, message);
}

std::uint32_t value(const Address &address)
{
	return (static_cast<std::uint32_t>(address[0]) << 24) |
		(static_cast<std::uint32_t>(address[1]) << 16) |
		(static_cast<std::uint32_t>(address[2]) << 8) | address[3];
}

/* The clock the hold is timed by: it moves only when the probe says. */
class TestClock final {
public:
	[[nodiscard]] Clock::time_point now() const
	{
		return base_ + std::chrono::nanoseconds{offset_.load()};
	}
	void advance(std::chrono::milliseconds amount)
	{
		offset_.fetch_add(
			std::chrono::duration_cast<std::chrono::nanoseconds>(amount)
				.count());
	}

private:
	const Clock::time_point base_{Clock::now()};
	std::atomic<std::int64_t> offset_{0};
};

/* A listener the guest's connections reach, and the host's end of the one
 * that arrived: a Unix-domain socket behind a guest forward at the gateway,
 * or a TCP socket on an address of the host. */
class Server final {
public:
	/* A Unix-domain listener forwarded from the gateway's `guest_port`. */
	Server(const std::string &directory, std::uint16_t guest_port)
		: guest_port_{guest_port}, address_{10, 0, 2, 2}
	{
		path_ = directory + "/s" + std::to_string(guest_port);
		sockaddr_un named{};
		named.sun_family = AF_UNIX;
		std::strncpy(named.sun_path, path_.c_str(),
			     sizeof(named.sun_path) - 1);
		descriptor_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
		if (descriptor_ < 0 ||
		    ::bind(descriptor_, reinterpret_cast<sockaddr *>(&named),
			   sizeof(named)) != 0 ||
		    ::listen(descriptor_, 4) != 0)
			close_all();
	}
	/* A TCP listener on `address`. */
	explicit Server(const Address &address) : address_{address}
	{
		descriptor_ = ::socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in where{};
		where.sin_family = AF_INET;
		where.sin_addr.s_addr = htonl(value(address));
		socklen_t length = sizeof(where);
		if (descriptor_ < 0 ||
		    ::bind(descriptor_, reinterpret_cast<sockaddr *>(&where),
			   sizeof(where)) != 0 ||
		    ::listen(descriptor_, 4) != 0 ||
		    ::getsockname(descriptor_, reinterpret_cast<sockaddr *>(&where),
				  &length) != 0) {
			close_all();
			return;
		}
		guest_port_ = ntohs(where.sin_port);
	}
	~Server()
	{
		close_all();
		if (!path_.empty())
			(void)::unlink(path_.c_str());
	}
	Server(const Server &) = delete;
	Server &operator=(const Server &) = delete;

	[[nodiscard]] bool ready() const noexcept { return descriptor_ >= 0; }
	/* Where the guest connects to reach it. */
	[[nodiscard]] const Address &address() const noexcept { return address_; }
	[[nodiscard]] std::uint16_t port() const noexcept { return guest_port_; }
	[[nodiscard]] const std::string &path() const noexcept { return path_; }

	/* The host's end of the connection, once it has arrived. */
	[[nodiscard]] bool accept_connection()
	{
		pollfd entry{descriptor_, POLLIN, 0};
		if (::poll(&entry, 1, prompt_ms) <= 0)
			return false;
		if (accepted_ >= 0)
			::close(accepted_);
		accepted_ = ::accept(descriptor_, nullptr, nullptr);
		return accepted_ >= 0;
	}

	/* Bytes the guest sent, read within the bound. */
	[[nodiscard]] std::size_t read_some(int wait_ms = prompt_ms)
	{
		pollfd entry{accepted_, POLLIN, 0};
		if (accepted_ < 0 || ::poll(&entry, 1, wait_ms) <= 0)
			return 0;
		std::array<char, 512> buffer{};
		const auto count = ::recv(accepted_, buffer.data(), buffer.size(), 0);
		return count > 0 ? static_cast<std::size_t>(count) : 0;
	}

	bool write_some(std::size_t length)
	{
		const std::string text(length, 'x');
		return accepted_ >= 0 &&
			::send(accepted_, text.data(), text.size(), 0) ==
				static_cast<ssize_t>(length);
	}

	/* Whether the host's end saw the connection end: an end of file or an
	 * error, within the bound. */
	[[nodiscard]] bool saw_close(int wait_ms = prompt_ms)
	{
		pollfd entry{accepted_, POLLIN, 0};
		if (accepted_ < 0 || ::poll(&entry, 1, wait_ms) <= 0)
			return false;
		std::array<char, 512> buffer{};
		return ::recv(accepted_, buffer.data(), buffer.size(), 0) <= 0;
	}

private:
	void close_all()
	{
		if (accepted_ >= 0)
			::close(accepted_);
		if (descriptor_ >= 0)
			::close(descriptor_);
		accepted_ = descriptor_ = -1;
	}

	int descriptor_{-1};
	int accepted_{-1};
	std::uint16_t guest_port_{};
	Address address_{};
	std::string path_;
};

/* A private directory for the Unix-domain sockets, removed with it. */
class Directory final {
public:
	Directory()
	{
		char base[] = "/tmp/uml-addrreset.XXXXXX";
		if (::mkdtemp(base) != nullptr)
			path_ = base;
	}
	~Directory()
	{
		if (!path_.empty())
			(void)::rmdir(path_.c_str());
	}
	Directory(const Directory &) = delete;
	Directory &operator=(const Directory &) = delete;
	[[nodiscard]] const std::string &path() const noexcept { return path_; }

private:
	std::string path_;
};

Bytes udp_frame(const Address &source, const Address &destination,
		std::uint16_t source_port, std::uint16_t destination_port,
		const Bytes &payload)
{
	const std::size_t udp_length = 8 + payload.size();
	Bytes frame(14 + 20 + udp_length, 0);
	for (std::size_t index = 0; index < 6; ++index) {
		frame[index] = 0xff;
		frame[6 + index] = guest_mac[index];
	}
	put16(frame, 12, 0x0800);
	frame[14] = 0x45;
	put16(frame, 16, static_cast<std::uint16_t>(20 + udp_length));
	put16(frame, 20, 0x4000);
	frame[22] = 64;
	frame[23] = IPPROTO_UDP;
	for (std::size_t index = 0; index < 4; ++index) {
		frame[26 + index] = source[index];
		frame[30 + index] = destination[index];
	}
	put16(frame, 24, checksum_fold(checksum_add(0, &frame[14], 20)));
	put16(frame, 34, source_port);
	put16(frame, 36, destination_port);
	put16(frame, 38, static_cast<std::uint16_t>(udp_length));
	std::copy(payload.begin(), payload.end(), frame.begin() + 42);
	std::uint32_t sum = checksum_add(0, &frame[26], 8);
	sum += IPPROTO_UDP;
	sum += static_cast<std::uint32_t>(udp_length);
	sum = checksum_add(sum, &frame[34], udp_length);
	const auto folded = checksum_fold(sum);
	put16(frame, 40, folded == 0 ? 0xffff : folded);
	return frame;
}

/* One guest TCP connection through the provider. */
struct Flow {
	Address remote{};
	std::uint16_t guest_port{};
	std::uint16_t remote_port{};
	std::uint32_t guest_seq{};
	std::uint32_t remote_seq{};
};

/* The provider, its stand-ins and the guest. */
class Rig final {
public:
	/* `real_clock`: the hold is timed by the system's clock. */
	explicit Rig(bool real_clock = false)
	{
		if (!real_clock)
			provider.set_clock_for_testing(
				[this] { return clock.now(); });
		provider.set_host_address_reader_for_testing(
			[this]() -> std::optional<HostAddresses> {
				std::function<void()> action;
				std::optional<HostAddresses> table;
				{
					std::lock_guard guard{lock_};
					action = std::move(on_read_);
					on_read_ = nullptr;
					if (fail_next_read_) {
						fail_next_read_ = false;
						return std::nullopt;
					}
					if (next_read_) {
						table = std::move(next_read_);
						next_read_.reset();
					} else {
						table.emplace();
						table->assign_ipv4(addresses_);
					}
				}
				/* Outside the lock: the action may publish reports. */
				if (action)
					action();
				return table;
			});
	}
	~Rig() { provider.stop(); }
	Rig(const Rig &) = delete;
	Rig &operator=(const Rig &) = delete;

	/* The host's addresses, as the system lists them from now on. */
	void set_addresses(std::initializer_list<Address> addresses)
	{
		std::lock_guard guard{lock_};
		addresses_.clear();
		for (const auto &address : addresses)
			addresses_.push_back(value(address));
	}

	/* What happens during the next read of the host's addresses, on the
	 * thread that reads: `action` runs after the table is taken. */
	void during_next_read(std::function<void()> action)
	{
		std::lock_guard guard{lock_};
		on_read_ = std::move(action);
	}

	/* The next read of the host's addresses fails, as a system that cannot
	 * list them does. */
	void fail_next_read()
	{
		std::lock_guard guard{lock_};
		fail_next_read_ = true;
	}

	/* The next read of the host's addresses returns `table`, moved out. */
	void next_read_returns(HostAddresses table)
	{
		std::lock_guard guard{lock_};
		next_read_ = std::move(table);
	}

	/* The guest's connections to `server`, a Unix-domain one, are forwarded
	 * to it; applies to every open. */
	void forward_to(const Server &server)
	{
		provider.add_guest_forward_for_testing(
			{uml::network::host_alias_address, server.port(),
			 server.path()});
	}

	/* The flows that reach `server` are bound to `address`, as the stand-in
	 * tells the provider. */
	void bind_flow(const Server &server, const Address &address)
	{
		std::lock_guard guard{lock_};
		bound_[server.path()] = address;
	}

	/* Installs the local-address stand-in: the host socket of a flow is
	 * bound to the address its Unix-domain peer was given. */
	void use_bound_addresses()
	{
		provider.set_flow_local_address_for_testing(
			[this](slirp_os_socket descriptor)
				-> std::optional<HostIpv4> {
				sockaddr_un peer{};
				socklen_t length = sizeof(peer);
				if (::getpeername(descriptor,
						  reinterpret_cast<sockaddr *>(&peer),
						  &length) != 0 ||
				    peer.sun_family != AF_UNIX)
					return std::nullopt;
				std::lock_guard guard{lock_};
				const auto found = bound_.find(peer.sun_path);
				if (found == bound_.end())
					return std::nullopt;
				return found->second;
			});
	}

	bool open()
	{
		std::array<unsigned char, 6> mac{};
		unsigned int mtu = 0;
		return provider.open(mac, mtu) == 0 && resolve_gateway();
	}

	/* The system reports its path: an interface carries it, or none. The
	 * event loop has read the report when this returns, woken by the
	 * report alone. */
	void report_carried()
	{
		provider.path_state_for_testing().report(true, true);
		settle_reports();
	}
	void report_outage()
	{
		provider.path_state_for_testing().report(false, false);
		settle_reports();
	}

	/* Time passes for the hold, and the event loop has looked at it when
	 * this returns: a test clock gives no wake of its own, so one is given. */
	void advance(std::chrono::milliseconds amount)
	{
		clock.advance(amount);
		/* A pass that begins after this read reads the clock as moved. */
		const auto begun = provider.event_loop_passes_for_testing();
		provider.wake_for_testing();
		wait_for_checks(begun + 1);
		settle_reports();
	}

	/* Waits until the event loop has read every report, those published
	 * while it read included, with no wake but the reports' own. */
	void settle_reports()
	{
		const auto deadline = Clock::now() + std::chrono::milliseconds{prompt_ms};
		while (Clock::now() < deadline) {
			if (provider.path_reports_handled_for_testing() >=
			    provider.path_state_for_testing().reports())
				return;
			std::this_thread::sleep_for(std::chrono::milliseconds{2});
		}
		fail("settle", "a report did not wake the event loop in time");
	}

	/* Waits until the event loop has looked at the hold `checks` times. */
	void wait_for_checks(std::uint64_t checks)
	{
		const auto deadline = Clock::now() + std::chrono::milliseconds{prompt_ms};
		while (Clock::now() < deadline) {
			if (provider.address_reset_checks_for_testing() >= checks)
				return;
			std::this_thread::sleep_for(std::chrono::milliseconds{2});
		}
		fail("settle", "the event loop did not look at the hold in time");
	}

	/* Segments that arrive for the guest within `wait_ms`. */
	std::vector<Segment> watch(int wait_ms)
	{
		std::vector<Segment> segments;
		const auto deadline = Clock::now() + std::chrono::milliseconds{wait_ms};
		while (Clock::now() < deadline) {
			pump(&segments);
			pollfd entry{provider.irq_fd(), POLLIN, 0};
			(void)::poll(&entry, 1, 10);
		}
		pump(&segments);
		return segments;
	}

	/* The first segment for which `accept` is true, within `wait_ms`. */
	template <class Accept>
	std::optional<Segment> wait_for(Accept accept, int wait_ms)
	{
		const auto deadline = Clock::now() + std::chrono::milliseconds{wait_ms};
		for (;;) {
			std::vector<Segment> segments;
			pump(&segments);
			for (const auto &segment : segments)
				if (accept(segment))
					return segment;
			if (Clock::now() >= deadline)
				return std::nullopt;
			pollfd entry{provider.irq_fd(), POLLIN, 0};
			(void)::poll(&entry, 1, 20);
		}
	}

	/* A connection the guest opens to `server`, through the handshake. */
	std::optional<Flow> connect(const Server &server)
	{
		Flow flow;
		flow.remote = server.address();
		flow.guest_port = next_port_++;
		flow.remote_port = server.port();
		flow.guest_seq = 1000;
		(void)send(tcp_frame(guest_ip, flow.remote, flow.guest_port,
				     flow.remote_port, flow.guest_seq, 0, tcp_syn,
				     65535));
		const auto synack = wait_for(
			[&](const Segment &segment) {
				return segment.destination_port == flow.guest_port &&
					(segment.flags & (tcp_syn | tcp_ack)) ==
						(tcp_syn | tcp_ack);
			},
			prompt_ms);
		if (!synack)
			return std::nullopt;
		flow.guest_seq++;
		flow.remote_seq = synack->seq + 1;
		(void)send(tcp_frame(guest_ip, flow.remote, flow.guest_port,
				     flow.remote_port, flow.guest_seq,
				     flow.remote_seq, tcp_ack, 65535));
		return flow;
	}

	/* The guest writes `length` bytes on `flow`. */
	void write(Flow &flow, std::size_t length)
	{
		(void)send(tcp_frame(guest_ip, flow.remote, flow.guest_port,
				     flow.remote_port, flow.guest_seq,
				     flow.remote_seq, tcp_psh | tcp_ack, 65535,
				     length));
		flow.guest_seq += static_cast<std::uint32_t>(length);
	}

	/* Whether the connection got a reset within `wait_ms`. */
	std::optional<Segment> reset_of(const Flow &flow, int wait_ms)
	{
		return wait_for(
			[&](const Segment &segment) {
				return (segment.flags & tcp_rst) != 0 &&
					segment.destination_port == flow.guest_port &&
					segment.source_port == flow.remote_port;
			},
			wait_ms);
	}

	/* Whether data from the host's end reached the guest. */
	bool data_reached_guest(const Flow &flow, int wait_ms)
	{
		return wait_for(
			       [&](const Segment &segment) {
				       return segment.payload != 0 &&
					       segment.destination_port ==
						       flow.guest_port &&
					       segment.source_port == flow.remote_port;
			       },
			       wait_ms)
			.has_value();
	}

	bool send(const Bytes &frame)
	{
		return provider.write(std::as_bytes(std::span{frame})) == 0;
	}

	/* Every frame SLiRP has for the guest: TCP segments parsed, and the UDP
	 * datagrams kept for datagrams_to(). */
	void pump(std::vector<Segment> *segments)
	{
		std::array<std::byte, uml::network::frame_size> buffer{};
		for (;;) {
			const int length = provider.read(buffer);
			if (length <= 0)
				return;
			const auto *bytes =
				reinterpret_cast<const std::uint8_t *>(buffer.data());
			Bytes frame(bytes, bytes + length);
			if (auto segment = parse_tcp(frame)) {
				if (segments != nullptr)
					segments->push_back(*segment);
			} else if (frame.size() >= 42 && get16(frame, 12) == 0x0800 &&
				   frame[23] == IPPROTO_UDP) {
				datagrams_.push_back(
					{get16(frame, 34), get16(frame, 36)});
			}
		}
	}

	/* UDP datagrams from `source_port` that reached the guest's `port`. */
	std::size_t datagrams_to(std::uint16_t port, std::uint16_t source_port)
	{
		std::vector<Segment> ignored;
		pump(&ignored);
		return static_cast<std::size_t>(std::count_if(
			datagrams_.begin(), datagrams_.end(), [&](const auto &entry) {
				return entry.first == source_port && entry.second == port;
			}));
	}

	SlirpProvider provider;
	TestClock clock;

private:
	bool resolve_gateway()
	{
		Bytes request(42, 0);
		for (std::size_t index = 0; index < 6; ++index) {
			request[index] = 0xff;
			request[6 + index] = guest_mac[index];
			request[22 + index] = guest_mac[index];
		}
		put16(request, 12, 0x0806);
		put16(request, 14, 1);
		put16(request, 16, 0x0800);
		request[18] = 6;
		request[19] = 4;
		put16(request, 20, 1);
		for (std::size_t index = 0; index < 4; ++index) {
			request[28 + index] = guest_ip[index];
			request[38 + index] = gateway_ip[index];
		}
		(void)send(request);
		const auto deadline = Clock::now() + std::chrono::milliseconds{prompt_ms};
		std::array<std::byte, uml::network::frame_size> buffer{};
		while (Clock::now() < deadline) {
			const int length = provider.read(buffer);
			if (length <= 0) {
				pollfd entry{provider.irq_fd(), POLLIN, 0};
				(void)::poll(&entry, 1, 20);
				continue;
			}
			if (length >= 42 &&
			    ((static_cast<unsigned>(buffer[12]) << 8) |
			     static_cast<unsigned>(buffer[13])) == 0x0806)
				return true;
		}
		return false;
	}

	std::mutex lock_;
	std::vector<std::uint32_t> addresses_;
	std::function<void()> on_read_;
	std::optional<HostAddresses> next_read_;
	bool fail_next_read_{};
	std::map<std::string, Address> bound_;
	std::vector<std::pair<std::uint16_t, std::uint16_t>> datagrams_;
	std::uint16_t next_port_{40000};
};

/* Common set-up of the scenarios that stand the flows for bound addresses:
 * the host holds the lost and the kept address, and one flow is bound to
 * each. */
struct Pair {
	Directory directory;
	Server lost_server{directory.path(), 8081};
	Server kept_server{directory.path(), 8082};
	std::optional<Flow> lost;
	std::optional<Flow> kept;
};

bool make_pair(const char *scenario, Rig &rig, Pair &pair)
{
	rig.set_addresses({lost_address, kept_address});
	rig.use_bound_addresses();
	if (!pair.lost_server.ready() || !pair.kept_server.ready()) {
		fail(scenario, "no Unix-domain listener");
		return false;
	}
	rig.forward_to(pair.lost_server);
	rig.forward_to(pair.kept_server);
	rig.bind_flow(pair.lost_server, lost_address);
	rig.bind_flow(pair.kept_server, kept_address);
	if (!rig.open()) {
		fail(scenario, "the provider did not open");
		return false;
	}
	pair.lost = rig.connect(pair.lost_server);
	pair.kept = rig.connect(pair.kept_server);
	if (!pair.lost || !pair.kept || !pair.lost_server.accept_connection() ||
	    !pair.kept_server.accept_connection()) {
		fail(scenario, "the guest could not open its connections");
		return false;
	}
	return true;
}

/* Whether the host's end of `server` carries a write from the guest on
 * `flow`, and the guest hears the host's reply: the flow is alive. */
bool flow_is_alive(Rig &rig, Flow &flow, Server &server)
{
	rig.write(flow, 40);
	if (server.read_some() == 0 || !server.write_some(30))
		return false;
	return rig.data_reached_guest(flow, prompt_ms);
}

bool no_reset_among(const std::vector<Segment> &segments)
{
	return std::none_of(segments.begin(), segments.end(),
			    [](const Segment &segment) {
				    return (segment.flags & tcp_rst) != 0;
			    });
}

void lost_address_is_reset_after_the_hold()
{
	const char *scenario = "lost address, carried path";
	Rig rig;
	Pair pair;
	if (!make_pair(scenario, rig, pair))
		return;
	const auto before = rig.provider.counters();

	rig.report_carried();
	rig.set_addresses({kept_address});
	rig.report_carried();
	/* A moment short of the hold: nothing yet. */
	rig.advance(uml::network::address_reset_hold -
		    std::chrono::milliseconds{100});
	check(scenario, no_reset_among(rig.watch(quiet_ms)),
	      "a flow was reset before the hold had passed");
	check(scenario, rig.provider.address_reset_calls_for_testing() == 0,
	      "the hold ended early");

	rig.advance(std::chrono::milliseconds{200});
	check(scenario, rig.reset_of(*pair.lost, prompt_ms).has_value(),
	      "the flow bound to the lost address got no reset");
	check(scenario, pair.lost_server.saw_close(),
	      "the host socket of the reset flow stayed open");
	const auto after = rig.watch(quiet_ms);
	check(scenario,
	      std::none_of(after.begin(), after.end(), [&](const Segment &segment) {
		      return (segment.flags & tcp_rst) != 0 &&
			      segment.destination_port == pair.kept->guest_port;
	      }),
	      "the flow bound to the address the host still has was reset");
	check(scenario, flow_is_alive(rig, *pair.kept, pair.kept_server),
	      "the flow bound to the kept address stopped working");
	check(scenario, rig.provider.address_reset_calls_for_testing() == 1,
	      "libslirp was not asked exactly once");
	const auto counts = rig.provider.counters();
	check(scenario,
	      counts.tcp_reset == before.tcp_reset &&
		      counts.tcp_attempts == before.tcp_attempts,
	      "an established flow's reset counted as a connection attempt");
}

void address_back_within_the_hold_resets_nothing()
{
	const char *scenario = "address back within the hold";
	Rig rig;
	Pair pair;
	if (!make_pair(scenario, rig, pair))
		return;
	rig.report_carried();
	rig.set_addresses({kept_address});
	rig.report_carried();
	rig.advance(std::chrono::seconds{3});
	/* Back before the hold ended. */
	rig.set_addresses({lost_address, kept_address});
	rig.report_carried();
	rig.advance(uml::network::address_reset_hold +
		    std::chrono::milliseconds{100});
	check(scenario, no_reset_among(rig.watch(2 * quiet_ms)),
	      "a flow was reset though its address came back");
	check(scenario, flow_is_alive(rig, *pair.lost, pair.lost_server),
	      "the flow whose address came back stopped working");
	check(scenario, rig.provider.address_reset_calls_for_testing() == 1,
	      "the hold did not end exactly once");
}

void address_back_without_a_report_resets_nothing()
{
	/* The hold judges the addresses as they are when it ends, not as they
	 * were at the report that started it: an address that came back with no
	 * report of its own is not lost either. */
	const char *scenario = "address back without a report";
	Rig rig;
	Pair pair;
	if (!make_pair(scenario, rig, pair))
		return;
	rig.report_carried();
	rig.set_addresses({kept_address});
	rig.report_carried();
	rig.advance(std::chrono::seconds{3});
	rig.set_addresses({lost_address, kept_address});
	rig.advance(uml::network::address_reset_hold);
	check(scenario, no_reset_among(rig.watch(2 * quiet_ms)),
	      "a flow was reset though its address came back");
	check(scenario, flow_is_alive(rig, *pair.lost, pair.lost_server),
	      "the flow whose address came back stopped working");
	check(scenario, rig.provider.address_reset_calls_for_testing() == 1,
	      "the hold did not end exactly once");
}

void a_plain_outage_resets_nothing()
{
	const char *scenario = "outage";
	Rig rig;
	Pair pair;
	if (!make_pair(scenario, rig, pair))
		return;
	rig.report_carried();
	rig.set_addresses({kept_address});
	/* No interface carries the path: no hold, however long it lasts. */
	rig.report_outage();
	rig.advance(std::chrono::seconds{600});
	check(scenario, no_reset_among(rig.watch(2 * quiet_ms)),
	      "a flow was reset during an outage");
	check(scenario, rig.provider.address_reset_calls_for_testing() == 0,
	      "an outage started a hold");

	/* The same address returns with the path: nothing to reset. */
	rig.set_addresses({lost_address, kept_address});
	rig.report_carried();
	rig.advance(uml::network::address_reset_hold +
		    std::chrono::milliseconds{100});
	check(scenario, no_reset_among(rig.watch(2 * quiet_ms)),
	      "a flow was reset though its address returned with the path");
	check(scenario, flow_is_alive(rig, *pair.lost, pair.lost_server),
	      "the flow stopped working after the outage");

	/* An interface carries the path and the address stays gone. */
	rig.set_addresses({kept_address});
	rig.report_carried();
	rig.advance(uml::network::address_reset_hold +
		    std::chrono::milliseconds{100});
	check(scenario, rig.reset_of(*pair.lost, prompt_ms).has_value(),
	      "an interface that carries the path left the lost flow alone");
}

/* The scenarios of the hold's edges start alike: the host holds the kept
 * address only, the lost one gone before the hold starts, and an interface
 * carries the path. */
bool start_hold_with_lost_address_gone(const char *scenario, Rig &rig, Pair &pair)
{
	if (!make_pair(scenario, rig, pair))
		return false;
	rig.set_addresses({kept_address});
	rig.report_carried();
	return true;
}

void an_outage_between_two_passes_starts_the_hold_again()
{
	const char *scenario = "outage between two passes";
	Rig rig;
	Pair pair;
	if (!start_hold_with_lost_address_gone(scenario, rig, pair))
		return;
	rig.advance(std::chrono::milliseconds{7900});
	/* While the loop reads the table for the next report, the system
	 * reports an outage and the path coming back: the loop sees one report
	 * more than it can tell apart from an ordinary one, and the flag says
	 * carried. */
	auto &state = rig.provider.path_state_for_testing();
	rig.during_next_read([&state] {
		state.report(false, false);
		state.report(true, true);
	});
	rig.report_carried();
	/* The old hold would have ended at 8 s. */
	rig.advance(std::chrono::milliseconds{200});
	check(scenario, no_reset_among(rig.watch(quiet_ms)),
	      "an outage that came and went reset a flow when the old hold ended");
	check(scenario, rig.provider.address_reset_calls_for_testing() == 0,
	      "the old hold ended though an outage had cancelled it");
	rig.advance(std::chrono::milliseconds{7600});
	check(scenario, no_reset_among(rig.watch(quiet_ms)),
	      "the hold did not start again at the outage");
	/* The address is still gone a whole hold after the path was back. */
	rig.advance(std::chrono::milliseconds{400});
	check(scenario, rig.reset_of(*pair.lost, prompt_ms).has_value(),
	      "the new hold ended in no reset");
	check(scenario, rig.provider.address_reset_calls_for_testing() == 1,
	      "libslirp was not asked exactly once");
}

void an_outage_during_the_read_that_ends_the_hold_resets_nothing()
{
	const char *scenario = "outage during the read at the end of the hold";
	Rig rig;
	Pair pair;
	if (!start_hold_with_lost_address_gone(scenario, rig, pair))
		return;
	rig.advance(std::chrono::milliseconds{7900});
	/* The hold ends carried; while its table is read the path is lost and
	 * comes back: the flag alone says carried again. */
	auto &state = rig.provider.path_state_for_testing();
	rig.during_next_read([&state] {
		state.report(false, false);
		state.report(true, true);
	});
	rig.advance(std::chrono::milliseconds{200});
	check(scenario, no_reset_among(rig.watch(quiet_ms)),
	      "a flow was reset though an outage came during the read");
	check(scenario, rig.provider.address_reset_calls_for_testing() == 0,
	      "libslirp was asked though an outage came during the read");
	/* The outage started the hold again from the path coming back. */
	rig.advance(std::chrono::milliseconds{7700});
	check(scenario, no_reset_among(rig.watch(quiet_ms)),
	      "the outage did not start the hold again");
	rig.advance(std::chrono::milliseconds{300});
	check(scenario, rig.reset_of(*pair.lost, prompt_ms).has_value(),
	      "the new hold ended in no reset");
}

void an_address_that_goes_missing_at_the_end_of_the_hold_gets_a_hold_of_its_own()
{
	const char *scenario = "address missing since the end of the hold";
	Rig rig;
	Pair pair;
	if (!make_pair(scenario, rig, pair))
		return;
	/* The hold starts with both addresses held. */
	rig.report_carried();
	rig.advance(std::chrono::milliseconds{7950});
	/* The lost one goes with no report, 50 ms before the hold ends. */
	rig.set_addresses({kept_address});
	rig.advance(std::chrono::milliseconds{100});
	check(scenario, no_reset_among(rig.watch(quiet_ms)),
	      "an address missing for 50 ms was reset");
	check(scenario, rig.provider.address_reset_calls_for_testing() == 1,
	      "the hold did not end once");
	rig.advance(std::chrono::milliseconds{7800});
	check(scenario, no_reset_among(rig.watch(quiet_ms)),
	      "the address was reset before it had been gone for a hold");
	rig.advance(std::chrono::milliseconds{300});
	check(scenario, rig.reset_of(*pair.lost, prompt_ms).has_value(),
	      "an address gone for a whole hold was not reset");
	check(scenario, rig.provider.address_reset_calls_for_testing() == 2,
	      "the address's own hold did not end once");
}

void a_failed_read_at_the_start_of_a_hold_leaves_it_no_first_sample()
{
	/* The sample an outage's report read lacks the lost address, the address
	 * returns, and the read at the report that brings the path back fails:
	 * the hold that starts has no first sample, and the outage's is not its
	 * own. The address goes again, with no report, 50 ms before the hold
	 * ends. */
	const char *scenario = "failed read at the start of the hold";
	Rig rig;
	Pair pair;
	if (!make_pair(scenario, rig, pair))
		return;
	rig.set_addresses({kept_address});
	rig.report_outage();
	rig.set_addresses({lost_address, kept_address});
	rig.fail_next_read();
	rig.report_carried();
	rig.advance(std::chrono::milliseconds{7950});
	rig.set_addresses({kept_address});
	rig.advance(std::chrono::milliseconds{100});
	check(scenario, no_reset_among(rig.watch(quiet_ms)),
	      "an address missing for 50 ms was reset on the strength of an "
	      "older hold's sample");
	check(scenario, rig.provider.address_reset_calls_for_testing() == 0,
	      "libslirp was asked with no first sample");
	rig.advance(std::chrono::milliseconds{7800});
	check(scenario, no_reset_among(rig.watch(quiet_ms)),
	      "the address was reset before it had been gone for a hold");
	rig.advance(std::chrono::milliseconds{300});
	check(scenario, rig.reset_of(*pair.lost, prompt_ms).has_value(),
	      "an address gone for a whole hold was not reset");
	check(scenario, rig.provider.address_reset_calls_for_testing() == 1,
	      "the address's own hold did not end once");
}

/* Waits for the event loop to finish a pass, then lets half a second go by:
 * the next periodic pass is half a second away. */
void off_the_beat_of_the_loop(Rig &rig)
{
	const auto passes = rig.provider.event_loop_passes_for_testing();
	const auto deadline = Clock::now() + std::chrono::milliseconds{2500};
	while (rig.provider.event_loop_passes_for_testing() == passes &&
	       Clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds{2});
	std::this_thread::sleep_for(std::chrono::milliseconds{500});
}

void the_loop_needs_no_wake_but_the_reports_and_the_deadline()
{
	/* With no connection open the loop passes once a second and no oftener
	 * (libslirp asks for a quicker pass while a connection lives). A report
	 * half a second after a pass must be read at once, by the wake it gives;
	 * a hold that ends half a second after a pass must be looked at when it
	 * ends, by its own deadline. A late look is a wake the provider forgot;
	 * a few tries tell that from a loaded machine. */
	const char *scenario = "report wake and deadline wake";
	Rig rig{true};
	rig.set_addresses({kept_address});
	if (!rig.open()) {
		fail(scenario, "the provider did not open");
		return;
	}
	auto &state = rig.provider.path_state_for_testing();

	bool read_at_once = false;
	for (int attempt = 0; attempt < 3 && !read_at_once; ++attempt) {
		off_the_beat_of_the_loop(rig);
		const auto reports = state.reports() + 1;
		const auto start = Clock::now();
		/* An outage: it starts no hold. */
		state.report(false, false);
		while (rig.provider.path_reports_handled_for_testing() < reports &&
		       Clock::now() < start + std::chrono::milliseconds{prompt_ms})
			std::this_thread::sleep_for(std::chrono::milliseconds{1});
		read_at_once = Clock::now() - start < std::chrono::milliseconds{250};
	}
	check(scenario, read_at_once,
	      "a report was read at the next periodic pass, not at once");

	const auto hold = std::chrono::duration_cast<std::chrono::milliseconds>(
		uml::network::address_reset_hold);
	bool ended_on_time = false;
	for (int attempt = 0; attempt < 2 && !ended_on_time; ++attempt) {
		const auto asked = rig.provider.address_reset_calls_for_testing();
		/* The hold ends no sooner than a hold after this reading of the
		 * clock, which the report follows: a test thread that is late
		 * later on cannot make the end look early. */
		const auto published = Clock::now();
		state.report(true, true);
		rig.settle_reports();
		const auto started = Clock::now();
		/* The beat now follows the pass that began the hold: shift it. */
		std::this_thread::sleep_for(std::chrono::milliseconds{500});
		rig.provider.wake_for_testing();
		const auto limit = started + hold + std::chrono::milliseconds{3000};
		while (rig.provider.address_reset_calls_for_testing() == asked &&
		       Clock::now() < limit)
			std::this_thread::sleep_for(std::chrono::milliseconds{2});
		const auto seen = Clock::now();
		const auto late = seen - (started + hold);
		check(scenario,
		      rig.provider.address_reset_calls_for_testing() > asked,
		      "the end of the hold was never looked at");
		check(scenario, seen >= published + hold,
		      "the hold ended before its time");
		ended_on_time = late < std::chrono::milliseconds{300};
	}
	check(scenario, ended_on_time,
	      "the end of the hold was looked at at the next periodic pass, "
	      "not when it ended");
}

void the_table_read_at_the_end_of_the_hold_is_kept_without_allocating()
{
	const char *scenario = "table kept without a copy";
	Rig rig;
	Pair pair;
	if (!start_hold_with_lost_address_gone(scenario, rig, pair))
		return;
	/* A table whose copy would allocate a block no other allocation of the
	 * event loop does; the hold ends with it read, and the allocation of that
	 * size is made to fail. A copy in the event loop, which cannot throw,
	 * would end the process. */
	constexpr std::size_t count = 1237;
	std::vector<std::uint32_t> addresses{value(kept_address)};
	for (std::size_t index = 1; index < count; ++index)
		addresses.push_back(0xc6120000U + static_cast<std::uint32_t>(index));
	HostAddresses big;
	big.assign_ipv4(std::move(addresses));
	rig.advance(std::chrono::milliseconds{7900});
	rig.next_read_returns(std::move(big));
	allocation_trap::fired.store(false);
	allocation_trap::bytes.store(count * sizeof(std::uint32_t));
	rig.advance(std::chrono::milliseconds{200});
	allocation_trap::bytes.store(0);
	check(scenario, !allocation_trap::fired.load(),
	      "the table read at the end of the hold was copied");
	check(scenario, rig.reset_of(*pair.lost, prompt_ms).has_value(),
	      "the flow bound to the lost address got no reset");
}

void a_hold_does_not_outlive_its_instance()
{
	const char *scenario = "stop and reopen during a hold";
	Rig rig;
	Pair pair;
	if (!make_pair(scenario, rig, pair))
		return;
	rig.report_carried();
	rig.set_addresses({kept_address});
	rig.report_carried();
	rig.provider.stop();
	/* The system's monitor lives across a stop, so a report can come between
	 * the stop and the next open: it reaches the state and nothing else. */
	rig.provider.path_state_for_testing().report(false, false);
	rig.provider.path_state_for_testing().report(true, true);

	/* The same provider opens again; a flow bound to the lost address, on the
	 * new instance, is not the old hold's business, nor are the reports that
	 * came while it was stopped. */
	if (!rig.open()) {
		fail(scenario, "the provider did not open again");
		return;
	}
	auto flow = rig.connect(pair.lost_server);
	if (!flow || !pair.lost_server.accept_connection()) {
		fail(scenario, "the guest could not open its connection");
		return;
	}
	/* An old hold that outlived its instance would end with no table to
	 * judge by and start a hold of its own: look past that one too. */
	rig.advance(std::chrono::seconds{60});
	rig.advance(std::chrono::seconds{60});
	check(scenario, no_reset_among(rig.watch(2 * quiet_ms)),
	      "the old instance's hold reset a flow of the new one");
	check(scenario, rig.provider.address_reset_calls_for_testing() == 0,
	      "the old instance's hold called into the new one");
}

/* The addresses of the host that a probe may use as its own, none if it has
 * only the loopback. */
std::optional<Address> host_lan_address()
{
	ifaddrs *list = nullptr;
	if (::getifaddrs(&list) != 0)
		return std::nullopt;
	std::optional<Address> found;
	for (const ifaddrs *entry = list; entry != nullptr && !found;
	     entry = entry->ifa_next) {
		if (entry->ifa_addr == nullptr ||
		    entry->ifa_addr->sa_family != AF_INET ||
		    (entry->ifa_flags & IFF_LOOPBACK) != 0 ||
		    (entry->ifa_flags & IFF_UP) == 0)
			continue;
		const auto host = ntohl(reinterpret_cast<const sockaddr_in *>(
						entry->ifa_addr)->sin_addr.s_addr);
		/* Not an address SLiRP drops (link-local) or serves (its own
		 * 10.0.2.0/24): a host on another 10.x network has one to use. */
		if ((host >> 24) == 169 || (host >> 8) == 0x0a0002U)
			continue;
		found = Address{static_cast<std::uint8_t>(host >> 24),
				static_cast<std::uint8_t>(host >> 16),
				static_cast<std::uint8_t>(host >> 8),
				static_cast<std::uint8_t>(host)};
	}
	::freeifaddrs(list);
	return found;
}

std::uint16_t unused_loopback_port()
{
	const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in where{};
	where.sin_family = AF_INET;
	where.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t length = sizeof(where);
	std::uint16_t port = 0;
	if (descriptor >= 0 &&
	    ::bind(descriptor, reinterpret_cast<sockaddr *>(&where),
		   sizeof(where)) == 0 &&
	    ::getsockname(descriptor, reinterpret_cast<sockaddr *>(&where),
			  &length) == 0)
		port = ntohs(where.sin_port);
	if (descriptor >= 0)
		::close(descriptor);
	return port;
}

/* A UDP exchange between the guest and a host socket on `address`: the guest
 * sends, the host answers, and the answer reaches the guest. */
bool udp_round_trip(Rig &rig, int udp, std::uint16_t udp_port,
		    const Address &address)
{
	const auto before = rig.datagrams_to(guest_udp_port, udp_port);
	(void)rig.send(udp_frame(guest_ip, address, guest_udp_port, udp_port,
				 Bytes{'p', 'i', 'n', 'g'}));
	pollfd entry{udp, POLLIN, 0};
	if (::poll(&entry, 1, prompt_ms) <= 0)
		return false;
	char buffer[16];
	sockaddr_in from{};
	socklen_t from_length = sizeof(from);
	const auto count = ::recvfrom(udp, buffer, sizeof(buffer), 0,
				      reinterpret_cast<sockaddr *>(&from),
				      &from_length);
	if (count <= 0 ||
	    ::sendto(udp, buffer, static_cast<std::size_t>(count), 0,
		     reinterpret_cast<sockaddr *>(&from),
		     from_length) != count)
		return false;
	const auto deadline = Clock::now() + std::chrono::milliseconds{prompt_ms};
	while (Clock::now() < deadline) {
		if (rig.datagrams_to(guest_udp_port, udp_port) > before)
			return true;
		pollfd wait{rig.provider.irq_fd(), POLLIN, 0};
		(void)::poll(&wait, 1, 20);
	}
	return false;
}

void the_sockets_decide_what_is_reset()
{
	const char *scenario = "real sockets";
	Directory directory;
	Server through_unix{directory.path(), 8083};
	if (!through_unix.ready()) {
		fail(scenario, "no Unix-domain listener");
		return;
	}
	const auto lan = host_lan_address();
	if (!lan)
		std::puts("note: the host has no usable address (only loopback, "
			  "link-local or SLiRP's own 10.0.2.0/24); the connection to "
			  "a host address is not checked");

	Rig rig;
	/* The host has one address, and it is not one of its own: every host
	 * socket bound to a real address is bound to a lost one, if the
	 * sockets are asked. */
	rig.set_addresses({kept_address});
	rig.forward_to(through_unix);
	uml::network::HostForward forward{};
	forward.host_port = unused_loopback_port();
	forward.guest_port = forward_guest_port;
	if (forward.host_port == 0 || rig.provider.add_forward(forward) != 0) {
		fail(scenario, "no loopback forward");
		return;
	}
	if (!rig.open()) {
		fail(scenario, "the provider did not open");
		return;
	}

	/* A connection to a socket of the host's own address (bound to it), a
	 * connection through a Unix-domain forward, and a UDP flow to the same
	 * address. */
	std::optional<Server> server;
	std::optional<Flow> bound;
	int udp = -1;
	std::uint16_t udp_port = 0;
	if (lan) {
		server.emplace(*lan);
		bound = rig.connect(*server);
		if (!bound || !server->accept_connection()) {
			fail(scenario, "no connection to the host's address");
			return;
		}
		udp = ::socket(AF_INET, SOCK_DGRAM, 0);
		sockaddr_in name{};
		name.sin_family = AF_INET;
		name.sin_addr.s_addr = htonl(value(*lan));
		socklen_t name_length = sizeof(name);
		if (udp < 0 ||
		    ::bind(udp, reinterpret_cast<sockaddr *>(&name),
			   sizeof(name)) != 0 ||
		    ::getsockname(udp, reinterpret_cast<sockaddr *>(&name),
				  &name_length) != 0) {
			fail(scenario, "no UDP socket");
			return;
		}
		udp_port = ntohs(name.sin_port);
		check(scenario, udp_round_trip(rig, udp, udp_port, *lan),
		      "the UDP flow did not work at the start");
	}
	auto unix_flow = rig.connect(through_unix);
	if (!unix_flow || !through_unix.accept_connection()) {
		fail(scenario, "the Unix-domain forward did not connect");
		return;
	}

	/* A host client's connection to a forward: the guest accepts it. It is
	 * bound to the loopback, which is not among the host's addresses. */
	const int client = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in target{};
	target.sin_family = AF_INET;
	target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	target.sin_port = htons(forward.host_port);
	if (client < 0 ||
	    ::connect(client, reinterpret_cast<sockaddr *>(&target),
		      sizeof(target)) != 0) {
		fail(scenario, "the host client could not connect");
		return;
	}
	const auto syn = rig.wait_for(
		[](const Segment &segment) {
			return (segment.flags & tcp_syn) != 0 &&
				(segment.flags & tcp_ack) == 0 &&
				segment.destination_port == forward_guest_port;
		},
		prompt_ms);
	if (!syn) {
		fail(scenario, "the forwarded connection did not reach the guest");
		::close(client);
		return;
	}
	(void)rig.send(tcp_frame(guest_ip, gateway_ip, forward_guest_port,
				 syn->source_port, 5000, syn->seq + 1,
				 tcp_syn | tcp_ack, 65535));
	(void)rig.watch(150);

	/* The path is carried and the host's addresses are not the ones the
	 * sockets are bound to. */
	rig.report_carried();
	rig.advance(uml::network::address_reset_hold +
		    std::chrono::milliseconds{100});

	if (bound) {
		check(scenario, rig.reset_of(*bound, prompt_ms).has_value(),
		      "a connection bound to an address the host does not have "
		      "got no reset");
		check(scenario, server->saw_close(),
		      "the host socket of the reset connection stayed open");
	}
	const auto rest = rig.watch(2 * quiet_ms);
	check(scenario,
	      std::none_of(rest.begin(), rest.end(), [&](const Segment &segment) {
		      return (segment.flags & tcp_rst) != 0 &&
			      (segment.destination_port == unix_flow->guest_port ||
			       segment.destination_port == forward_guest_port);
	      }),
	      "a Unix-domain or a forwarded connection was reset");
	check(scenario, rig.provider.address_reset_calls_for_testing() == 1,
	      "libslirp was not asked exactly once");

	/* The connection a host client made to a forward still carries data to
	 * the guest. */
	const char hello[] = "hello";
	check(scenario,
	      ::send(client, hello, sizeof(hello), 0) ==
		      static_cast<ssize_t>(sizeof(hello)) &&
		      rig.wait_for(
			      [](const Segment &segment) {
				      return segment.payload != 0 &&
					      segment.destination_port ==
						      forward_guest_port;
			      },
			      prompt_ms)
			      .has_value(),
	      "the connection a host client made to a forward stopped working");
	check(scenario, flow_is_alive(rig, *unix_flow, through_unix),
	      "the Unix-domain connection stopped working");
	if (lan)
		check(scenario, udp_round_trip(rig, udp, udp_port, *lan),
		      "the UDP flow stopped working after the reset");
	::close(client);
	if (udp >= 0)
		::close(udp);
}

} // namespace

int main()
{
	lost_address_is_reset_after_the_hold();
	address_back_within_the_hold_resets_nothing();
	address_back_without_a_report_resets_nothing();
	a_plain_outage_resets_nothing();
	an_outage_between_two_passes_starts_the_hold_again();
	an_outage_during_the_read_that_ends_the_hold_resets_nothing();
	an_address_that_goes_missing_at_the_end_of_the_hold_gets_a_hold_of_its_own();
	a_failed_read_at_the_start_of_a_hold_leaves_it_no_first_sample();
	the_loop_needs_no_wake_but_the_reports_and_the_deadline();
	the_table_read_at_the_end_of_the_hold_is_kept_without_allocating();
	a_hold_does_not_outlive_its_instance();
	the_sockets_decide_what_is_reset();
	if (failures != 0) {
		std::fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	std::puts("PASS: a guest connection bound to a lost address is reset "
		  "after the hold, and only then");
	return 0;
}
