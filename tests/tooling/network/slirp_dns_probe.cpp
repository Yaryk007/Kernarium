/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The guest's name server is answered by the host's system resolver, and the
 * guest's DNS never makes SLiRP open a host Internet socket: SLiRP's own proxy
 * would send each query to the host's first configured server, which on most
 * networks is a local-network router.
 *
 * The probe runs the provider with the resolver confined to
 * kDNSServiceInterfaceIndexLocalOnly, which answers from /etc/hosts and gives
 * an immediate negative answer for any other name, so it needs no network.
 * It checks UDP and TCP, the name server and the gateway address, a missing
 * name, the refusals (a non-DNS TCP port is reset, a non-DNS datagram is
 * dropped), guest multicast, and a stop and reopen. The wire rules themselves
 * are the unit test network-dns-rules.
 *
 * While the host has no network path the system resolver says "no such
 * record" at once, and the guest must be told to try again instead. The
 * probe stands in for the path with the provider's state: a missing name is
 * answered as before while the path is usable, with SERVFAIL (and counted as
 * failed) while it is not, over UDP and TCP, a name with records is answered
 * in both states, an alias whose target has no record is one answer while the
 * path is usable and SERVFAIL without records while it is not (the aliases
 * alone are no address), and the state at the moment of the answer decides, not
 * the state when the query came in. An outage that was reported ends with the
 * provider's stop. A last part starts the system's monitor and checks that the
 * provider hears a report of that open, whichever path this host has, and that
 * one monitor serves the provider's life, across a stop and a reopen. The
 * system giving no monitor is made here too: the provider then makes another
 * at its next open, and keeps the one that works. What the state does to an
 * answer across a stop and a reopen is checked with a monitor that writes
 * nothing, the state being the probe's own: a monitor that works keeps an
 * outage across a stop, and one the system did not give does not.
 */
#include "dns_message.hpp"
#include "host_network_path_attempt.hpp"
#include "provider.hpp"
#include "slirp_tcp_harness.hpp"
#include "uml_slirp_net.h"

#include <dns_sd.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace slirp_harness;
using Address = std::array<std::uint8_t, 4>;
using Clock = std::chrono::steady_clock;
namespace dns = uml::network::dns;

constexpr Address name_server{10, 0, 2, 3};
constexpr Address gateway{10, 0, 2, 2};

int failures;

void fail(const char *label, const std::string &message)
{
	std::fprintf(stderr, "%s: %s\n", label, message.c_str());
	failures++;
}

std::vector<std::uint8_t> wire(const char *presentation)
{
	return dns::wire_name(presentation).value_or(std::vector<std::uint8_t>{});
}

std::vector<std::uint8_t> query_message(std::uint16_t id, const char *name,
					std::uint16_t type, bool edns = false)
{
	std::vector<std::uint8_t> message(12, 0);
	put16(message, 0, id);
	put16(message, 2, 0x0100); /* RD */
	put16(message, 4, 1);
	put16(message, 10, edns ? 1 : 0);
	const auto encoded = wire(name);
	message.insert(message.end(), encoded.begin(), encoded.end());
	message.resize(message.size() + 4);
	put16(message, message.size() - 4, type);
	put16(message, message.size() - 2, 1);
	if (edns) {
		const std::uint8_t opt[] = {0, 0, 41, 0x04, 0xd0, 0, 0, 0, 0, 0, 0};
		message.insert(message.end(), std::begin(opt), std::end(opt));
	}
	return message;
}

std::vector<std::uint8_t> udp_frame(const Address &destination,
				    std::uint16_t source_port,
				    std::uint16_t destination_port,
				    const std::vector<std::uint8_t> &payload)
{
	const std::size_t udp_length = 8 + payload.size();
	std::vector<std::uint8_t> frame(14 + 20 + udp_length, 0);
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
		frame[26 + index] = guest_ip[index];
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
	put16(frame, 40, checksum_fold(sum));
	return frame;
}

std::vector<std::uint8_t> tcp_data_frame(const Address &destination,
					 std::uint16_t source_port,
					 std::uint32_t seq, std::uint32_t ack,
					 std::uint8_t flags,
					 const std::vector<std::uint8_t> &payload)
{
	auto frame = tcp_frame(guest_ip, destination, source_port, 53, seq, ack,
			       flags, 65535, payload.size());
	std::copy(payload.begin(), payload.end(), frame.begin() + 54);
	put16(frame, 50, 0);
	std::uint32_t sum = checksum_add(0, &frame[26], 8);
	sum += IPPROTO_TCP;
	sum += static_cast<std::uint32_t>(frame.size() - 34);
	sum = checksum_add(sum, &frame[34], frame.size() - 34);
	put16(frame, 50, checksum_fold(sum));
	return frame;
}

std::vector<std::uint8_t> arp_request()
{
	std::vector<std::uint8_t> frame(42, 0);
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

class Guest final {
public:
	explicit Guest(uml::network::SlirpProvider &provider)
		: provider_(provider)
	{
	}

	bool send(const std::vector<std::uint8_t> &frame)
	{
		return provider_.write({reinterpret_cast<const std::byte *>(
						frame.data()),
					frame.size()}) == 0;
	}

	/* The next frame the provider delivers that `accept` takes. */
	template <class Accept>
	std::optional<std::vector<std::uint8_t>> receive(Accept accept,
							 int timeout_ms = 3000)
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
			std::vector<std::uint8_t> frame(
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

struct Reply {
	std::uint16_t id{};
	std::uint8_t rcode{};
	bool truncated{};
	std::uint16_t answers{};
	std::vector<std::uint8_t> message;
};

std::optional<Reply> parse_reply(const std::vector<std::uint8_t> &message)
{
	if (message.size() < 12 || (message[2] & 0x80) == 0)
		return std::nullopt;
	return Reply{get16(message, 0), static_cast<std::uint8_t>(message[3] & 0x0f),
		     (message[2] & 0x02) != 0, get16(message, 6), message};
}

/* A UDP DNS reply from `server`:53 to `port`. */
std::optional<Reply> udp_reply(const std::vector<std::uint8_t> &frame,
			       const Address &server, std::uint16_t port)
{
	if (frame.size() < 42 || get16(frame, 12) != 0x0800 ||
	    frame[23] != IPPROTO_UDP || get16(frame, 34) != 53 ||
	    get16(frame, 36) != port)
		return std::nullopt;
	for (std::size_t index = 0; index < 4; ++index)
		if (frame[26 + index] != server[index] ||
		    frame[30 + index] != guest_ip[index])
			return std::nullopt;
	return parse_reply({frame.begin() + 42, frame.end()});
}

bool has_loopback_answer(const Reply &reply)
{
	const std::uint8_t loopback[] = {0, 1, 0, 1, 0, 0, 0, 0, 0, 4,
					 127, 0, 0, 1};
	/* Type A, class IN, any TTL, RDLENGTH 4, 127.0.0.1. */
	for (std::size_t index = 0; index + sizeof(loopback) <= reply.message.size();
	     ++index) {
		bool match = true;
		for (std::size_t byte = 0; byte < sizeof(loopback); ++byte) {
			if (byte >= 4 && byte < 8)
				continue;
			if (reply.message[index + byte] != loopback[byte]) {
				match = false;
				break;
			}
		}
		if (match)
			return true;
	}
	return false;
}

void check_udp(uml::network::SlirpProvider &provider, Guest &guest)
{
	const auto internet = provider.internet_sockets_for_testing();
	std::uint16_t port = 40000;
	for (const auto &server : {name_server, gateway}) {
		const char *label = server == name_server ?
			"UDP to the name server" : "UDP to the gateway";
		++port;
		const std::uint16_t id = static_cast<std::uint16_t>(0x5100 + port);
		const auto started = Clock::now();
		if (!guest.send(udp_frame(server, port, 53,
					  query_message(id, "localhost", 1))))
			fail(label, "the provider refused the query");
		std::optional<Reply> reply;
		(void)guest.receive([&](const std::vector<std::uint8_t> &frame) {
			reply = udp_reply(frame, server, port);
			return reply.has_value();
		});
		if (!reply || reply->id != id || reply->rcode != 0 ||
		    !has_loopback_answer(*reply)) {
			fail(label, "localhost has no 127.0.0.1 answer");
			continue;
		}
		std::printf("%s: localhost answered in %lld ms\n", label,
			    static_cast<long long>(
				    std::chrono::duration_cast<
					    std::chrono::milliseconds>(
					    Clock::now() - started)
					    .count()));
	}

	const char *label = "a missing name";
	if (!guest.send(udp_frame(name_server, 40100, 53,
				  query_message(0x6200,
						"uml-dns-probe-missing.invalid",
						1))))
		fail(label, "the provider refused the query");
	std::optional<Reply> reply;
	(void)guest.receive([&](const std::vector<std::uint8_t> &frame) {
		reply = udp_reply(frame, name_server, 40100);
		return reply.has_value();
	});
	if (!reply || reply->answers != 0 ||
	    (reply->rcode != dns::rcode_noerror &&
	     reply->rcode != dns::rcode_nxdomain))
		fail(label, "no negative answer");
	else
		std::printf("%s: rcode %u\n", label, reply->rcode);

	label = "guest multicast";
	const Address mdns{224, 0, 0, 251};
	const auto before_multicast = provider.internet_sockets_for_testing();
	if (!guest.send(udp_frame(mdns, 5353, 5353, query_message(0, "adb.example", 255))))
		fail(label, "the provider refused the datagram");
	std::this_thread::sleep_for(std::chrono::milliseconds{200});
	if (provider.internet_sockets_for_testing() != before_multicast)
		fail(label, "an mDNS datagram left the virtual link");

	label = "a datagram to another port";
	if (!guest.send(udp_frame(name_server, 40101, 123,
				  std::vector<std::uint8_t>(48, 0))))
		fail(label, "the provider refused the datagram");
	if (guest.receive([](const std::vector<std::uint8_t> &frame) {
		    return frame.size() > 42 && frame[23] == IPPROTO_UDP &&
			    get16(frame, 36) == 40101;
	    }, 300))
		fail(label, "it was answered");

	if (provider.internet_sockets_for_testing() != internet)
		fail("UDP", "the guest's DNS made SLiRP open a host Internet socket");

	/* The witness itself: a datagram SLiRP does handle, to the gateway's
	 * discard port, which it sends to host loopback, is counted. */
	label = "the socket witness";
	if (!guest.send(udp_frame(gateway, 40102, 9,
				  std::vector<std::uint8_t>(8, 0))))
		fail(label, "the provider refused the datagram");
	const auto deadline = Clock::now() + std::chrono::seconds{2};
	while (provider.internet_sockets_for_testing() == internet &&
	       Clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds{1});
	if (provider.internet_sockets_for_testing() == internet)
		fail(label, "a datagram SLiRP forwards was not counted");
}

/* One DNS-over-TCP exchange with `server`:53; returns the reply. With
 * `half_close` the guest sends its FIN with the query, as a client does that
 * shuts down its sending side and then reads. */
std::optional<Reply> tcp_exchange(Guest &guest, const Address &server,
				  std::uint16_t port,
				  const std::vector<std::uint8_t> &message,
				  bool half_close = false)
{
	const auto from_server = [&](const std::vector<std::uint8_t> &frame) {
		const auto segment = parse_tcp(frame);
		return segment && segment->source == server &&
			segment->source_port == 53 &&
			segment->destination_port == port;
	};
	std::uint32_t seq = 1000;
	if (!guest.send(tcp_frame(guest_ip, server, port, 53, seq, 0, tcp_syn,
				  65535)))
		return std::nullopt;
	const auto synack = guest.receive(from_server);
	if (!synack)
		return std::nullopt;
	const auto handshake = *parse_tcp(*synack);
	if ((handshake.flags & (tcp_syn | tcp_ack)) != (tcp_syn | tcp_ack))
		return std::nullopt;
	++seq;
	std::uint32_t ack = handshake.seq + 1;
	std::vector<std::uint8_t> framed;
	framed.push_back(static_cast<std::uint8_t>(message.size() >> 8));
	framed.push_back(static_cast<std::uint8_t>(message.size()));
	framed.insert(framed.end(), message.begin(), message.end());
	if (!guest.send(tcp_data_frame(server, port, seq, ack,
				       tcp_ack | tcp_psh |
					       (half_close ? tcp_fin : 0),
				       framed)))
		return std::nullopt;
	seq += static_cast<std::uint32_t>(framed.size()) + (half_close ? 1 : 0);

	std::vector<std::uint8_t> stream;
	while (stream.size() < 2 ||
	       stream.size() < 2U + get16(stream, 0)) {
		const auto frame = guest.receive(from_server);
		if (!frame)
			return std::nullopt;
		const auto segment = *parse_tcp(*frame);
		if ((segment.flags & tcp_rst) != 0)
			return std::nullopt;
		if (segment.payload == 0)
			continue;
		const std::size_t offset = frame->size() - segment.payload;
		stream.insert(stream.end(), frame->begin() + offset, frame->end());
		ack = segment.seq + static_cast<std::uint32_t>(segment.payload);
		(void)guest.send(tcp_frame(guest_ip, server, port, 53, seq, ack,
					   tcp_ack, 65535));
	}
	if (!half_close)
		(void)guest.send(tcp_frame(guest_ip, server, port, 53, seq, ack,
					   tcp_fin | tcp_ack, 65535));
	return parse_reply({stream.begin() + 2, stream.end()});
}

void check_tcp(uml::network::SlirpProvider &provider, Guest &guest)
{
	const auto internet = provider.internet_sockets_for_testing();
	std::uint16_t port = 41000;
	for (const auto &server : {name_server, gateway}) {
		const char *label = server == name_server ?
			"TCP to the name server" : "TCP to the gateway";
		const auto reply = tcp_exchange(guest, server, ++port,
						query_message(0x7300, "localhost", 1));
		if (!reply || reply->id != 0x7300 || reply->rcode != 0 ||
		    !has_loopback_answer(*reply))
			fail(label, "localhost has no 127.0.0.1 answer");
	}

	const char *label = "TCP half-close";
	const auto half = tcp_exchange(guest, name_server, ++port,
				       query_message(0x7301, "localhost", 1),
				       true);
	if (!half || half->id != 0x7301 || half->rcode != 0 ||
	    !has_loopback_answer(*half))
		fail(label, "a client that shut down its sending side got no answer");

	label = "DNS over TLS";
	if (!guest.send(tcp_frame(guest_ip, name_server, 41100, 853, 5000, 0,
				  tcp_syn, 65535)))
		fail(label, "the provider refused the SYN");
	const auto reset = guest.receive([](const std::vector<std::uint8_t> &frame) {
		const auto segment = parse_tcp(frame);
		return segment && segment->source_port == 853 &&
			segment->destination_port == 41100;
	});
	const auto segment = reset ? parse_tcp(*reset) : std::nullopt;
	if (!segment || segment->flags != (tcp_rst | tcp_ack) ||
	    segment->ack != 5001)
		fail(label, "port 853 of the name server is not reset");

	if (provider.internet_sockets_for_testing() != internet)
		fail("TCP", "the guest's DNS made SLiRP open a host Internet socket");
}

/* A UDP query to the name server; the reply. */
std::optional<Reply> ask(Guest &guest, const char *name, std::uint16_t id,
			 std::uint16_t port)
{
	if (!guest.send(udp_frame(name_server, port, 53,
				  query_message(id, name, 1))))
		return std::nullopt;
	std::optional<Reply> reply;
	(void)guest.receive([&](const std::vector<std::uint8_t> &frame) {
		reply = udp_reply(frame, name_server, port);
		return reply.has_value() && reply->id == id;
	});
	return reply;
}

/* "No such record" or "no such name", the answer for a missing name. */
bool is_negative(const std::optional<Reply> &reply)
{
	return reply && reply->answers == 0 &&
		(reply->rcode == dns::rcode_noerror ||
		 reply->rcode == dns::rcode_nxdomain);
}

bool is_servfail(const std::optional<Reply> &reply)
{
	return reply && reply->answers == 0 &&
		reply->rcode == dns::rcode_servfail;
}

bool has_loopback(const std::optional<Reply> &reply)
{
	return reply && reply->rcode == dns::rcode_noerror &&
		has_loopback_answer(*reply);
}

/* A CNAME in the system resolver's local-only registry, which stays while the
 * object lives. Its target has no record, so a query for an address of the
 * alias gets the alias, then "no such record". The registry takes a record
 * only in the mDNS domain: a name under .invalid, .test, .example,
 * .localhost, .home.arpa, .lan, .internal or a real domain is refused as a
 * bad parameter, which is why the name is the one environment-portability
 * allowlist entry of this file. */
class LocalAlias final {
public:
	static constexpr const char *name = "uml-dns-probe-alias.local";

	LocalAlias()
	{
		const auto target = wire("uml-dns-probe-target.invalid");
		if (DNSServiceCreateConnection(&connection_) !=
		    kDNSServiceErr_NoError)
			return;
		if (DNSServiceRegisterRecord(
			    connection_, &record_, kDNSServiceFlagsUnique,
			    kDNSServiceInterfaceIndexLocalOnly, name,
			    kDNSServiceType_CNAME, kDNSServiceClass_IN,
			    static_cast<std::uint16_t>(target.size()),
			    target.data(), 120, &registered, this) !=
		    kDNSServiceErr_NoError)
			return;
		/* The daemon says so when the record is in. */
		const int descriptor = DNSServiceRefSockFD(connection_);
		const auto deadline = Clock::now() + std::chrono::seconds{5};
		while (!done_ && Clock::now() < deadline) {
			pollfd waiting{descriptor, POLLIN, 0};
			if (::poll(&waiting, 1, 100) > 0 &&
			    DNSServiceProcessResult(connection_) !=
				    kDNSServiceErr_NoError)
				break;
		}
	}
	LocalAlias(const LocalAlias &) = delete;
	LocalAlias &operator=(const LocalAlias &) = delete;
	~LocalAlias()
	{
		if (connection_ != nullptr)
			DNSServiceRefDeallocate(connection_);
	}
	[[nodiscard]] bool ok() const noexcept { return ok_; }

private:
	static void DNSSD_API registered(DNSServiceRef, DNSRecordRef,
					 DNSServiceFlags,
					 DNSServiceErrorType error,
					 void *context)
	{
		auto *self = static_cast<LocalAlias *>(context);
		self->ok_ = error == kDNSServiceErr_NoError;
		self->done_ = true;
	}

	DNSServiceRef connection_{};
	DNSRecordRef record_{};
	bool done_{};
	bool ok_{};
};

/* Changes the path from the resolver's thread, after the system has
 * answered and before the guest is. */
struct PathChange final {
	uml::platform::HostNetworkPathState *path{};
	bool usable{};
};

void change_path(void *context)
{
	const auto *change = static_cast<const PathChange *>(context);
	change->path->report(change->usable);
}

/* A missing name asks for a retry while the host has no network path, and is
 * a plain "no such record" while it has one; a name with records is answered
 * either way; and the state when the answer is made decides. */
void check_outage(uml::network::SlirpProvider &provider, Guest &guest)
{
	auto &path = provider.path_state_for_testing();
	const char *const missing = "uml-dns-probe-outage.invalid";
	std::uint16_t port = 42000;
	const auto before = provider.counters();
	const int failures_before = failures;
	if (!path.usable())
		fail("the path state", "it does not start usable");
	const LocalAlias alias;
	if (!alias.ok())
		fail("the alias record", "the system did not take it");

	const char *label = "a missing name with a path";
	if (!is_negative(ask(guest, missing, 0x7a01, ++port)))
		fail(label, "no plain negative answer");

	label = "an alias with no address, with a path";
	const auto aliased = ask(guest, LocalAlias::name, 0x7a0a, ++port);
	if (!aliased || aliased->rcode != dns::rcode_noerror ||
	    aliased->answers != 1)
		fail(label, "no answer that carries the alias");

	path.report(false);
	label = "an alias with no address, with no path";
	if (!is_servfail(ask(guest, LocalAlias::name, 0x7a0b, ++port)))
		fail(label, "the alias was passed on, or the guest was not told "
			    "to try again");
	label = "a missing name with no path";
	if (!is_servfail(ask(guest, missing, 0x7a02, ++port)))
		fail(label, "the guest was not told to try again");
	label = "a name with records and no path";
	if (!has_loopback(ask(guest, "localhost", 0x7a03, ++port)))
		fail(label, "localhost has no 127.0.0.1 answer");
	label = "a missing name over TCP with no path";
	if (!is_servfail(tcp_exchange(guest, name_server, ++port,
				      query_message(0x7a04, missing, 1))))
		fail(label, "the guest was not told to try again");
	label = "a name with records over TCP and no path";
	if (!has_loopback(tcp_exchange(guest, name_server, ++port,
				       query_message(0x7a05, "localhost", 1))))
		fail(label, "localhost has no 127.0.0.1 answer");

	path.report(true);
	label = "a missing name once the path is back";
	if (!is_negative(ask(guest, missing, 0x7a06, ++port)))
		fail(label, "no plain negative answer");
	label = "a missing name over TCP once the path is back";
	if (!is_negative(tcp_exchange(guest, name_server, ++port,
				      query_message(0x7a07, missing, 1))))
		fail(label, "no plain negative answer");

	/* The state at the answer decides: the system answers while the path is
	 * as it was, and the path changes before the guest is told. */
	PathChange change{&path, false};
	provider.set_dns_before_answer_for_testing(&change_path, &change);
	label = "a path lost as the answer is made";
	if (!is_servfail(ask(guest, missing, 0x7a08, ++port)))
		fail(label, "the state when the query came in decided");
	change.usable = true;
	label = "a path back as the answer is made";
	if (!is_negative(ask(guest, missing, 0x7a09, ++port)))
		fail(label, "the state when the query came in decided");
	provider.set_dns_before_answer_for_testing(nullptr, nullptr);
	if (!path.usable())
		fail("the path state", "it was left in an outage");

	/* Each query is one outcome: an answer, or a failure the guest retries.
	 * Seven were answered (two names with records, an alias with a path, four
	 * missing names with a path) and four met the outage (an alias and three
	 * missing names). */
	const auto counts = provider.counters();
	const auto answered = counts.dns_answered - before.dns_answered;
	const auto failed = counts.dns_failed - before.dns_failed;
	if (answered != 7 || failed != 4 ||
	    counts.dns_policy_denied != before.dns_policy_denied ||
	    counts.dns_timed_out != before.dns_timed_out)
		fail("the counts", "answered " + std::to_string(answered) +
			", failed " + std::to_string(failed) +
			" for seven answers and four outages");
	if (failures == failures_before)
		std::puts("the guest is told to retry while the host has no path");
}

/* Without a socket path for DNS over TCP, a connection to port 53 is reset
 * like one to any other port: SLiRP, with its own proxy off, would connect to
 * the literal address on the host's network. */
void check_tcp_without_stream(uml::network::SlirpProvider &provider,
			      Guest &guest)
{
	const auto internet = provider.internet_sockets_for_testing();
	std::uint16_t port = 41200;
	for (const auto &server : {name_server, gateway}) {
		const char *label = server == name_server ?
			"TCP without a stream socket, name server" :
			"TCP without a stream socket, gateway";
		++port;
		if (!guest.send(tcp_frame(guest_ip, server, port, 53, 7000, 0,
					  tcp_syn, 65535)))
			fail(label, "the provider refused the SYN");
		const auto answer = guest.receive(
			[&](const std::vector<std::uint8_t> &frame) {
				const auto segment = parse_tcp(frame);
				return segment && segment->source == server &&
					segment->source_port == 53 &&
					segment->destination_port == port;
			});
		const auto segment = answer ? parse_tcp(*answer) : std::nullopt;
		if (!segment || segment->flags != (tcp_rst | tcp_ack) ||
		    segment->ack != 7001)
			fail(label, "port 53 is not reset");
	}
	if (provider.internet_sockets_for_testing() != internet)
		fail("TCP without a stream socket",
		     "SLiRP opened a host Internet socket");
}

bool open_provider(uml::network::SlirpProvider &provider, Guest &guest)
{
	std::array<unsigned char, 6> mac{};
	unsigned int mtu = 0;
	provider.set_dns_interface_for_testing(kDNSServiceInterfaceIndexLocalOnly);
	if (provider.open(mac, mtu) != 0)
		return false;
	(void)guest.send(arp_request());
	(void)guest.receive([](const std::vector<std::uint8_t> &frame) {
		return frame.size() >= 14 && get16(frame, 12) == 0x0806;
	});
	return true;
}

/* An outage that was reported is over when the provider stops: a reopen whose
 * monitor has not reported yet (none runs here) is answered as a host with a
 * path is. */
void check_reopen_after_outage(uml::network::SlirpProvider &provider,
			       Guest &guest)
{
	const char *label = "a reopen after an outage";
	auto &path = provider.path_state_for_testing();
	path.report(false);
	provider.stop();
	if (!open_provider(provider, guest)) {
		fail(label, "the provider did not reopen");
		return;
	}
	if (!path.usable())
		fail(label, "the reopened provider starts in the old outage");
	if (!is_negative(ask(guest, "uml-dns-probe-reopen.invalid", 0x7a0c,
			     42200)))
		fail(label, "a missing name was not a plain negative answer");
}

/* Ends the provider's monitor: an open that no monitor is asked for drops the
 * one it holds, and the state is the one of a host with a path again. */
void drop_path_monitor(uml::network::SlirpProvider &provider, Guest &guest)
{
	const char *label = "a dropped path monitor";
	auto &path = provider.path_state_for_testing();
	/* What the monitor reported last, written here as the system would. */
	path.report(false);
	provider.set_path_monitor_for_testing(false);
	if (!open_provider(provider, guest)) {
		fail(label, "the provider did not open");
		return;
	}
	if (provider.path_monitor_for_testing() != nullptr)
		fail(label, "the provider kept a monitor that was no longer asked "
			    "for");
	if (!path.usable())
		fail(label, "the last report of a dropped monitor stayed the state");
	provider.stop();
}

/* The provider starts the system's monitor at open() and hears its report of
 * that open, whichever path this host has, and the one monitor serves its life.
 * What the report says is the host's own and may move at any moment, so only
 * that it arrives and how many monitors were made are checked here; what the
 * state does to an answer is checked with a monitor that writes nothing. */
void check_path_monitor(uml::network::SlirpProvider &provider, Guest &guest)
{
	const char *label = "the system's path monitor";
	provider.set_path_monitor_for_testing(true);
	const auto made_before = provider.path_monitors_made_for_testing();
	/* A report that counts is one of this open: the earlier reports here
	 * were made by the probe itself. */
	const auto reports_before = provider.path_state_for_testing().reports();
	if (!open_provider(provider, guest)) {
		fail(label, "the provider did not open");
		provider.set_path_monitor_for_testing(false);
		return;
	}
	auto &path = provider.path_state_for_testing();
	const auto deadline = Clock::now() + std::chrono::seconds{10};
	while (path.reports() == reports_before && Clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds{5});
	if (path.reports() == reports_before) {
		fail(label, "the system did not report the path");
	} else {
		std::printf("%s: the host's path is %s\n", label,
			    path.usable() ? "usable" : "down");
		/* A guest's restart stops and opens the provider again, and the
		 * monitor lives on. */
		provider.stop();
		if (!open_provider(provider, guest))
			fail(label, "the provider did not reopen");
		else if (provider.path_monitors_made_for_testing() !=
			 made_before + 1)
			fail(label, "the monitor was made again at the reopen");
	}
	provider.stop();
	drop_path_monitor(provider, guest);
}

/* A monitor that reports nothing, for a check that writes the state itself as
 * the system would: the only writer is the probe. It says it failed when it was
 * made to. */
class ControlledMonitor final : public uml::platform::HostNetworkPathMonitor {
public:
	explicit ControlledMonitor(bool failed) : failed_(failed) {}
	[[nodiscard]] bool failed() const noexcept override { return failed_; }

private:
	const bool failed_;
};

/* While a monitor that works lives, a stop does not end an outage it reported
 * (a guest's restart stops and opens the provider again): a reopen still tells
 * a guest to retry, until a report says the path is back. A monitor the system
 * gave no monitor reports nothing, so a stop ends an outage as with no monitor
 * at all. */
void check_state_across_a_restart(uml::network::SlirpProvider &provider,
				  Guest &guest)
{
	const char *label = "the state across a restart";
	auto &path = provider.path_state_for_testing();
	provider.set_path_monitor_for_testing(true);
	provider.set_path_monitor_factory_for_testing(
		[](uml::platform::HostNetworkPathState &) {
			return std::make_unique<ControlledMonitor>(false);
		});
	const auto made_before = provider.path_monitors_made_for_testing();
	if (!open_provider(provider, guest)) {
		fail(label, "the provider did not open");
	} else {
		path.report(false);
		if (!is_servfail(ask(guest, "uml-dns-probe-monitor.invalid",
				     0x7a10, 42100)))
			fail(label, "a missing name was not told to retry");
		provider.stop();
		if (path.usable())
			fail(label, "the stop ended an outage that a monitor which "
				    "works keeps");
		if (!open_provider(provider, guest)) {
			fail(label, "the provider did not reopen");
		} else {
			if (!is_servfail(ask(guest, "uml-dns-probe-monitor.invalid",
					     0x7a11, 42101)))
				fail(label, "a reopen during an outage did not tell "
					    "a guest to retry");
			path.report(true);
			if (!is_negative(ask(guest, "uml-dns-probe-monitor.invalid",
					     0x7a12, 42102)))
				fail(label, "a path that is back did not end the "
					    "outage");
			if (provider.path_monitors_made_for_testing() !=
			    made_before + 1)
				fail(label, "the monitor was made again at the "
					    "reopen");
		}
	}
	provider.stop();
	drop_path_monitor(provider, guest);

	provider.set_path_monitor_for_testing(true);
	provider.set_path_monitor_factory_for_testing(
		[](uml::platform::HostNetworkPathState &) {
			return std::make_unique<ControlledMonitor>(true);
		});
	if (!open_provider(provider, guest)) {
		fail(label, "the provider did not open with a failed monitor");
	} else {
		path.report(false);
		provider.stop();
		if (!path.usable())
			fail(label, "the stop kept an outage that a monitor the "
				    "system did not give cannot end");
	}
	provider.stop();
	provider.set_path_monitor_factory_for_testing({});
	drop_path_monitor(provider, guest);
}

/* The system's creation fails for the first monitor only. */
std::atomic<int> system_creations;

bool only_the_first_system_creation_fails()
{
	return system_creations.fetch_add(1) != 0;
}

/* The system gives the provider no path monitor at its first open: the monitor
 * says so (nothing waits for its report), the next open makes another, whose
 * report arrives, and that one, which works, is kept however often the provider
 * stops and opens. */
void check_a_monitor_the_system_did_not_give(
	uml::network::SlirpProvider &provider, Guest &guest)
{
	const char *label = "a path monitor the system did not give";
	auto &path = provider.path_state_for_testing();
	system_creations = 0;
	provider.set_path_monitor_factory_for_testing(
		[](uml::platform::HostNetworkPathState &state) {
			return uml::platform::makeAppleHostNetworkPathMonitor(
				state, only_the_first_system_creation_fails);
		});
	provider.set_path_monitor_for_testing(true);
	const auto made_before = provider.path_monitors_made_for_testing();
	const auto reports_before = path.reports();
	if (!open_provider(provider, guest)) {
		fail(label, "the provider did not open");
		provider.set_path_monitor_factory_for_testing({});
		provider.set_path_monitor_for_testing(false);
		return;
	}
	const auto *first = provider.path_monitor_for_testing();
	const auto deadline = Clock::now() + std::chrono::seconds{10};
	while (first != nullptr && !first->failed() && Clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds{5});
	if (first == nullptr || !first->failed())
		fail(label, "a monitor the system did not give did not say so");
	provider.stop();
	if (!open_provider(provider, guest)) {
		fail(label, "the provider did not reopen");
	} else {
		const auto report_deadline = Clock::now() + std::chrono::seconds{10};
		while (path.reports() == reports_before &&
		       Clock::now() < report_deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds{5});
		if (path.reports() == reports_before)
			fail(label, "the reopen did not make another monitor");
		if (system_creations.load() != 2)
			fail(label, "the system was not asked a second time");
		const auto *second = provider.path_monitor_for_testing();
		if (second == nullptr || second->failed())
			fail(label, "the reopen did not hold a monitor that works");
		/* A monitor that works stays, as at every stop and reopen. */
		provider.stop();
		if (!open_provider(provider, guest))
			fail(label, "the provider did not reopen again");
		else if (provider.path_monitors_made_for_testing() !=
				 made_before + 2 ||
			 system_creations.load() != 2)
			fail(label, "a monitor that works was made again");
	}
	provider.stop();
	provider.set_path_monitor_factory_for_testing({});
	drop_path_monitor(provider, guest);
}

/* Where the resolver bound its DNS-over-TCP socket, for the log of a run on a
 * device or in the Simulator. */
void report_stream(const char *label, uml::network::SlirpProvider &provider)
{
	auto &resolver = provider.resolver_for_testing();
	std::printf("%s: DNS over TCP %s\n", label,
		    resolver.streams_available() ?
			    resolver.stream_path().c_str() : "is unavailable");
}

bool starts_with(const std::string &text, const std::string &prefix)
{
	return text.compare(0, prefix.size(), prefix) == 0;
}

/* The socket for DNS over TCP goes where $TMPDIR names, when that leaves room
 * in sun_path (an iOS device's container tmp does), and in a shorter
 * directory when it does not (an iOS Simulator's does not). */
void check_stream_directory(uml::network::SlirpProvider &provider, Guest &guest,
			    const char *label, const std::string &directory,
			    bool expect_inside)
{
	::setenv("TMPDIR", directory.c_str(), 1);
	if (!open_provider(provider, guest)) {
		fail(label, "the provider did not open");
		return;
	}
	report_stream(label, provider);
	auto &resolver = provider.resolver_for_testing();
	if (!resolver.streams_available())
		fail(label, "the resolver has no DNS-over-TCP listener");
	else if (starts_with(resolver.stream_path(), directory) != expect_inside)
		fail(label, expect_inside ?
			"the socket is not in $TMPDIR" :
			"the socket is in a directory too long for sun_path");
	else if (resolver.stream_path().size() >= sizeof(sockaddr_un::sun_path))
		fail(label, "the socket path does not fit sun_path");
	check_tcp(provider, guest);
	provider.stop();
}

/* A Unix socket's name left in the file system, as a killed process leaves. */
void leave_socket(const std::string &path)
{
	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
	const int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (descriptor < 0)
		return;
	(void)::bind(descriptor, reinterpret_cast<sockaddr *>(&address),
		     sizeof(address));
	(void)::close(descriptor);
}

/* The pid of a process that has exited and been collected. */
pid_t exited_process()
{
	const pid_t child = ::fork();
	if (child == 0)
		::_exit(0);
	int status = 0;
	if (child > 0)
		(void)::waitpid(child, &status, 0);
	return child;
}

bool exists(const std::string &path)
{
	struct stat details{};
	return ::lstat(path.c_str(), &details) == 0;
}

/* A run killed outright leaves its DNS-over-TCP socket. The next run in that
 * directory removes it, and nothing else: not a live process's socket, not a
 * file that is no socket, not another name. */
void check_abandoned_streams(uml::network::SlirpProvider &provider,
			     Guest &guest)
{
	const char *label = "a killed run's DNS-over-TCP socket";
	char base[] = "/tmp/uml-dns-probe.XXXXXX";
	if (::mkdtemp(base) == nullptr) {
		fail(label, "no scratch directory");
		return;
	}
	const std::string directory = std::string(base) + "/";
	const std::string prefix{uml::network::dns_stream_prefix};
	const pid_t dead = exited_process();
	const pid_t other_dead = exited_process();
	const auto stale = directory + prefix + std::to_string(dead);
	const auto live = directory + prefix + std::to_string(::getppid());
	const auto regular = directory + prefix + std::to_string(other_dead);
	const auto unrelated = directory + "mconsole";
	leave_socket(stale);
	leave_socket(live);
	leave_socket(unrelated);
	(void)::close(::open(regular.c_str(), O_CREAT | O_WRONLY, 0600));

	::setenv("TMPDIR", directory.c_str(), 1);
	if (!open_provider(provider, guest)) {
		fail(label, "the provider did not open");
	} else {
		const std::string own = provider.resolver_for_testing().stream_path();
		if (exists(stale))
			fail(label, "the socket of a process that exited was kept");
		if (!exists(live))
			fail(label, "the socket of a live process was removed");
		if (!exists(regular))
			fail(label, "a file that is no socket was removed");
		if (!exists(unrelated))
			fail(label, "a socket of another name was removed");
		if (!starts_with(own, directory) || !exists(own))
			fail(label, "the run has no socket of its own");
		provider.stop();
		if (exists(own))
			fail(label, "the run's socket outlived it");
	}
	std::error_code ignored;
	std::filesystem::remove_all(base, ignored);
}

} // namespace

int main()
{
	const char *original = std::getenv("TMPDIR");
	const std::string saved_directory = original != nullptr ? original : "";
	uml::network::SlirpProvider provider;
	Guest guest{provider};

	if (!open_provider(provider, guest)) {
		std::fprintf(stderr, "the provider did not open\n");
		return 1;
	}
	if (!provider.resolver_for_testing().streams_available())
		fail("TCP", "the resolver has no DNS-over-TCP listener");
	report_stream(saved_directory.empty() ? "default $TMPDIR (unset)" :
			      "default $TMPDIR", provider);
	check_udp(provider, guest);
	check_tcp(provider, guest);
	check_outage(provider, guest);
	check_reopen_after_outage(provider, guest);

	/* A guest restart: the resolver comes back with the link. */
	provider.stop();
	if (!open_provider(provider, guest)) {
		std::fprintf(stderr, "the provider did not reopen\n");
		return 1;
	}
	check_udp(provider, guest);
	provider.stop();
	check_path_monitor(provider, guest);
	check_a_monitor_the_system_did_not_give(provider, guest);
	check_state_across_a_restart(provider, guest);

	provider.set_dns_streams_unavailable_for_testing(true);
	if (open_provider(provider, guest)) {
		if (provider.resolver_for_testing().streams_available())
			fail("TCP without a stream socket",
			     "a listener exists though no path was offered");
		check_tcp_without_stream(provider, guest);
		provider.stop();
	} else {
		fail("TCP without a stream socket", "the provider did not open");
	}
	provider.set_dns_streams_unavailable_for_testing(false);

	/* An iOS device's container tmp is 89 bytes and the socket's name fits
	 * it; the Simulator's is about 164 and does not. */
	char base[] = "/tmp/uml-dns-probe.XXXXXX";
	if (::mkdtemp(base) == nullptr) {
		fail("stream directory", "no scratch directory");
	} else {
		const std::string container = std::string(base) + "/" +
			std::string(52, 't') + "/";
		if (::mkdir((container.substr(0, container.size() - 1)).c_str(),
			    0700) != 0)
			fail("stream directory", "no container directory");
		else
			check_stream_directory(provider, guest,
					       "a container tmp that fits",
					       container, true);
		std::error_code ignored;
		std::filesystem::remove_all(base, ignored);
	}
	check_stream_directory(provider, guest, "a container tmp too long",
			       "/" + std::string(150, 'l') + "/tmp/", false);
	check_abandoned_streams(provider, guest);

	if (original != nullptr)
		::setenv("TMPDIR", saved_directory.c_str(), 1);
	else
		::unsetenv("TMPDIR");

	if (failures != 0) {
		std::fprintf(stderr, "%d DNS check(s) failed\n", failures);
		return 1;
	}
	std::puts("guest DNS is answered by the system resolver");
	return 0;
}
