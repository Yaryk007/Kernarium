/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A host socket that dies in both directions while libslirp still holds the
 * guest's connection must not keep the event loop busy. XNU reports a hang-up
 * at once for a socket polled only for writing, and libslirp read it as data
 * to receive: soread() met the end of file it had already handled, and the
 * pass found the socket ready again, for as long as the connection lived and
 * on one core.
 *
 * The probe runs one forwarded connection per scenario. A host client
 * connects to a forward, the probe, as the guest, accepts the connection,
 * and the host side then fails in one way:
 *
 *   dead        SLiRP's own socket is shut down both ways, with no error
 *               pending, while the guest's data waits to be sent;
 *   deadlate    the same, but the guest writes only a second later;
 *   reset       the client resets the connection;
 *   fin         the client half-closes and stays;
 *   closereuse  the client closes a keep-alive connection, which the guest
 *               then reuses.
 *
 * A dead connection must reach the guest as a reset and release SLiRP's
 * socket. The three others keep the answer they always had. In every
 * scenario the event loop's passes in a window long after the failure stay
 * few: a spin makes thousands per second.
 */
#include "provider.hpp"
#include "slirp_tcp_harness.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace {

constexpr std::uint16_t guest_port = 7;
/* How long after the host failure the guest writes, in deadlate. */
constexpr auto late_write = std::chrono::milliseconds{1000};
/* The window in which the event loop's passes are counted opens this long
 * after the host failure, once every answer has been given. */
constexpr auto settle = std::chrono::milliseconds{2000};
constexpr auto window = std::chrono::milliseconds{1000};
/* An idle loop wakes for its timers a few times a second. */
constexpr std::uint64_t window_pass_limit = 100;

enum class Mode { dead, deadlate, reset, fin, closereuse };
enum class Seen { any, absent, required };

struct Expectation {
	const char *name;
	Mode mode;
	/* What the guest receives, and whether SLiRP's host socket is still
	 * open at the end. */
	Seen fin;
	Seen rst;
	bool socket_open;
};

constexpr std::array<Expectation, 5> expectations{{
	{"dead", Mode::dead, Seen::any, Seen::required, false},
	{"deadlate", Mode::deadlate, Seen::any, Seen::required, false},
	{"reset", Mode::reset, Seen::absent, Seen::required, false},
	{"fin", Mode::fin, Seen::required, Seen::absent, true},
	{"closereuse", Mode::closereuse, Seen::required, Seen::required, false},
}};

struct Outcome {
	int fins{};
	int resets{};
	bool socket_open{};
	std::uint64_t window_passes{};
};

std::uint16_t unused_loopback_port()
{
	const int descriptor = socket(AF_INET, SOCK_STREAM, 0);
	if (descriptor < 0)
		return 0;
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t length = sizeof(address);
	std::uint16_t port = 0;
	if (bind(descriptor, reinterpret_cast<const sockaddr *>(&address),
		 sizeof(address)) == 0 &&
	    getsockname(descriptor, reinterpret_cast<sockaddr *>(&address),
			&length) == 0)
		port = ntohs(address.sin_port);
	close(descriptor);
	return port;
}

bool wait_readable(int descriptor, int timeout_ms)
{
	pollfd entry{descriptor, POLLIN, 0};
	return poll(&entry, 1, timeout_ms) == 1 && (entry.revents & POLLIN) != 0;
}

/* Frames SLiRP has for the guest, as the harness parses them. */
std::vector<std::vector<std::uint8_t>> drain(
	uml::network::SlirpProvider &provider)
{
	std::vector<std::vector<std::uint8_t>> frames;
	std::array<std::byte, uml::network::frame_size> buffer{};
	for (;;) {
		const int length = provider.read(buffer);
		if (length <= 0)
			return frames;
		const auto *bytes =
			reinterpret_cast<const std::uint8_t *>(buffer.data());
		frames.emplace_back(bytes, bytes + length);
	}
}

void send_to_slirp(uml::network::SlirpProvider &provider,
		   const std::vector<std::uint8_t> &frame)
{
	provider.write(std::as_bytes(std::span{frame}));
}

/* A descriptor of the probe's own process with this local port, other than
 * `skip`. SLiRP's accepted socket for a forward has the forward's port, which
 * the listening socket has too, and a socket shut down both ways has no peer
 * address to tell it by. */
int find_socket_on_port(std::uint16_t port, int skip)
{
	for (int descriptor = 3; descriptor < 4096; ++descriptor) {
		sockaddr_in local{};
		socklen_t length = sizeof(local);
		if (descriptor != skip &&
		    getsockname(descriptor, reinterpret_cast<sockaddr *>(&local),
				&length) == 0 &&
		    local.sin_family == AF_INET && ntohs(local.sin_port) == port)
			return descriptor;
	}
	return -1;
}

/* Collects what SLiRP sends the guest on the connection until a deadline. */
void watch(uml::network::SlirpProvider &provider, std::uint16_t slirp_port,
	   std::chrono::steady_clock::time_point deadline, Outcome &outcome)
{
	while (std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds{50});
		for (const auto &frame : drain(provider)) {
			const auto segment = slirp_harness::parse_tcp(frame);
			if (!segment || segment->destination_port != guest_port ||
			    segment->source_port != slirp_port)
				continue;
			if ((segment->flags & slirp_harness::tcp_fin) != 0)
				++outcome.fins;
			if ((segment->flags & slirp_harness::tcp_rst) != 0)
				++outcome.resets;
		}
	}
}

/* Sets up one connection, fails it as the mode says, and records what the
 * guest saw and how busy the event loop was. False if the setup failed. */
bool run_scenario(Mode mode, Outcome &outcome)
{
	uml::network::SlirpProvider provider;
	uml::network::HostForward forward{};
	forward.host_port = unused_loopback_port();
	forward.guest_port = guest_port;
	if (forward.host_port == 0 || provider.add_forward(forward) != 0) {
		fprintf(stderr, "could not register a loopback forward\n");
		return false;
	}
	std::array<unsigned char, 6> mac{};
	unsigned int mtu = 0;
	if (provider.open(mac, mtu) != 0) {
		fprintf(stderr, "the provider did not open\n");
		return false;
	}

	/* The guest asks for the gateway, so SLiRP knows where to send a SYN. */
	std::vector<std::uint8_t> request(42, 0);
	for (std::size_t index = 0; index < 6; ++index) {
		request[index] = 0xff;
		request[6 + index] = slirp_harness::guest_mac[index];
		request[22 + index] = slirp_harness::guest_mac[index];
	}
	slirp_harness::put16(request, 12, 0x0806);
	slirp_harness::put16(request, 14, 1);
	slirp_harness::put16(request, 16, 0x0800);
	request[18] = 6;
	request[19] = 4;
	slirp_harness::put16(request, 20, 1);
	for (std::size_t index = 0; index < 4; ++index) {
		request[28 + index] = slirp_harness::guest_ip[index];
		request[38 + index] = slirp_harness::gateway_ip[index];
	}
	send_to_slirp(provider, request);
	bool resolved = false;
	for (int attempt = 0; !resolved && attempt < 20; ++attempt) {
		if (!wait_readable(provider.irq_fd(), 100))
			continue;
		for (const auto &frame : drain(provider))
			resolved |= frame.size() >= 42 &&
				slirp_harness::get16(frame, 12) == 0x0806;
	}
	if (!resolved) {
		fprintf(stderr, "SLiRP did not answer the guest's ARP request\n");
		return false;
	}

	const int listener = find_socket_on_port(forward.host_port, -1);
	int client = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons(forward.host_port);
	if (client < 0 ||
	    connect(client, reinterpret_cast<const sockaddr *>(&address),
		    sizeof(address)) != 0) {
		fprintf(stderr, "the host client could not connect\n");
		return false;
	}

	std::optional<slirp_harness::Segment> syn;
	for (int attempt = 0; !syn && attempt < 30; ++attempt) {
		if (!wait_readable(provider.irq_fd(), 100))
			continue;
		for (const auto &frame : drain(provider)) {
			const auto segment = slirp_harness::parse_tcp(frame);
			if (segment && (segment->flags & slirp_harness::tcp_syn) != 0 &&
			    segment->destination_port == guest_port)
				syn = segment;
		}
	}
	if (!syn) {
		fprintf(stderr, "the connection did not reach the guest\n");
		close(client);
		return false;
	}
	const std::uint16_t slirp_port = syn->source_port;
	const std::uint32_t guest_seq = 5000;
	std::uint32_t next_seq = guest_seq + 1;
	const auto guest_data = [&](std::uint32_t segments) {
		for (std::uint32_t segment = 0; segment != segments; ++segment) {
			send_to_slirp(provider, slirp_harness::tcp_frame(
				slirp_harness::guest_ip, slirp_harness::gateway_ip,
				guest_port, slirp_port, next_seq, syn->seq + 1,
				slirp_harness::tcp_psh | slirp_harness::tcp_ack,
				65535, 400));
			next_seq += 400;
		}
	};
	send_to_slirp(provider, slirp_harness::tcp_frame(
		slirp_harness::guest_ip, slirp_harness::gateway_ip, guest_port,
		slirp_port, guest_seq, syn->seq + 1,
		slirp_harness::tcp_syn | slirp_harness::tcp_ack, 65535));
	std::this_thread::sleep_for(std::chrono::milliseconds{300});
	(void)drain(provider);

	const int slirp_socket = find_socket_on_port(forward.host_port, listener);
	if (slirp_socket < 0) {
		fprintf(stderr, "could not find SLiRP's host socket\n");
		close(client);
		return false;
	}

	if (mode == Mode::closereuse) {
		/* The client closes a keep-alive connection, and its FIN reaches
		 * the guest, which then sends a new request on it. The closed
		 * client answers that data with a reset. */
		close(client);
		client = -1;
		std::this_thread::sleep_for(std::chrono::milliseconds{300});
		guest_data(1);
		std::this_thread::sleep_for(std::chrono::milliseconds{300});
	}

	/* Hold the provider between passes, fail the host side, give it the
	 * guest's data, and let it run. */
	provider.pause_transmit_for_testing();
	if (!provider.wait_for_transmit_pause_for_testing()) {
		fprintf(stderr, "the provider did not pause\n");
		if (client >= 0)
			close(client);
		return false;
	}
	switch (mode) {
	case Mode::dead:
	case Mode::deadlate:
		(void)shutdown(slirp_socket, SHUT_RDWR);
		break;
	case Mode::reset: {
		linger reset{1, 0};
		(void)setsockopt(client, SOL_SOCKET, SO_LINGER, &reset,
				 sizeof(reset));
		close(client);
		client = -1;
		break;
	}
	case Mode::fin:
		(void)shutdown(client, SHUT_WR);
		break;
	case Mode::closereuse:
		break;
	}
	std::this_thread::sleep_for(std::chrono::milliseconds{100});
	if (mode != Mode::deadlate)
		guest_data(4);
	provider.resume_transmit_for_testing();

	const auto started = std::chrono::steady_clock::now();
	if (mode == Mode::deadlate) {
		watch(provider, slirp_port, started + late_write, outcome);
		guest_data(4);
	}
	watch(provider, slirp_port, started + settle, outcome);
	const auto passes = provider.event_loop_passes_for_testing();
	watch(provider, slirp_port, started + settle + window, outcome);
	outcome.window_passes = provider.event_loop_passes_for_testing() - passes;
	outcome.socket_open =
		find_socket_on_port(forward.host_port, listener) >= 0;
	provider.stop();
	if (client >= 0)
		close(client);
	return true;
}

bool seen_as_expected(Seen expected, int count)
{
	return expected == Seen::any ||
		(expected == Seen::required) == (count != 0);
}

} // namespace

int main(void)
{
	bool passed = true;
	for (const auto &expected : expectations) {
		Outcome outcome;
		if (!run_scenario(expected.mode, outcome)) {
			fprintf(stderr, "FAIL %s: the scenario did not start\n",
				expected.name);
			return 1;
		}
		printf("%s: fin_to_guest=%d rst_to_guest=%d socket_open=%d "
		       "window_passes=%llu\n",
		       expected.name, outcome.fins, outcome.resets,
		       outcome.socket_open ? 1 : 0,
		       static_cast<unsigned long long>(outcome.window_passes));
		if (outcome.window_passes >= window_pass_limit) {
			fprintf(stderr,
				"FAIL %s: %llu event-loop passes in %lld ms, "
				"the loop is spinning\n",
				expected.name,
				static_cast<unsigned long long>(
					outcome.window_passes),
				static_cast<long long>(window.count()));
			passed = false;
		}
		if (!seen_as_expected(expected.fin, outcome.fins)) {
			fprintf(stderr, "FAIL %s: the guest got %s\n",
				expected.name,
				expected.fin == Seen::required ? "no FIN" : "a FIN");
			passed = false;
		}
		if (!seen_as_expected(expected.rst, outcome.resets)) {
			fprintf(stderr, "FAIL %s: the guest got %s\n",
				expected.name,
				expected.rst == Seen::required ? "no RST" : "a RST");
			passed = false;
		}
		if (outcome.socket_open != expected.socket_open) {
			fprintf(stderr, "FAIL %s: SLiRP's host socket is %s\n",
				expected.name,
				outcome.socket_open ? "still open" : "closed");
			passed = false;
		}
	}
	if (!passed)
		return 1;
	printf("PASS: a host socket dead in both directions is reset toward the "
	       "guest, closed, and leaves the event loop idle\n");
	return 0;
}
