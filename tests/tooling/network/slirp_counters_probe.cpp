/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The provider counts the guest's network for the session record: DNS queries
 * by how they ended, DHCP exchanges, and TCP connection attempts by how SLiRP
 * answered them. The probe drives the provider the way a guest does and reads
 * the counts back through the provider and through the C entry the runtime
 * uses (uml_slirp_net_read_counters).
 *
 * The resolver is confined to kDNSServiceInterfaceIndexLocalOnly, which needs
 * no network. The guest cannot reach the host's loopback through the gateway,
 * so a connection that is made goes to a listener behind a test guest forward
 * (a Unix socket the probe listens on); a SYN to the gateway at the port of a
 * live host listener is one that is unreachable, and the listener hears
 * nothing; a connect to port 0 of a documentation address is one SLiRP leaves
 * unanswered. A SYN to port 53 of the name server is answered by the guest
 * forward and is no attempt at all. The class a reset falls in comes from SLiRP
 * itself, in the second part, which feeds the observer the frames of an
 * unmodified SLiRP.
 *
 * The third part checks how the resolver's failures reach the counts. The
 * system resolver does not refuse by policy, die or stay silent on request, so
 * test-build hooks of the resolver inject what the daemon would do (a
 * refusal, a failed start, an answer that never comes, a dead connection, a
 * full table), and the guest must still get SERVFAIL for each.
 *
 * The last part reads a real session record: the provider's counts go through
 * the C entry the runtime uses into the runtime's session record, as the
 * Kernarium runtime wires them, and the record's network slot must show a DHCP
 * exchange and the connections made.
 */
#include "network_counters.hpp"
#include "provider.hpp"
#include "session_record.hpp"
#include "slirp_tcp_harness.hpp"
#include "uml_slirp_net.h"

#include <dns_sd.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace slirp_harness;
using Address = std::array<std::uint8_t, 4>;
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
using uml::network::NetworkCounters;
using uml::network::NetworkCounts;
using uml::network::TrafficObserver;

constexpr Address gateway{10, 0, 2, 2};
constexpr Address name_server{10, 0, 2, 3};
constexpr Address documentation{192, 0, 2, 1};

int failures;

void fail(const char *label, const std::string &message)
{
	std::fprintf(stderr, "%s: %s\n", label, message.c_str());
	failures++;
}

std::string describe(const NetworkCounts &counts)
{
	return "dns " + std::to_string(counts.dns_answered) + "/" +
		std::to_string(counts.dns_policy_denied) + "/" +
		std::to_string(counts.dns_timed_out) + "/" +
		std::to_string(counts.dns_failed) + " dhcp " +
		std::to_string(counts.dhcp_discovers) + "/" +
		std::to_string(counts.dhcp_requests) + "/" +
		std::to_string(counts.dhcp_acks) + " tcp " +
		std::to_string(counts.tcp_attempts) + "/" +
		std::to_string(counts.tcp_established) + "/" +
		std::to_string(counts.tcp_unreachable) + "/" +
		std::to_string(counts.tcp_reset);
}

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

/* A DHCP client message of `type`: a DISCOVER, or a REQUEST for the lease. */
Bytes dhcp_message(std::uint8_t type)
{
	Bytes bootp(240, 0);
	bootp[0] = 1;
	bootp[1] = 1;
	bootp[2] = 6;
	put32(bootp, 4, 0x1badcafe);
	put16(bootp, 10, 0x8000);
	std::copy(guest_mac.begin(), guest_mac.end(), bootp.begin() + 28);
	put32(bootp, 236, 0x63825363U);
	bootp.insert(bootp.end(), {53, 1, type});
	if (type == 3) {
		bootp.insert(bootp.end(), {50, 4, 10, 0, 2, 15});
		bootp.insert(bootp.end(), {54, 4, 10, 0, 2, 2});
	}
	bootp.push_back(255);
	return bootp;
}

/* The DHCP message type of a frame from SLiRP's server port, or 0. */
std::uint8_t server_dhcp_type(const Bytes &frame)
{
	if (frame.size() < 42 + 240 || get16(frame, 12) != 0x0800 ||
	    frame[23] != IPPROTO_UDP || get16(frame, 34) != 67 ||
	    get16(frame, 36) != 68)
		return 0;
	for (std::size_t index = 42 + 240; index + 2 < frame.size();) {
		const std::uint8_t code = frame[index];
		if (code == 255)
			break;
		if (code == 0) {
			index++;
			continue;
		}
		if (code == 53)
			return frame[index + 2];
		index += 2 + frame[index + 1];
	}
	return 0;
}

class Guest final {
public:
	explicit Guest(uml::network::SlirpProvider &provider)
		: provider_(provider)
	{
	}

	bool send(const Bytes &frame)
	{
		return provider_.write({reinterpret_cast<const std::byte *>(
						frame.data()),
					frame.size()}) == 0;
	}

	template <class Accept>
	std::optional<Bytes> receive(Accept accept, int timeout_ms = 3000)
	{
		const auto deadline =
			Clock::now() + std::chrono::milliseconds{timeout_ms};
		std::array<std::byte, 2048> buffer{};
		while (Clock::now() < deadline) {
			const int length = provider_.read(buffer);
			if (length <= 0) {
				pollfd descriptor{provider_.irq_fd(), POLLIN, 0};
				(void)::poll(&descriptor, 1, 10);
				continue;
			}
			Bytes frame(
				reinterpret_cast<const std::uint8_t *>(buffer.data()),
				reinterpret_cast<const std::uint8_t *>(buffer.data()) +
					length);
			if (accept(frame))
				return frame;
		}
		return std::nullopt;
	}

private:
	uml::network::SlirpProvider &provider_;
};

Bytes arp_request()
{
	Bytes frame(42, 0);
	for (std::size_t index = 0; index < 6; ++index) {
		frame[index] = 0xff;
		frame[6 + index] = guest_mac[index];
		frame[22 + index] = guest_mac[index];
	}
	put16(frame, 12, 0x0806);
	put16(frame, 14, 1);
	put16(frame, 16, 0x0800);
	frame[18] = 6;
	frame[19] = 4;
	put16(frame, 20, 1);
	std::copy(guest_ip.begin(), guest_ip.end(), frame.begin() + 28);
	std::copy(gateway.begin(), gateway.end(), frame.begin() + 38);
	return frame;
}

Bytes query_message(std::uint16_t id, const char *name)
{
	Bytes message(12, 0);
	put16(message, 0, id);
	put16(message, 2, 0x0100);
	put16(message, 4, 1);
	for (const char *label = name; *label != '\0';) {
		const char *end = label;
		while (*end != '\0' && *end != '.')
			++end;
		message.push_back(static_cast<std::uint8_t>(end - label));
		message.insert(message.end(), label, end);
		label = *end == '.' ? end + 1 : end;
	}
	message.push_back(0);
	message.insert(message.end(), {0, 1, 0, 1});
	return message;
}

/* A listener on the host's loopback; the gateway reaches it. */
struct Listener final {
	int descriptor{-1};
	std::uint16_t port{};

	Listener()
	{
		descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		socklen_t length = sizeof(address);
		if (descriptor < 0 ||
		    ::bind(descriptor, reinterpret_cast<sockaddr *>(&address),
			   sizeof(address)) != 0 ||
		    ::listen(descriptor, 4) != 0 ||
		    ::getsockname(descriptor,
				  reinterpret_cast<sockaddr *>(&address),
				  &length) != 0)
			return;
		port = ntohs(address.sin_port);
	}

	~Listener()
	{
		if (descriptor >= 0)
			::close(descriptor);
	}
};

std::uint16_t unused_loopback_port()
{
	Listener listener;
	return listener.port;
}

/* The listener behind a test guest forward: a Unix socket in a directory of
 * the probe's own. */
struct ForwardTarget final {
	static constexpr std::uint16_t guest_port = 4242;

	std::string directory;
	std::string path;
	int descriptor{-1};

	ForwardTarget()
	{
		char name[] = "/tmp/uml-counters-probe-XXXXXX";
		if (::mkdtemp(name) == nullptr)
			return;
		directory = name;
		path = directory + "/forward";
		sockaddr_un address{};
		address.sun_family = AF_UNIX;
		std::copy(path.begin(), path.end(), address.sun_path);
		descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
		if (descriptor < 0 ||
		    ::bind(descriptor, reinterpret_cast<sockaddr *>(&address),
			   sizeof(address)) != 0 ||
		    ::listen(descriptor, 4) != 0) {
			if (descriptor >= 0)
				::close(descriptor);
			descriptor = -1;
		}
	}

	~ForwardTarget()
	{
		if (descriptor >= 0)
			::close(descriptor);
		if (!path.empty())
			(void)::unlink(path.c_str());
		if (!directory.empty())
			(void)::rmdir(directory.c_str());
	}

	ForwardTarget(const ForwardTarget &) = delete;
	ForwardTarget &operator=(const ForwardTarget &) = delete;

	[[nodiscard]] bool ready() const noexcept { return descriptor >= 0; }

	/* The provider's guest forward to this listener, from the gateway. */
	void install(uml::network::SlirpProvider &provider) const
	{
		provider.add_guest_forward_for_testing(
			{uml::network::host_alias_address, guest_port, path});
	}
};

/* Whether a connection arrived at `descriptor` within `wait_ms`. */
bool heard_connection(int descriptor, int wait_ms = 150)
{
	pollfd entry{descriptor, POLLIN, 0};
	return ::poll(&entry, 1, wait_ms) > 0;
}

void expect(const char *label, bool condition,
	    const NetworkCounts &counts)
{
	if (!condition)
		fail(label, "unexpected counts: " + describe(counts));
}

void check_provider()
{
	uml::network::SlirpProvider provider;
	Guest guest{provider};
	std::array<unsigned char, 6> mac{};
	unsigned int mtu = 0;
	provider.set_dns_interface_for_testing(kDNSServiceInterfaceIndexLocalOnly);
	ForwardTarget target;
	if (!target.ready()) {
		fail("forward", "no Unix listener for the test guest forward");
		return;
	}
	target.install(provider);
	if (provider.open(mac, mtu) != 0) {
		fail("open", "the provider did not open");
		return;
	}
	(void)guest.send(arp_request());
	(void)guest.receive([](const Bytes &frame) {
		return frame.size() >= 14 && get16(frame, 12) == 0x0806;
	});
	auto counts = provider.counters();
	expect("a fresh provider", describe(counts) == "dns 0/0/0/0 dhcp 0/0/0 tcp 0/0/0/0",
	       counts);

	/* DHCP: DISCOVER is answered by an OFFER, REQUEST by an ACK. */
	const Address any{0, 0, 0, 0};
	const Address broadcast{255, 255, 255, 255};
	const char *label = "DHCP";
	if (!guest.send(udp_frame(any, broadcast, 68, 67, dhcp_message(1))))
		fail(label, "the provider refused the DISCOVER");
	if (!guest.receive([](const Bytes &frame) {
		    return server_dhcp_type(frame) == 2;
	    }))
		fail(label, "no OFFER");
	if (!guest.send(udp_frame(any, broadcast, 68, 67, dhcp_message(3))))
		fail(label, "the provider refused the REQUEST");
	if (!guest.receive([](const Bytes &frame) {
		    return server_dhcp_type(frame) == 5;
	    }))
		fail(label, "no ACK");
	counts = provider.counters();
	expect(label,
	       counts.dhcp_discovers == 1 && counts.dhcp_requests == 1 &&
		       counts.dhcp_acks == 1,
	       counts);

	/* TCP made, unreachable, and unanswered. */
	Listener listener;
	if (listener.port == 0) {
		fail("listener", "no loopback listener");
		return;
	}
	label = "a connection that is made";
	(void)guest.send(tcp_frame(guest_ip, gateway, 42000, target.guest_port,
				   100, 0, tcp_syn, 65535));
	const auto made = guest.receive([&](const Bytes &frame) {
		const auto segment = parse_tcp(frame);
		return segment && segment->source_port == target.guest_port &&
			segment->destination_port == 42000;
	});
	if (!made || (parse_tcp(*made)->flags & (tcp_syn | tcp_ack)) !=
			     (tcp_syn | tcp_ack))
		fail(label, "no SYN-ACK");
	counts = provider.counters();
	expect(label,
	       counts.tcp_attempts == 1 && counts.tcp_established == 1 &&
		       counts.tcp_reset == 0 && counts.tcp_unanswered() == 0,
	       counts);

	/* The host's loopback is not the guest's: a live listener behind the
	 * gateway is unreachable, and hears nothing. */
	label = "a connection that is unreachable";
	(void)guest.send(tcp_frame(guest_ip, gateway, 42001, listener.port, 200, 0,
				   tcp_syn, 65535));
	const auto refused = guest.receive([&](const Bytes &frame) {
		return frame.size() >= 14 + 20 + 8 && frame[23] == IPPROTO_ICMP &&
			frame[34] == 3;
	});
	if (!refused)
		fail(label, "no ICMP unreachable");
	if (heard_connection(listener.descriptor))
		fail(label, "the host listener took a connection");
	counts = provider.counters();
	expect(label,
	       counts.tcp_attempts == 2 && counts.tcp_established == 1 &&
		       counts.tcp_unreachable == 1 && counts.tcp_reset == 0 &&
		       counts.tcp_unanswered() == 0,
	       counts);

	/* Port 0 of a documentation address: Darwin refuses it before sending
	 * anything, and SLiRP leaves the SYN unanswered, however often the guest
	 * sends it. */
	label = "a connection nothing answers";
	for (int attempt = 0; attempt < 3; ++attempt)
		(void)guest.send(tcp_frame(guest_ip, documentation, 42002, 0, 300,
					   0, tcp_syn, 65535));
	if (guest.receive([](const Bytes &frame) {
		    const auto segment = parse_tcp(frame);
		    return segment && segment->destination_port == 42002;
	    }, 300))
		fail(label, "it was answered");
	counts = provider.counters();
	expect(label,
	       counts.tcp_attempts == 3 && counts.tcp_established == 1 &&
		       counts.tcp_unreachable == 1 && counts.tcp_reset == 0 &&
		       counts.tcp_unanswered() == 1,
	       counts);

	/* DNS over TCP to the name server and to the gateway is answered by the
	 * guest forward itself: the SYN-ACK says nothing about the network, so it
	 * is neither an attempt nor a connection made. */
	label = "DNS over TCP";
	for (const auto &[server, port] :
	     {std::pair<Address, std::uint16_t>{name_server, 42003},
	      std::pair<Address, std::uint16_t>{gateway, 42004}}) {
		(void)guest.send(tcp_frame(guest_ip, server, port, 53, 400, 0,
					   tcp_syn, 65535));
		const auto forward = guest.receive([&](const Bytes &frame) {
			const auto segment = parse_tcp(frame);
			return segment && segment->source_port == 53 &&
				segment->destination_port == port;
		});
		if (!forward || (parse_tcp(*forward)->flags & (tcp_syn | tcp_ack)) !=
					(tcp_syn | tcp_ack))
			fail(label, "no SYN-ACK from the guest forward");
	}
	counts = provider.counters();
	expect(label,
	       counts.tcp_attempts == 3 && counts.tcp_established == 1 &&
		       counts.tcp_unreachable == 1 && counts.tcp_reset == 0 &&
		       counts.tcp_unanswered() == 1,
	       counts);

	/* DNS: a name in /etc/hosts and a missing one are both answers. */
	label = "DNS";
	for (const char *name : {"localhost", "uml-counters-probe-missing.invalid"}) {
		const std::uint16_t port = name[0] == 'l' ? 43000 : 43001;
		(void)guest.send(udp_frame(guest_ip, name_server, port, 53,
					   query_message(port, name)));
		if (!guest.receive([&](const Bytes &frame) {
			    return frame.size() > 42 && frame[23] == IPPROTO_UDP &&
				    get16(frame, 34) == 53 &&
				    get16(frame, 36) == port;
		    }))
			fail(label, std::string{"no answer for "} + name);
	}
	counts = provider.counters();
	expect(label,
	       counts.dns_answered == 2 && counts.dns_policy_denied == 0 &&
		       counts.dns_timed_out == 0 && counts.dns_failed == 0,
	       counts);

	/* The runtime reads them through the C entry. */
	label = "the C entry";
	uml_slirp_net_counters read{};
	if (uml_slirp_net_read_counters(&provider, &read) != 0 ||
	    read.dns_answered != 2 || read.dhcp_discovers != 1 ||
	    read.dhcp_requests != 1 || read.dhcp_acks != 1 ||
	    read.tcp_attempts != 3 || read.tcp_established != 1 ||
	    read.tcp_unreachable != 1 || read.tcp_reset != 0)
		fail(label, "the counts differ from the provider's");
	if (uml_slirp_net_read_counters(nullptr, &read) >= 0 ||
	    uml_slirp_net_read_counters(&provider, nullptr) >= 0)
		fail(label, "a null argument was accepted");

	/* The counts belong to the provider, not to the link. */
	label = "a link that comes up again";
	provider.stop();
	if (provider.open(mac, mtu) != 0) {
		fail(label, "the provider did not open again");
		return;
	}
	counts = provider.counters();
	expect(label, counts.dns_answered == 2 && counts.tcp_attempts == 3,
	       counts);
	provider.stop();
}

/* A provider on the local-only resolver, ready for a query. */
struct Rig final {
	uml::network::SlirpProvider provider;
	Guest guest{provider};
	bool opened{};

	explicit Rig(std::function<void(uml::network::SlirpProvider &)> before = {})
	{
		std::array<unsigned char, 6> mac{};
		unsigned int mtu = 0;
		provider.set_dns_interface_for_testing(
			kDNSServiceInterfaceIndexLocalOnly);
		if (before)
			before(provider);
		opened = provider.open(mac, mtu) == 0;
		if (!opened)
			fail("open", "the provider did not open");
	}

	~Rig()
	{
		if (opened)
			provider.stop();
	}

	void ask(std::uint16_t port, const char *name)
	{
		(void)guest.send(udp_frame(guest_ip, name_server, port, 53,
					   query_message(port, name)));
	}

	/* The answer to the query sent from `port`: its response code, or none
	 * when no answer comes in time. */
	std::optional<std::uint8_t> answer(std::uint16_t port,
					   int timeout_ms = 3000)
	{
		const auto reply = guest.receive([&](const Bytes &frame) {
			return frame.size() > 46 && frame[23] == IPPROTO_UDP &&
				get16(frame, 34) == 53 && get16(frame, 36) == port;
		}, timeout_ms);
		if (!reply)
			return std::nullopt;
		return static_cast<std::uint8_t>((*reply)[45] & 0x0f);
	}

	/* Waits for the resolver to have dropped `count` of the answers the
	 * system resolver gave while they were held: the queries have started. */
	bool dropped(std::uint64_t count, int timeout_ms = 3000)
	{
		const auto deadline =
			Clock::now() + std::chrono::milliseconds{timeout_ms};
		while (provider.dns_answers_dropped_for_testing() < count) {
			if (Clock::now() >= deadline)
				return false;
			std::this_thread::sleep_for(std::chrono::milliseconds{2});
		}
		return true;
	}
};

constexpr std::uint8_t rcode_noerror = 0;
constexpr std::uint8_t rcode_servfail = 2;

void expect_answer(const char *label, const std::optional<std::uint8_t> &rcode,
		   std::uint8_t wanted, const NetworkCounts &counts)
{
	if (!rcode)
		fail(label, "no answer reached the guest: " + describe(counts));
	else if (*rcode != wanted)
		fail(label, "the guest was answered with rcode " +
				    std::to_string(*rcode) + ", not " +
				    std::to_string(wanted));
}

/* How the resolver's failures reach the counts, through test-build hooks (see
 * the head of this file). Each counts once, in its class, and the guest is
 * told SERVFAIL as it always was. */
void check_resolver_wiring()
{
	/* The system resolver refuses the application's query by policy. */
	{
		Rig rig;
		if (!rig.opened)
			return;
		rig.provider.inject_dns_reply_error_for_testing(
			kDNSServiceErr_PolicyDenied);
		rig.ask(43100, "localhost");
		const auto rcode = rig.answer(43100);
		const auto counts = rig.provider.counters();
		expect_answer("a query refused by policy", rcode, rcode_servfail,
			      counts);
		expect("a query refused by policy",
		       counts.dns_policy_denied == 1 && counts.dns_answered == 0 &&
			       counts.dns_timed_out == 0 && counts.dns_failed == 0,
		       counts);
	}

	/* The query cannot be started: by policy, or for any other reason. */
	{
		Rig rig;
		if (!rig.opened)
			return;
		rig.provider.inject_dns_submit_error_for_testing(
			kDNSServiceErr_NotPermitted);
		rig.ask(43101, "localhost");
		auto rcode = rig.answer(43101);
		auto counts = rig.provider.counters();
		expect_answer("a query that cannot start by policy", rcode,
			      rcode_servfail, counts);
		expect("a query that cannot start by policy",
		       counts.dns_policy_denied == 1 && counts.dns_failed == 0 &&
			       counts.dns_answered == 0,
		       counts);
		rig.provider.inject_dns_submit_error_for_testing(
			kDNSServiceErr_NoMemory);
		rig.ask(43102, "localhost");
		rcode = rig.answer(43102);
		counts = rig.provider.counters();
		expect_answer("a query that cannot start", rcode, rcode_servfail,
			      counts);
		expect("a query that cannot start",
		       counts.dns_policy_denied == 1 && counts.dns_failed == 1 &&
			       counts.dns_answered == 0 && counts.dns_timed_out == 0,
		       counts);
		/* And the next query is answered as before. */
		rig.ask(43103, "localhost");
		rcode = rig.answer(43103);
		counts = rig.provider.counters();
		expect_answer("a query after a failed start", rcode, rcode_noerror,
			      counts);
		expect("a query after a failed start", counts.dns_answered == 1,
		       counts);
	}

	/* The system resolver never answers: the query's deadline passes. */
	{
		Rig rig;
		if (!rig.opened)
			return;
		rig.provider.hold_dns_answers_for_testing(true);
		rig.provider.set_dns_deadline_for_testing(
			std::chrono::milliseconds{150});
		rig.ask(43104, "localhost");
		const auto rcode = rig.answer(43104);
		const auto counts = rig.provider.counters();
		expect_answer("a query nobody answers", rcode, rcode_servfail,
			      counts);
		expect("a query nobody answers",
		       counts.dns_timed_out == 1 && counts.dns_answered == 0 &&
			       counts.dns_policy_denied == 0 && counts.dns_failed == 0,
		       counts);
		rig.provider.hold_dns_answers_for_testing(false);
		rig.provider.set_dns_deadline_for_testing(
			std::chrono::milliseconds{0});
		rig.ask(43105, "localhost");
		const auto after = rig.answer(43105);
		const auto last = rig.provider.counters();
		expect_answer("a query after an expiry", after, rcode_noerror,
			      last);
		expect("a query after an expiry",
		       last.dns_answered == 1 && last.dns_timed_out == 1, last);
	}

	/* The daemon goes away with a query in flight: that query is lost and
	 * failed with the one that found the connection dead. */
	{
		Rig rig;
		if (!rig.opened)
			return;
		rig.provider.hold_dns_answers_for_testing(true);
		rig.ask(43106, "localhost");
		if (!rig.dropped(1)) {
			fail("a lost connection", "the first query did not start");
			return;
		}
		rig.provider.inject_dns_submit_error_for_testing(
			kDNSServiceErr_ServiceNotRunning);
		rig.ask(43107, "localhost");
		const auto second = rig.answer(43107);
		const auto first = rig.answer(43106);
		const auto counts = rig.provider.counters();
		expect_answer("a lost connection (the query that found it)", second,
			      rcode_servfail, counts);
		expect_answer("a lost connection (the query it held)", first,
			      rcode_servfail, counts);
		expect("a lost connection",
		       counts.dns_failed == 2 && counts.dns_timed_out == 0 &&
			       counts.dns_policy_denied == 0 && counts.dns_answered == 0,
		       counts);
		/* The resolver connects again for the next query. */
		rig.provider.hold_dns_answers_for_testing(false);
		rig.ask(43108, "localhost");
		const auto after = rig.answer(43108);
		const auto last = rig.provider.counters();
		expect_answer("a query after a lost connection", after,
			      rcode_noerror, last);
		expect("a query after a lost connection",
		       last.dns_answered == 1 && last.dns_failed == 2, last);
	}

	/* More queries than the resolver carries at once. */
	{
		Rig rig;
		if (!rig.opened)
			return;
		rig.provider.set_dns_pending_limit_for_testing(1);
		rig.provider.hold_dns_answers_for_testing(true);
		rig.ask(43109, "localhost");
		rig.ask(43110, "localhost");
		const auto rcode = rig.answer(43110);
		const auto counts = rig.provider.counters();
		expect_answer("a full table", rcode, rcode_servfail, counts);
		expect("a full table",
		       counts.dns_failed == 1 && counts.dns_answered == 0 &&
			       counts.dns_timed_out == 0 &&
			       counts.dns_policy_denied == 0,
		       counts);
	}
}

/* The provider the session record's reader reads, as the Kernarium runtime
 * publishes its own for the runtime's reader. */
uml::network::SlirpProvider *recorded_provider;

bool read_recorded_counts(uml::runtime::SessionNetworkCounts &counts) noexcept
{
	uml_slirp_net_counters read{};
	if (recorded_provider == nullptr ||
	    uml_slirp_net_read_counters(recorded_provider, &read) != 0)
		return false;
	counts = {.dns_answered = read.dns_answered,
		  .dns_policy_denied = read.dns_policy_denied,
		  .dns_timed_out = read.dns_timed_out,
		  .dns_failed = read.dns_failed,
		  .dhcp_discovers = read.dhcp_discovers,
		  .dhcp_requests = read.dhcp_requests,
		  .dhcp_acks = read.dhcp_acks,
		  .tcp_attempts = read.tcp_attempts,
		  .tcp_established = read.tcp_established,
		  .tcp_unreachable = read.tcp_unreachable,
		  .tcp_reset = read.tcp_reset};
	return true;
}

/* A guest that gets its lease and makes one connection and one that is
 * unreachable, in a session record: the slot says so in words, as Andarium
 * reads them. */
void check_session_record()
{
	const char *label = "the session record";
	ForwardTarget target;
	if (!target.ready()) {
		fail(label, "no Unix listener for the test guest forward");
		return;
	}
	Rig rig{[&](uml::network::SlirpProvider &provider) {
		target.install(provider);
	}};
	if (!rig.opened)
		return;
	/* The provider counted before the session began: the slot must hold only
	 * what came after. */
	const Address any{0, 0, 0, 0};
	const Address broadcast{255, 255, 255, 255};
	char name[] = "/tmp/uml-counters-probe-record-XXXXXX";
	const int descriptor = ::mkstemp(name);
	if (descriptor < 0) {
		fail(label, "no temporary record");
		return;
	}
	::close(descriptor);
	const std::string path = name;
	recorded_provider = &rig.provider;
	uml::runtime::reset_session_record();
	uml::runtime::set_network_counts_reader(&read_recorded_counts);
	if (!uml::runtime::begin_session_record(path, {})) {
		fail(label, "the record did not open");
		recorded_provider = nullptr;
		uml::runtime::set_network_counts_reader(nullptr);
		return;
	}
	if (!rig.guest.send(udp_frame(any, broadcast, 68, 67, dhcp_message(1))) ||
	    !rig.guest.receive([](const Bytes &frame) {
		    return server_dhcp_type(frame) == 2;
	    }) ||
	    !rig.guest.send(udp_frame(any, broadcast, 68, 67, dhcp_message(3))) ||
	    !rig.guest.receive([](const Bytes &frame) {
		    return server_dhcp_type(frame) == 5;
	    }))
		fail(label, "the guest did not get its lease");
	Listener listener;
	(void)rig.guest.send(tcp_frame(guest_ip, gateway, 45000,
				       target.guest_port, 100, 0, tcp_syn, 65535));
	(void)rig.guest.receive([&](const Bytes &frame) {
		const auto segment = parse_tcp(frame);
		return segment && segment->source_port == target.guest_port;
	});
	(void)rig.guest.send(tcp_frame(guest_ip, gateway, 45001, listener.port,
				       200, 0, tcp_syn, 65535));
	(void)rig.guest.receive([&](const Bytes &frame) {
		return frame.size() >= 14 + 20 + 8 && frame[23] == IPPROTO_ICMP &&
			frame[34] == 3;
	});
	uml::runtime::checkpoint_session_memory();
	std::ifstream file{path, std::ios::binary};
	const std::string text{std::istreambuf_iterator<char>{file}, {}};
	const std::size_t offset = uml::runtime::session_network_offset;
	const std::string slot = text.size() >= offset +
			uml::runtime::session_network_slot_bytes ?
		text.substr(offset, uml::runtime::session_network_slot_bytes) :
		std::string{};
	const auto has = [&](const char *line) {
		return slot.find(line) != std::string::npos;
	};
	if (!slot.starts_with("network 1\n") ||
	    !has("net-dhcp discovers 1 requests 1 acks 1\n") ||
	    !has("net-tcp attempts 2 established 1 unreachable 1 reset 0\n") ||
	    !has("net-notice denials 0 now none\n"))
		fail(label, "the network slot is wrong: " + slot);
	/* The slot's lines, without the blanks that pad it. */
	if (slot.size() > 2)
		std::fprintf(stderr, "network slot:\n%.*s",
			     static_cast<int>(slot.find_last_of(
						      '\n', slot.size() - 2) + 1),
			     slot.c_str());
	uml::runtime::set_network_counts_reader(nullptr);
	uml::runtime::reset_session_record();
	recorded_provider = nullptr;
	(void)::unlink(path.c_str());
}

/* The frames of an unmodified SLiRP, through the observer: a destination the
 * host cannot route to is answered with ICMP net unreachable, which the guest
 * reads as unreachable. */
void check_slirp_errors()
{
	Network network;
	if (network.slirp() == nullptr || !network.resolve()) {
		fail("SLiRP", "SLiRP did not start or answer ARP");
		return;
	}
	NetworkCounters counters;
	TrafficObserver observer{counters};
	const auto send = [&](const Bytes &frame) {
		observer.from_guest(frame);
		network.input(frame);
	};
	const auto collect = [&] {
		for (int pass = 0; pass < 30; ++pass)
			network.pump(20, 5);
		while (!network.frames().empty()) {
			observer.from_network(network.frames().front());
			network.frames().pop_front();
		}
	};

	/* Linux refuses a TCP connect to a multicast address with ENETUNREACH,
	 * and so does Darwin; SLiRP answers with net unreachable. */
	send(tcp_frame(guest_ip, {224, 0, 0, 1}, 44000, 80, 500, 0, tcp_syn,
		       65535));
	collect();
	auto counts = counters.read();
	expect("ICMP net unreachable",
	       counts.tcp_attempts == 1 && counts.tcp_unreachable == 1 &&
		       counts.tcp_reset == 0 && counts.tcp_unanswered() == 0,
	       counts);

	/* A reset in answer to a SYN, as SLiRP writes it. */
	const std::uint16_t closed = unused_loopback_port();
	send(tcp_frame(guest_ip, gateway, 44001, closed, 600, 0, tcp_syn, 65535));
	collect();
	counts = counters.read();
	expect("a reset",
	       counts.tcp_attempts == 2 && counts.tcp_reset == 1 &&
		       counts.tcp_unreachable == 1,
	       counts);

	Listener listener;
	send(tcp_frame(guest_ip, gateway, 44002, listener.port, 700, 0, tcp_syn,
		       65535));
	collect();
	counts = counters.read();
	expect("a SYN-ACK",
	       counts.tcp_attempts == 3 && counts.tcp_established == 1 &&
		       counts.tcp_unanswered() == 0,
	       counts);
}

} // namespace

int main(void)
{
	check_provider();
	check_slirp_errors();
	check_resolver_wiring();
	check_session_record();
	if (failures != 0) {
		std::fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	std::puts("the network counters read what the provider saw");
	return 0;
}
