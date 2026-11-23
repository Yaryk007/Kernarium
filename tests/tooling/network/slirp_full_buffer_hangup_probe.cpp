/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A host peer that hangs up while the guest is not taking the data SLiRP
 * holds for it must not abort the process, spin the event loop, or lose what
 * the guest is owed. SLiRP reads a host socket into its buffer for the guest
 * only while that buffer is less than half full, so a guest that stops
 * reading leaves the buffer filling or full and the socket unread. The socket
 * is still polled for writing while the guest has data to send, and poll()
 * reports a hang-up whether or not it was asked for. libslirp handed that to
 * soread() on every such pass. Short of full it read the room there was, and
 * XNU keeps received data readable after a reset, so the buffer filled
 * within a few passes; at full soread() had no room and asserted.
 *
 * What a failure looks like depends on the build. A libslirp with assertions
 * (the iOS prefixes) aborts in the reset, reset-pending, fin-then-data, dead,
 * hup-fill and unix-close-full scenarios, and each fails naming the signal.
 * One without them (the macOS prefix) calls recv() on an uninitialised buffer
 * instead. What that returns depends on what the stack held, so the scenarios
 * that expect a reset may pass there; unix-close and unix-close-full catch
 * a connection reset or cut short that should not be, by what the guest is
 * told, and every scenario also checks that the event loop is quiet.
 *
 * Each scenario runs in its own process. The guest closes its window, a host
 * peer sends until SLiRP's buffer for the guest is full (above half full in
 * the two scenarios that say so) and the rest waits in the host socket, and
 * then the host side fails:
 *
 *   reset          the peer resets the connection and the guest then sends;
 *   reset-pending  the guest's own data is stuck in SLiRP when the peer
 *                  resets, so the error is still pending when it is polled;
 *   fin-then-data  the peer closes, as a server does an idle connection, and
 *                  the guest sends twice, the second time into the reset the
 *                  first write provoked;
 *   fin-idle       the peer closes and the guest sends nothing: no hang-up
 *                  is reported, and the guest still gets everything and then
 *                  the FIN once it reads;
 *   dead           with the guest's data stuck, the host socket is shut down
 *                  in both directions: it hangs up with no error pending,
 *                  and the only sign that the peer is gone is its address;
 *   hup-fill       the buffer is above half full, short of full, with more
 *                  waiting in the host socket and the guest's data stuck;
 *                  the peer resets, and the hang-up passes fill the buffer;
 *   unix-close     a Unix-domain forward, as the DNS-over-TCP ones are, and
 *                  no fill: the peer writes and closes in order while the
 *                  guest has not read, and the guest gets all of it and a
 *                  FIN, not a reset;
 *   unix-close-full  the same with hup-fill's state and the guest's data
 *                  stuck: the hang-up passes fill the buffer, there is still
 *                  no reset, and the guest gets everything it is owed, then
 *                  the FIN;
 *   unix-close-writer  the same, but the guest's window is open when the peer
 *                  closes. The guest then writes more than any window holds
 *                  before it reads: every octet is acknowledged and the
 *                  window stays open, with no reset. It reads everything,
 *                  gets the FIN, and its next data is reset.
 *
 * A connection the host lost must reach the guest as a reset and release
 * SLiRP's socket. In every scenario the event loop stays quiet afterwards:
 * a hang-up that is only ignored is reported again on every pass.
 */
#include "slirp_tcp_harness.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/tcp_fsm.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using namespace slirp_harness;

constexpr std::uint16_t guest_port = 40000;
constexpr std::uint32_t guest_iss = 1000;
/* The guest forward the Unix-domain scenarios connect to. */
constexpr std::array<std::uint8_t, 4> forward_ip{10, 0, 2, 100};
constexpr std::uint16_t forward_port = 5000;
/*
 * SLiRP's buffer for the guest holds 128 KiB rounded up to whole 1460-octet
 * segments. A read it asks for, below half full, is trimmed to whole
 * segments of the connection's size: 1440 octets for a connection the guest
 * opened to a host address (a forward to a Unix socket has 1460). Such a
 * read leaves the buffer full to its last octet only if the free space is a
 * whole number of segments. The scenarios that need an exactly full buffer
 * of the first kind get there that way: the host peer sends in two parts,
 * the first leaving the buffer holding what the second needs to fill it, and
 * the probe checks the result. (A hang-up pass reads whatever is free, so
 * hup-fill and the Unix scenarios need no such arithmetic.)
 */
constexpr std::size_t buffer_size = 90 * 1460;
constexpr std::size_t read_size = 1440;
constexpr std::size_t first_part =
	buffer_size % read_size + 20 * read_size;
/* More than the buffer holds, less than the host socket can hold unread. */
constexpr std::size_t flood_bytes = 200000;
/* Above half of the buffer and well short of full. */
constexpr std::size_t half_plus_bytes = 100000;
/* What a Unix-domain peer writes before it closes, with room to spare. */
constexpr std::size_t unix_bytes = 3000;
/* The window SLiRP offers for the guest's data is 65535 until what it holds
 * for the host leaves less room than that; the writer scenario uploads until
 * it is below this, so that the host socket has data to write and hangs up
 * visibly, and the window is still open. */
constexpr std::uint16_t upload_window_limit = 60000;
/* More than a window and more than SLiRP's whole buffer for the host. */
constexpr std::size_t writer_bytes = 300000;
constexpr int quiet_passes = 30;
/* An idle loop finds nothing ready; a spinning one finds something on
 * every pass. */
constexpr std::uint64_t quiet_ready_limit = 3;
/* A scenario that has not finished in this long is stalled, and fails by
 * name. The alarm times the number of scenarios (9 x 20 s) stays under the
 * test's TIMEOUT in SlirpToolingTests.cmake. */
constexpr unsigned scenario_seconds = 20;

enum class Scenario {
	reset,
	reset_pending,
	fin_then_data,
	fin_idle,
	dead,
	hup_fill,
	unix_close,
	unix_close_full,
	unix_close_writer,
};
enum class Seen { absent, required };

struct Expectation {
	const char *name;
	Scenario scenario;
	Seen rst;
	Seen fin;
	/* What the guest has received at the end; zero where that is moot. */
	std::size_t bytes;
	/* Whether SLiRP's host socket is still open at the end. */
	bool socket_open;
};

constexpr std::array<Expectation, 9> expectations{{
	{"reset", Scenario::reset, Seen::required, Seen::absent, 0, false},
	{"reset-pending", Scenario::reset_pending, Seen::required, Seen::absent,
	 0, false},
	{"fin-then-data", Scenario::fin_then_data, Seen::required, Seen::absent,
	 0, false},
	{"fin-idle", Scenario::fin_idle, Seen::absent, Seen::required,
	 flood_bytes, true},
	{"dead", Scenario::dead, Seen::required, Seen::absent, 0, false},
	{"hup-fill", Scenario::hup_fill, Seen::required, Seen::absent, 0, false},
	{"unix-close", Scenario::unix_close, Seen::absent, Seen::required,
	 unix_bytes, true},
	{"unix-close-full", Scenario::unix_close_full, Seen::absent,
	 Seen::required, flood_bytes, true},
	/* The guest's next data after the FIN is reset, which ends the
	 * connection. */
	{"unix-close-writer", Scenario::unix_close_writer, Seen::required,
	 Seen::required, flood_bytes, false},
}};

/* Where a scenario is, for the alarm to name. */
const char *current_scenario = "";
const char *current_stage = "starting";

/* The directory of the Unix-domain sockets, made and removed by the parent so
 * that a scenario that dies leaves nothing behind. */
std::string socket_directory;

void stage(const char *name)
{
	current_stage = name;
}

void stalled(int)
{
	static const char prefix[] = "FAIL ";
	static const char middle[] = ": stalled, no result in time, while: ";
	static const char newline[] = "\n";
	(void)!write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
	(void)!write(STDERR_FILENO, current_scenario,
		     std::strlen(current_scenario));
	(void)!write(STDERR_FILENO, middle, sizeof(middle) - 1);
	(void)!write(STDERR_FILENO, current_stage, std::strlen(current_stage));
	(void)!write(STDERR_FILENO, newline, sizeof(newline) - 1);
	_exit(3);
}

bool wait_for(const std::function<bool()> &condition, int timeout_ms)
{
	const auto deadline = std::chrono::steady_clock::now() +
		std::chrono::milliseconds{timeout_ms};
	while (!condition()) {
		if (std::chrono::steady_clock::now() >= deadline)
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds{5});
	}
	return true;
}

/* Waits for a state of the host sockets, which no event-loop pass is needed
 * to reach. */
bool wait_state(const char *name, const std::function<bool()> &condition)
{
	stage(name);
	if (wait_for(condition, 5000))
		return true;
	fprintf(stderr, "the host sockets never reached the state: %s\n", name);
	return false;
}

std::size_t unread_bytes(int descriptor)
{
	int count = 0;
	return ioctl(descriptor, FIONREAD, &count) == 0 && count > 0 ?
		static_cast<std::size_t>(count) : 0;
}

/* Whether the descriptor reports a hang-up or an error. poll() does so
 * whether it is asked or not, and does not take a pending error. */
bool hung_up(int descriptor)
{
	pollfd entry{descriptor, POLLOUT, 0};
	return poll(&entry, 1, 0) == 1 &&
		(entry.revents & (POLLHUP | POLLERR)) != 0;
}

/* Whether the peer's FIN has reached the TCP socket. */
bool peer_closed(int descriptor)
{
	tcp_connection_info information{};
	socklen_t length = sizeof(information);
	return getsockopt(descriptor, IPPROTO_TCP, TCP_CONNECTION_INFO,
			  &information, &length) == 0 &&
		information.tcpi_state == TCPS_CLOSE_WAIT;
}

/* SLiRP's end of the loopback connection is the only socket of this process
 * whose peer is the listener's port. */
int find_slirp_socket(std::uint16_t host_port)
{
	for (int descriptor = 3; descriptor < 1024; ++descriptor) {
		sockaddr_in peer{};
		socklen_t length = sizeof(peer);
		if (getpeername(descriptor, reinterpret_cast<sockaddr *>(&peer),
				&length) == 0 &&
		    peer.sin_family == AF_INET && ntohs(peer.sin_port) == host_port)
			return descriptor;
	}
	return -1;
}

/* SLiRP's end of a Unix-domain connection is the only socket of this process
 * whose peer is the listener's path. */
int find_slirp_unix_socket(const std::string &path)
{
	for (int descriptor = 3; descriptor < 1024; ++descriptor) {
		sockaddr_un peer{};
		socklen_t length = sizeof(peer);
		if (getpeername(descriptor, reinterpret_cast<sockaddr *>(&peer),
				&length) == 0 &&
		    peer.sun_family == AF_UNIX && path == peer.sun_path)
			return descriptor;
	}
	return -1;
}

std::uint16_t local_port_of(int descriptor)
{
	sockaddr_in local{};
	socklen_t length = sizeof(local);
	if (getsockname(descriptor, reinterpret_cast<sockaddr *>(&local),
			&length) != 0 || local.sin_family != AF_INET)
		return 0;
	return ntohs(local.sin_port);
}

bool socket_open_on(std::uint16_t port)
{
	for (int descriptor = 3; descriptor < 1024; ++descriptor)
		if (local_port_of(descriptor) == port)
			return true;
	return false;
}

/* The host side of the connection: a listener the probe owns, standing in for
 * the Internet or, for the Unix-domain scenarios, for the resolver behind a
 * guest forward, and the peer socket it accepted. SLiRP's own socket is found
 * by scanning and stays SLiRP's. */
struct Host {
	int listener{-1};
	int peer{-1};
	int slirp{-1};
	/* What the guest connects to. */
	std::array<std::uint8_t, 4> server{gateway_ip};
	std::uint16_t port{};
	std::uint16_t slirp_port{};
	/* The listener's path, for a Unix-domain forward. */
	std::string path;

	~Host()
	{
		if (listener >= 0)
			close(listener);
		if (peer >= 0)
			close(peer);
	}

	bool is_unix() const
	{
		return !path.empty();
	}

	bool listen_on_loopback(int receive_buffer)
	{
		listener = socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		socklen_t length = sizeof(address);
		if (listener < 0 ||
		    (receive_buffer != 0 &&
		     setsockopt(listener, SOL_SOCKET, SO_RCVBUF, &receive_buffer,
				sizeof(receive_buffer)) != 0) ||
		    bind(listener, reinterpret_cast<const sockaddr *>(&address),
			 sizeof(address)) != 0 ||
		    listen(listener, 1) != 0 ||
		    getsockname(listener, reinterpret_cast<sockaddr *>(&address),
				&length) != 0)
			return false;
		port = ntohs(address.sin_port);
		return true;
	}

	/* A guest forward to a Unix-domain socket, which SLiRP connects to
	 * when the guest connects to the forward's address. */
	bool listen_on_unix(Network &network, const char *name)
	{
		path = socket_directory + "/" + name;
		sockaddr_un address{};
		address.sun_family = AF_UNIX;
		if (path.size() >= sizeof(address.sun_path))
			return false;
		std::memcpy(address.sun_path, path.c_str(), path.size());
		listener = socket(AF_UNIX, SOCK_STREAM, 0);
		in_addr forward{};
		std::memcpy(&forward, forward_ip.data(), forward_ip.size());
		if (listener < 0 ||
		    bind(listener, reinterpret_cast<const sockaddr *>(&address),
			 sizeof(address)) != 0 ||
		    listen(listener, 1) != 0 ||
		    slirp_add_unix(network.slirp(), path.c_str(), &forward,
				   forward_port) != 0)
			return false;
		server = forward_ip;
		port = forward_port;
		return true;
	}

	void accept_if_pending()
	{
		pollfd entry{listener, POLLIN, 0};
		if (peer < 0 && poll(&entry, 1, 0) == 1)
			peer = accept(listener, nullptr, nullptr);
	}

	/* A close that sends a reset instead of a FIN. */
	void reset()
	{
		const linger abort_close{1, 0};
		(void)setsockopt(peer, SOL_SOCKET, SO_LINGER, &abort_close,
				 sizeof(abort_close));
		close(peer);
		peer = -1;
	}

	void close_peer()
	{
		close(peer);
		peer = -1;
	}
};

/* The guest as the probe writes it: what SLiRP has sent it so far, and the
 * sequence numbers to answer with. Its receive window is whatever the
 * probe says, zero until it reads. */
struct Guest {
	Network &network;
	std::array<std::uint8_t, 4> server{gateway_ip};
	std::uint16_t host_port{};
	std::uint32_t next{guest_iss + 1};
	std::uint32_t receive_next{};
	std::uint32_t received{};
	/* SLiRP's latest acknowledgement of the guest's data, and the window
	 * it offered. */
	std::uint32_t slirp_ack{};
	std::uint16_t slirp_window{};
	bool syn{};
	bool fin{};
	bool rst{};

	void observe()
	{
		while (!network.frames().empty()) {
			const auto segment = parse_tcp(network.frames().front());
			network.frames().pop_front();
			if (!segment || segment->destination_port != guest_port ||
			    segment->source_port != host_port)
				continue;
			if ((segment->flags & tcp_syn) != 0) {
				syn = true;
				receive_next = segment->seq + 1;
			} else if (syn && segment->seq == receive_next) {
				receive_next += static_cast<std::uint32_t>(
					segment->payload);
				received += static_cast<std::uint32_t>(
					segment->payload);
				if ((segment->flags & tcp_fin) != 0) {
					fin = true;
					++receive_next;
				}
			}
			if ((segment->flags & tcp_rst) != 0)
				rst = true;
			if ((segment->flags & tcp_ack) != 0) {
				slirp_ack = segment->ack;
				slirp_window = segment->window;
			}
		}
	}

	void pump(int wait_ms)
	{
		network.pump(1, wait_ms);
		observe();
	}

	void acknowledge(std::uint16_t window)
	{
		network.input(tcp_frame(guest_ip, server, guest_port, host_port,
					next, receive_next, tcp_ack, window));
	}

	void send(std::size_t size)
	{
		network.input(tcp_frame(guest_ip, server, guest_port, host_port,
					next, receive_next, tcp_ack | tcp_psh,
					0, size));
		next += static_cast<std::uint32_t>(size);
		observe();
	}
};

/* Connects the guest to the host through SLiRP and closes its window, so that
 * nothing SLiRP reads from the host reaches it. */
bool connect_guest(Guest &guest, Host &host)
{
	guest.server = host.server;
	guest.host_port = host.port;
	guest.network.input(tcp_frame(guest_ip, guest.server, guest_port,
				      host.port, guest_iss, 0, tcp_syn, 65535));
	for (int pass = 0; pass < 300 && !guest.syn; ++pass)
		guest.pump(10);
	if (!guest.syn) {
		fprintf(stderr, "SLiRP did not answer the guest's SYN\n");
		return false;
	}
	/* A Unix-domain forward is connected only once the guest has completed
	 * the handshake. */
	guest.acknowledge(0);
	for (int pass = 0; pass < 300 && host.peer < 0; ++pass) {
		guest.pump(10);
		host.accept_if_pending();
	}
	if (host.peer < 0) {
		fprintf(stderr, "SLiRP did not connect to the host\n");
		return false;
	}
	guest.pump(10);
	host.slirp = host.is_unix() ? find_slirp_unix_socket(host.path) :
		find_slirp_socket(host.port);
	if (host.slirp < 0) {
		fprintf(stderr, "could not find SLiRP's host socket\n");
		return false;
	}
	host.slirp_port = local_port_of(host.slirp);
	return true;
}

bool send_all(int peer, std::size_t size)
{
	const std::vector<char> data(size, 'x');
	std::size_t sent = 0;
	const auto deadline = std::chrono::steady_clock::now() +
		std::chrono::seconds{5};
	while (sent < size && std::chrono::steady_clock::now() < deadline) {
		const auto count = send(peer, data.data() + sent, size - sent,
					MSG_DONTWAIT);
		if (count > 0) {
			sent += static_cast<std::size_t>(count);
		} else if (count < 0 && errno == EAGAIN) {
			pollfd entry{peer, POLLOUT, 0};
			(void)poll(&entry, 1, 50);
		} else {
			break;
		}
	}
	return sent == size;
}

/* Room for what the peer sends to wait in SLiRP's socket unread. */
void enlarge_buffers(const Host &host)
{
	const int large = 1 << 20;
	(void)setsockopt(host.slirp, SOL_SOCKET, SO_RCVBUF, &large,
			 sizeof(large));
	(void)setsockopt(host.peer, SOL_SOCKET, SO_SNDBUF, &large,
			 sizeof(large));
}

/* The peer sends until SLiRP has filled its buffer for the guest to the last
 * octet and the rest of the data waits unread in SLiRP's socket. */
bool fill_buffer(Guest &guest, Host &host)
{
	stage("filling SLiRP's buffer for the guest");
	enlarge_buffers(host);
	/* Each part is all there before SLiRP reads, so that it reads the part
	 * it asks for. */
	const std::size_t second_part = flood_bytes - first_part;
	if (!send_all(host.peer, first_part) ||
	    !wait_for([&] { return unread_bytes(host.slirp) == first_part; },
		      5000)) {
		fprintf(stderr, "the first part did not reach SLiRP's socket\n");
		return false;
	}
	for (int pass = 0; pass < 10 && unread_bytes(host.slirp) != 0; ++pass)
		guest.pump(20);
	if (unread_bytes(host.slirp) != 0 || !send_all(host.peer, second_part) ||
	    !wait_for([&] { return unread_bytes(host.slirp) == second_part; },
		      5000)) {
		fprintf(stderr, "SLiRP did not read the first part, or the second "
			"did not reach it\n");
		return false;
	}
	for (int pass = 0; pass < 10; ++pass)
		guest.pump(20);
	const std::size_t taken = flood_bytes - unread_bytes(host.slirp);
	if (taken != buffer_size) {
		fprintf(stderr, "SLiRP took %zu of %zu bytes, not the %zu its "
			"buffer holds\n", taken, flood_bytes, buffer_size);
		return false;
	}
	return true;
}

/* The peer sends a chunk SLiRP's buffer for the guest takes above half and
 * short of full, and then another that stays unread in SLiRP's socket. */
bool fill_above_half(Guest &guest, Host &host)
{
	stage("filling SLiRP's buffer for the guest above half");
	enlarge_buffers(host);
	if (!send_all(host.peer, half_plus_bytes) ||
	    !wait_for([&] { return unread_bytes(host.slirp) == half_plus_bytes; },
		      5000)) {
		fprintf(stderr, "the first chunk did not reach SLiRP's socket\n");
		return false;
	}
	for (int pass = 0; pass < 10 && unread_bytes(host.slirp) != 0; ++pass)
		guest.pump(20);
	if (unread_bytes(host.slirp) != 0 ||
	    !send_all(host.peer, half_plus_bytes) ||
	    !wait_for([&] { return unread_bytes(host.slirp) == half_plus_bytes; },
		      5000)) {
		fprintf(stderr, "SLiRP did not read the first chunk, or the "
			"second did not reach it\n");
		return false;
	}
	for (int pass = 0; pass < 10; ++pass)
		guest.pump(20);
	if (unread_bytes(host.slirp) != half_plus_bytes) {
		fprintf(stderr, "SLiRP read %zu of the second chunk while its "
			"buffer was above half full\n",
			half_plus_bytes - unread_bytes(host.slirp));
		return false;
	}
	return true;
}

/* The guest sends within SLiRP's window until the host socket and SLiRP's
 * buffer for the guest's data are full and the window stays closed, so the
 * socket is polled for writing from here on. */
bool stick_upload(Guest &guest)
{
	stage("sticking the guest's upload");
	int closed = 0;
	for (int pass = 0; pass < 4000 && closed < 50; ++pass) {
		const std::uint32_t edge = guest.slirp_ack + guest.slirp_window;
		while (static_cast<std::int32_t>(edge - guest.next) > 0) {
			const std::uint32_t room = edge - guest.next;
			guest.send(room < 1460 ? room : 1460);
		}
		guest.pump(1);
		closed = guest.slirp_window == 0 && guest.slirp_ack == guest.next ?
			closed + 1 : 0;
	}
	if (closed < 50) {
		fprintf(stderr, "SLiRP never closed the window for the guest's "
			"data\n");
		return false;
	}
	return true;
}

/* Passes of an event loop that has nothing to do must not find a socket
 * ready. */
std::uint64_t ready_while_quiet(Guest &guest)
{
	stage("watching the quiet event loop");
	const auto before = guest.network.ready_polls();
	for (int pass = 0; pass < quiet_passes; ++pass)
		guest.pump(20);
	return guest.network.ready_polls() - before;
}

/* The guest opens its window and reads until it has all the data, and the
 * FIN too if the host closed. */
void read_everything(Guest &guest, std::size_t wanted, bool until_fin)
{
	stage("the guest reading what it is owed");
	for (int pass = 0; pass < 3000; ++pass) {
		const auto before = guest.receive_next;
		guest.pump(5);
		if (guest.receive_next != before || pass == 0)
			guest.acknowledge(65535);
		if (guest.received >= wanted && (guest.fin || !until_fin))
			return;
	}
}

struct Outcome {
	bool rst{};
	bool fin{};
	std::size_t bytes{};
	bool socket_open{};
	std::uint64_t ready{};
	/* What went wrong on the way, where the end state does not say. */
	std::string problem;
};

/* The guest sends until SLiRP holds enough of its data for the host that
 * the host socket is polled for writing, with the window still open. */
bool partial_upload(Guest &guest)
{
	stage("the guest's upload before the peer closes");
	for (int pass = 0;
	     pass < 400 && guest.slirp_window > upload_window_limit; ++pass) {
		const std::uint32_t edge = guest.slirp_ack + guest.slirp_window;
		const std::int32_t room =
			static_cast<std::int32_t>(edge - guest.next);
		if (room > 0)
			guest.send(room < 1460 ?
				   static_cast<std::size_t>(room) : 1460);
		guest.pump(2);
	}
	if (guest.slirp_window > upload_window_limit) {
		fprintf(stderr, "SLiRP's window for the guest's data stayed "
			"at %u\n", static_cast<unsigned>(guest.slirp_window));
		return false;
	}
	return true;
}

/* The peer is gone and SLiRP no longer writes what the guest sends, which
 * must still be acknowledged and not hold the guest on a window that never
 * opens. The guest writes more than any window before it reads. Then it
 * reads everything and gets the FIN, and what it sends next is reset. */
void write_before_reading(Guest &guest, std::size_t wanted, Outcome &outcome)
{
	stage("the guest writing after the peer's close");
	std::size_t sent = 0;
	std::uint16_t lowest_window = 65535;
	for (int pass = 0; pass < 3000 && sent < writer_bytes; ++pass) {
		const std::uint32_t edge = guest.slirp_ack + guest.slirp_window;
		const std::int32_t room =
			static_cast<std::int32_t>(edge - guest.next);
		if (room > 0) {
			std::size_t size = room < 1460 ?
				static_cast<std::size_t>(room) : 1460;
			if (size > writer_bytes - sent)
				size = writer_bytes - sent;
			guest.send(size);
			sent += size;
		}
		guest.pump(2);
		if (guest.slirp_window < lowest_window)
			lowest_window = guest.slirp_window;
	}
	for (int pass = 0; pass < 20; ++pass)
		guest.pump(5);
	if (sent < writer_bytes) {
		outcome.problem = "the guest was held on a closed window after " +
			std::to_string(sent) + " of " +
			std::to_string(writer_bytes) + " octets";
		return;
	}
	if (lowest_window == 0) {
		outcome.problem = "the window fell to 0 while the guest wrote";
		return;
	}
	if (guest.slirp_ack != guest.next) {
		outcome.problem = "SLiRP left " +
			std::to_string(guest.next - guest.slirp_ack) +
			" octets of the guest's data unacknowledged";
		return;
	}
	if (guest.rst) {
		outcome.problem = "the guest got a RST while it had not read yet";
		return;
	}
	read_everything(guest, wanted, true);
	if (guest.rst) {
		outcome.problem = "the guest got a RST before its FIN was read";
		return;
	}
	/* The socket has no user any more: more data from the guest ends the
	 * connection with a reset. */
	stage("the guest writing after the FIN");
	guest.send(100);
	for (int pass = 0; pass < 50 && !guest.rst; ++pass)
		guest.pump(20);
}

bool connection_lost(Scenario scenario)
{
	return scenario != Scenario::fin_idle &&
		scenario != Scenario::unix_close &&
		scenario != Scenario::unix_close_full;
}

bool run_scenario(const Expectation &expected, Outcome &outcome)
{
	const Scenario scenario = expected.scenario;
	Network network;
	if (network.slirp() == nullptr || !network.resolve()) {
		fprintf(stderr, "SLiRP did not start or answer ARP\n");
		return false;
	}
	const bool unix_forward = scenario == Scenario::unix_close ||
		scenario == Scenario::unix_close_full ||
		scenario == Scenario::unix_close_writer;
	const bool stuck = scenario == Scenario::reset_pending ||
		scenario == Scenario::dead || scenario == Scenario::hup_fill ||
		scenario == Scenario::unix_close_full;
	Host host;
	stage("opening the host listener");
	/* A peer that never reads and has a small buffer stops taking the
	 * guest's data soon. */
	if (unix_forward ? !host.listen_on_unix(network, expected.name) :
			   !host.listen_on_loopback(stuck ? 4096 : 0)) {
		fprintf(stderr, "could not listen on the host\n");
		return false;
	}
	Guest guest{network};
	stage("connecting the guest");
	if (!connect_guest(guest, host))
		return false;
	if (scenario != Scenario::unix_close) {
		const bool filled = scenario == Scenario::hup_fill ||
			scenario == Scenario::unix_close_full ||
			scenario == Scenario::unix_close_writer ?
			fill_above_half(guest, host) : fill_buffer(guest, host);
		if (!filled)
			return false;
	}
	if (stuck && !stick_upload(guest))
		return false;
	if (scenario == Scenario::unix_close_writer && !partial_upload(guest))
		return false;

	stage("failing the host side");
	switch (scenario) {
	case Scenario::reset:
	case Scenario::hup_fill:
		host.reset();
		if (!wait_state("the peer's reset has reached SLiRP's socket",
				[&] { return hung_up(host.slirp); }))
			return false;
		if (scenario == Scenario::reset)
			guest.send(100);
		break;
	case Scenario::reset_pending:
		host.reset();
		if (!wait_state("the peer's reset has reached SLiRP's socket",
				[&] { return hung_up(host.slirp); }))
			return false;
		break;
	case Scenario::fin_then_data:
		host.close_peer();
		if (!wait_state("the peer's FIN has reached SLiRP's socket",
				[&] { return peer_closed(host.slirp); }))
			return false;
		guest.send(100);
		if (!wait_state("the reset the first write provoked has "
				"reached SLiRP's socket",
				[&] { return hung_up(host.slirp); }))
			return false;
		guest.send(100);
		break;
	case Scenario::fin_idle:
		host.close_peer();
		if (!wait_state("the peer's FIN has reached SLiRP's socket",
				[&] { return peer_closed(host.slirp); }))
			return false;
		break;
	case Scenario::dead:
		if (shutdown(host.slirp, SHUT_RDWR) != 0) {
			fprintf(stderr, "could not shut SLiRP's socket down\n");
			return false;
		}
		if (!wait_state("SLiRP's socket reports the hang-up",
				[&] { return hung_up(host.slirp); }))
			return false;
		break;
	case Scenario::unix_close:
		/* A resolver answers and closes, as it does a stream idle
		 * for 10 s. */
		if (!send_all(host.peer, unix_bytes)) {
			fprintf(stderr, "the peer could not write\n");
			return false;
		}
		host.close_peer();
		break;
	case Scenario::unix_close_full:
	case Scenario::unix_close_writer:
		host.close_peer();
		if (!wait_state("the peer's close has reached SLiRP's socket",
				[&] { return hung_up(host.slirp); }))
			return false;
		break;
	}
	/* A lost connection is reset toward the guest soon; the others stay as
	 * they are. */
	const bool lost = connection_lost(scenario);
	stage(lost ? "waiting for the reset" : "waiting for a reset that must "
	      "not come");
	for (int pass = 0; pass < (lost ? 50 : 5) && !guest.rst; ++pass)
		guest.pump(lost ? 100 : 20);
	outcome.ready = ready_while_quiet(guest);
	if (scenario == Scenario::unix_close_writer)
		write_before_reading(guest, expected.bytes, outcome);
	else if (!lost)
		read_everything(guest, expected.bytes, true);
	outcome.rst = guest.rst;
	outcome.fin = guest.fin;
	outcome.bytes = guest.received;
	stage("looking for SLiRP's socket");
	outcome.socket_open = unix_forward ? fcntl(host.slirp, F_GETFD) != -1 :
		socket_open_on(host.slirp_port);
	return true;
}

bool seen_as_expected(Seen expected, bool seen)
{
	return (expected == Seen::required) == seen;
}

/* Compares what the guest was told and what SLiRP kept with the table. */
bool end_state_as_expected(const Expectation &expected, const Outcome &outcome)
{
	bool passed = true;
	if (!seen_as_expected(expected.rst, outcome.rst)) {
		fprintf(stderr, "FAIL %s: the guest got %s\n", expected.name,
			expected.rst == Seen::required ? "no RST" : "a RST");
		passed = false;
	}
	if (!seen_as_expected(expected.fin, outcome.fin)) {
		fprintf(stderr, "FAIL %s: the guest got %s\n", expected.name,
			expected.fin == Seen::required ? "no FIN" : "a FIN");
		passed = false;
	}
	if (expected.bytes != 0 && outcome.bytes != expected.bytes) {
		fprintf(stderr, "FAIL %s: the guest read %zu bytes, expected "
			"%zu\n", expected.name, outcome.bytes, expected.bytes);
		passed = false;
	}
	if (outcome.socket_open != expected.socket_open) {
		fprintf(stderr, "FAIL %s: SLiRP's host socket is %s\n",
			expected.name,
			outcome.socket_open ? "still open" : "closed");
		passed = false;
	}
	return passed;
}

/* One scenario in a child process, so that an abort in libslirp is a failure
 * of that scenario and the others still run. */
bool run_in_child(const Expectation &expected)
{
	fflush(stdout);
	fflush(stderr);
	const pid_t child = fork();
	if (child < 0) {
		fprintf(stderr, "FAIL %s: could not fork\n", expected.name);
		return false;
	}
	if (child == 0) {
		current_scenario = expected.name;
		(void)signal(SIGALRM, stalled);
		alarm(scenario_seconds);
		Outcome outcome;
		if (!run_scenario(expected, outcome)) {
			fprintf(stderr, "FAIL %s: the scenario did not start, or "
				"never reached its state\n", expected.name);
			_exit(2);
		}
		printf("%s: rst_to_guest=%d fin_to_guest=%d bytes_to_guest=%zu "
		       "socket_open=%d ready_polls=%llu\n",
		       expected.name, outcome.rst ? 1 : 0, outcome.fin ? 1 : 0,
		       outcome.bytes, outcome.socket_open ? 1 : 0,
		       static_cast<unsigned long long>(outcome.ready));
		bool passed = true;
		if (!outcome.problem.empty()) {
			fprintf(stderr, "FAIL %s: %s\n", expected.name,
				outcome.problem.c_str());
			passed = false;
		}
		if (outcome.ready > quiet_ready_limit) {
			fprintf(stderr, "FAIL %s: %llu of %d passes found a "
				"descriptor ready, the loop is spinning\n",
				expected.name,
				static_cast<unsigned long long>(outcome.ready),
				quiet_passes);
			passed = false;
		}
		/* After a failure on the way the end state says nothing more. */
		if (outcome.problem.empty() &&
		    !end_state_as_expected(expected, outcome))
			passed = false;
		fflush(stdout);
		fflush(stderr);
		_exit(passed ? 0 : 1);
	}
	int status = 0;
	if (waitpid(child, &status, 0) != child) {
		fprintf(stderr, "FAIL %s: lost the scenario's process\n",
			expected.name);
		return false;
	}
	if (WIFSIGNALED(status)) {
		fprintf(stderr, "FAIL %s: libslirp died on signal %d\n",
			expected.name, WTERMSIG(status));
		return false;
	}
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace

int main(void)
{
	/* libslirp writes its host sockets with plain send(). */
	(void)signal(SIGPIPE, SIG_IGN);
	/* A Unix-domain path is short, so the directory is under /tmp. */
	char directory[] = "/tmp/uml-slirp-hangup.XXXXXX";
	if (mkdtemp(directory) == nullptr) {
		fprintf(stderr, "FAIL: no scratch directory for the sockets\n");
		return 1;
	}
	socket_directory = directory;
	bool passed = true;
	for (const auto &expected : expectations)
		passed = run_in_child(expected) && passed;
	std::error_code ignored;
	std::filesystem::remove_all(socket_directory, ignored);
	if (!passed)
		return 1;
	printf("PASS: a host socket that hangs up while SLiRP's buffer for the "
	       "guest is filling or full is reset toward the guest and closed, "
	       "or left alone when its close was orderly, and never aborts, "
	       "loses what the guest is owed or spins the event loop\n");
	return 0;
}
