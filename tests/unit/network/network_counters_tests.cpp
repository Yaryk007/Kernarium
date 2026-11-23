// SPDX-License-Identifier: GPL-2.0-or-later
//
// What the guest's user network counts (network_counters.hpp): how a system
// resolver's error is classed, and how the frames that cross the provider
// become DHCP and TCP connection counts. Every rule is a function of bytes and
// of the clock a test hands in, so these tests need no socket and no guest.

#include "network_counters.hpp"
#include "require.hpp"

#include <dns_sd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <type_traits>
#include <vector>

namespace {

using uml::network::DnsOutcome;
using uml::network::NetworkCounters;
using uml::network::NetworkCounts;
using uml::network::TrafficObserver;
using uml::testing::require;
using Bytes = std::vector<std::uint8_t>;
using Address = std::array<std::uint8_t, 4>;
using Clock = TrafficObserver::Clock;

constexpr Address guest{10, 0, 2, 15};
constexpr Address gateway{10, 0, 2, 2};
constexpr Address name_server{10, 0, 2, 3};
constexpr Address internet{93, 184, 216, 34};
constexpr Address elsewhere{198, 51, 100, 7};

constexpr std::uint8_t tcp_syn = 0x02;
constexpr std::uint8_t tcp_rst = 0x04;
constexpr std::uint8_t tcp_psh = 0x08;
constexpr std::uint8_t tcp_ack = 0x10;

void put16(Bytes &bytes, std::size_t offset, std::uint16_t value)
{
	bytes[offset] = static_cast<std::uint8_t>(value >> 8);
	bytes[offset + 1] = static_cast<std::uint8_t>(value);
}

/* An Ethernet frame carrying `transport`. */
Bytes ip_frame(const Address &source, const Address &destination,
	       std::uint8_t protocol, const Bytes &transport)
{
	Bytes frame(14 + 20 + transport.size(), 0);
	put16(frame, 12, 0x0800);
	frame[14] = 0x45;
	put16(frame, 16, static_cast<std::uint16_t>(20 + transport.size()));
	put16(frame, 20, 0x4000);
	frame[22] = 64;
	frame[23] = protocol;
	std::copy(source.begin(), source.end(), frame.begin() + 26);
	std::copy(destination.begin(), destination.end(), frame.begin() + 30);
	std::copy(transport.begin(), transport.end(), frame.begin() + 34);
	return frame;
}

Bytes tcp_header(std::uint16_t source_port, std::uint16_t destination_port,
		 std::uint8_t flags)
{
	Bytes tcp(20, 0);
	put16(tcp, 0, source_port);
	put16(tcp, 2, destination_port);
	tcp[12] = 5 << 4;
	tcp[13] = flags;
	return tcp;
}

Bytes tcp_frame(const Address &source, const Address &destination,
		std::uint16_t source_port, std::uint16_t destination_port,
		std::uint8_t flags)
{
	return ip_frame(source, destination, 6,
			tcp_header(source_port, destination_port, flags));
}

Bytes guest_syn(const Address &destination, std::uint16_t guest_port,
		std::uint16_t port = 443)
{
	return tcp_frame(guest, destination, guest_port, port, tcp_syn);
}

/* What SLiRP says to a SYN it could not make: a reset acknowledging it. */
Bytes reset_for(const Address &remote, std::uint16_t guest_port,
		std::uint16_t port = 443)
{
	return tcp_frame(remote, guest, port, guest_port, tcp_rst | tcp_ack);
}

Bytes syn_ack_for(const Address &remote, std::uint16_t guest_port,
		  std::uint16_t port = 443)
{
	return tcp_frame(remote, guest, port, guest_port, tcp_syn | tcp_ack);
}

/* An ICMP destination unreachable from the gateway, quoting the SYN as SLiRP
 * does: the IP header and the first eight bytes of the TCP header. */
Bytes unreachable_for(const Address &remote, std::uint16_t guest_port,
		      std::uint8_t code, std::uint16_t port = 443)
{
	const Bytes quoted = guest_syn(remote, guest_port, port);
	Bytes icmp(8, 0);
	icmp[0] = 3;
	icmp[1] = code;
	icmp.insert(icmp.end(), quoted.begin() + 14, quoted.begin() + 14 + 28);
	return ip_frame(gateway, guest, 1, icmp);
}

/* A BOOTP message from a client (op 1) or a server (op 2) with DHCP message
 * type `type` among other options, in a UDP datagram. */
Bytes dhcp_frame(bool from_guest, std::uint8_t type, bool with_cookie = true)
{
	Bytes bootp(236 + 4, 0);
	bootp[0] = from_guest ? 1 : 2;
	if (with_cookie) {
		bootp[236] = 0x63;
		bootp[237] = 0x82;
		bootp[238] = 0x53;
		bootp[239] = 0x63;
	}
	/* A pad, a host name option, then the message type, then the end. */
	for (const std::uint8_t byte : std::initializer_list<std::uint8_t>{
		     0, 12, 3, 'a', 'b', 'c', 53, 1, type, 255})
		bootp.push_back(byte);
	Bytes udp(8, 0);
	put16(udp, 0, from_guest ? 68 : 67);
	put16(udp, 2, from_guest ? 67 : 68);
	put16(udp, 4, static_cast<std::uint16_t>(8 + bootp.size()));
	udp.insert(udp.end(), bootp.begin(), bootp.end());
	return from_guest ?
		ip_frame({0, 0, 0, 0}, {255, 255, 255, 255}, 17, udp) :
		ip_frame(gateway, guest, 17, udp);
}

const Clock::time_point start{std::chrono::seconds{1000}};

/* An address as the observer is told it: the four bytes read big endian. */
constexpr std::uint32_t value_of(const Address &address)
{
	return (std::uint32_t{address[0]} << 24) |
		(std::uint32_t{address[1]} << 16) |
		(std::uint32_t{address[2]} << 8) | address[3];
}

void everyCountIsANumber()
{
	static_assert(std::is_trivially_copyable_v<NetworkCounts>);
	/* Eleven counts and nothing else: no address, port or name has a field to
	 * sit in. */
	static_assert(sizeof(NetworkCounts) == 11 * sizeof(std::uint64_t));
	static_assert(std::is_standard_layout_v<NetworkCounts>);
}

void aResolverErrorHasAnOutcome()
{
	require(uml::network::classify_dns_error(kDNSServiceErr_NoError) ==
		DnsOutcome::answered);
	/* No such name and no such record are answers. */
	require(uml::network::classify_dns_error(kDNSServiceErr_NoSuchName) ==
		DnsOutcome::answered);
	require(uml::network::classify_dns_error(kDNSServiceErr_NoSuchRecord) ==
		DnsOutcome::answered);
	require(uml::network::classify_dns_error(kDNSServiceErr_PolicyDenied) ==
		DnsOutcome::policy_denied);
	require(uml::network::classify_dns_error(kDNSServiceErr_NotPermitted) ==
		DnsOutcome::policy_denied);
	require(uml::network::classify_dns_error(kDNSServiceErr_Timeout) ==
		DnsOutcome::timed_out);
	for (const std::int32_t error : std::initializer_list<std::int32_t>{
		     kDNSServiceErr_Unknown, kDNSServiceErr_NoMemory,
		     kDNSServiceErr_ServiceNotRunning,
		     kDNSServiceErr_DefunctConnection, kDNSServiceErr_NoRouter,
		     kDNSServiceErr_Refused, -1, 12345})
		require(uml::network::classify_dns_error(error) ==
			DnsOutcome::failed);
}

void outcomesAreCountedApart()
{
	NetworkCounters counters;
	require(counters.read().dns_answered == 0);
	counters.dns(DnsOutcome::answered);
	counters.dns(DnsOutcome::answered);
	counters.dns(DnsOutcome::policy_denied);
	counters.dns(DnsOutcome::timed_out);
	counters.dns(DnsOutcome::failed);
	counters.dns(DnsOutcome::failed);
	counters.dns(DnsOutcome::failed);
	const auto counts = counters.read();
	require(counts.dns_answered == 2 && counts.dns_policy_denied == 1 &&
		counts.dns_timed_out == 1 && counts.dns_failed == 3);
	require(counts.dhcp_discovers == 0 && counts.tcp_attempts == 0);
}

void dhcpExchangesAreCounted()
{
	NetworkCounters counters;
	TrafficObserver observer{counters};
	observer.from_guest(dhcp_frame(true, 1), start);
	observer.from_network(dhcp_frame(false, 2));
	observer.from_guest(dhcp_frame(true, 3), start);
	observer.from_network(dhcp_frame(false, 5));
	auto counts = counters.read();
	/* The offer is neither a request nor a lease. */
	require(counts.dhcp_discovers == 1 && counts.dhcp_requests == 1 &&
		counts.dhcp_acks == 1);
	/* A NAK, a release and a client message in the server's direction are
	 * not counted; neither is a message without the DHCP cookie. */
	observer.from_network(dhcp_frame(false, 6));
	observer.from_guest(dhcp_frame(true, 7), start);
	observer.from_network(dhcp_frame(true, 5));
	observer.from_guest(dhcp_frame(true, 1, false), start);
	counts = counters.read();
	require(counts.dhcp_discovers == 1 && counts.dhcp_requests == 1 &&
		counts.dhcp_acks == 1);
	/* A guest that asks again is counted again. */
	observer.from_guest(dhcp_frame(true, 3), start);
	require(counters.read().dhcp_requests == 2);
}

void aConnectionThatIsAnsweredIsCountedOnce()
{
	NetworkCounters counters;
	TrafficObserver observer{counters};
	observer.from_guest(guest_syn(internet, 40000), start);
	require(counters.read().tcp_attempts == 1);
	observer.from_network(syn_ack_for(internet, 40000));
	auto counts = counters.read();
	require(counts.tcp_attempts == 1 && counts.tcp_established == 1 &&
		counts.tcp_unreachable == 0 && counts.tcp_reset == 0 &&
		counts.tcp_unanswered() == 0);
	/* The same answer again, or a reset of the established connection later,
	 * answers nothing: the attempt is resolved. */
	observer.from_network(syn_ack_for(internet, 40000));
	observer.from_network(reset_for(internet, 40000));
	counts = counters.read();
	require(counts.tcp_established == 1 && counts.tcp_reset == 0);
}

void aResetAndAnUnreachableNetworkAreClassed()
{
	NetworkCounters counters;
	TrafficObserver observer{counters};
	observer.from_guest(guest_syn(internet, 40000), start);
	observer.from_network(reset_for(internet, 40000));
	observer.from_guest(guest_syn(elsewhere, 40001, 8443), start);
	observer.from_network(unreachable_for(elsewhere, 40001, 0, 8443));
	observer.from_guest(guest_syn(elsewhere, 40002, 80), start);
	observer.from_network(unreachable_for(elsewhere, 40002, 1, 80));
	const auto counts = counters.read();
	require(counts.tcp_attempts == 3 && counts.tcp_reset == 1 &&
		counts.tcp_unreachable == 2 && counts.tcp_established == 0 &&
		counts.tcp_unanswered() == 0);
}

void onlyNetAndHostUnreachableAreUnreachable()
{
	NetworkCounters counters;
	TrafficObserver observer{counters};
	/* SLiRP sends codes 0 and 1 for a connect() that fails at once. The
	 * others (protocol, port, fragmentation needed, administratively
	 * prohibited) are not that, and leave the attempt unanswered. */
	std::uint16_t port = 40000;
	for (const std::uint8_t code : std::initializer_list<std::uint8_t>{
		     2, 3, 4, 9, 10, 13, 255}) {
		observer.from_guest(guest_syn(elsewhere, port), start);
		observer.from_network(unreachable_for(elsewhere, port, code));
		++port;
	}
	auto counts = counters.read();
	require(counts.tcp_attempts == 7 && counts.tcp_unreachable == 0 &&
		counts.tcp_unanswered() == 7);
	for (const std::uint8_t code : std::initializer_list<std::uint8_t>{0, 1}) {
		observer.from_guest(guest_syn(elsewhere, port), start);
		observer.from_network(unreachable_for(elsewhere, port, code));
		++port;
	}
	counts = counters.read();
	require(counts.tcp_attempts == 9 && counts.tcp_unreachable == 2 &&
		counts.tcp_unanswered() == 7);
}

void dnsOverTcpToTheGuestForwardIsNotAnAttempt()
{
	NetworkCounters counters;
	TrafficObserver observer{counters};
	/* The provider has put the guest forward on the name server and the
	 * gateway: SLiRP answers a SYN to port 53 of either itself. */
	observer.serve_dns_streams(value_of(name_server), value_of(gateway));
	observer.from_guest(guest_syn(name_server, 40000, 53), start);
	observer.from_guest(guest_syn(gateway, 40001, 53), start);
	/* Its SYN-ACK says nothing about the network either. */
	observer.from_network(syn_ack_for(name_server, 40000, 53));
	observer.from_network(syn_ack_for(gateway, 40001, 53));
	auto counts = counters.read();
	require(counts.tcp_attempts == 0 && counts.tcp_established == 0 &&
		counts.tcp_unanswered() == 0);
	/* Another port of the same two hosts, port 53 of any other address, and
	 * a retransmission of the skipped SYN are what they look like. */
	observer.from_guest(guest_syn(gateway, 40002, 443), start);
	observer.from_guest(guest_syn(name_server, 40003, 5353), start);
	observer.from_guest(guest_syn(internet, 40004, 53), start);
	observer.from_guest(guest_syn(name_server, 40000, 53),
			    start + std::chrono::seconds{1});
	counts = counters.read();
	require(counts.tcp_attempts == 3 && counts.tcp_unanswered() == 3);
	observer.from_network(syn_ack_for(internet, 40004, 53));
	require(counters.read().tcp_established == 1);
}

void dnsOverTcpIsAnAttemptWhereNothingAnswersIt()
{
	NetworkCounters counters;
	TrafficObserver observer{counters};
	/* No forward on the gateway, only on the name server (the provider
	 * passes 0 for an address with none); a zero address is no server. */
	observer.serve_dns_streams(value_of(name_server), 0);
	observer.from_guest(guest_syn(name_server, 40000, 53), start);
	observer.from_guest(guest_syn(gateway, 40001, 53), start);
	require(counters.read().tcp_attempts == 1);
	/* And with neither, the SYN reaches SLiRP like any other. */
	NetworkCounters bare;
	TrafficObserver none{bare};
	none.serve_dns_streams(0, 0);
	none.from_guest(guest_syn(name_server, 40000, 53), start);
	none.from_guest(guest_syn({0, 0, 0, 0}, 40001, 53), start);
	require(bare.read().tcp_attempts == 2);
}

void theClockIsReadForASynAlone()
{
	NetworkCounters counters;
	TrafficObserver observer{counters};
	/* The overload without a time reads the clock when a SYN needs it. It
	 * counts what the one with a time does. */
	observer.from_guest(guest_syn(internet, 40000));
	observer.from_guest(guest_syn(internet, 40000));
	require(counters.read().tcp_attempts == 1);
	observer.from_network(syn_ack_for(internet, 40000));
	require(counters.read().tcp_established == 1);
	observer.from_guest(dhcp_frame(true, 1));
	observer.from_guest(tcp_frame(guest, internet, 40001, 443, tcp_ack));
	Bytes udp = ip_frame(guest, internet, 17, Bytes(20, 0));
	observer.from_guest(udp);
	observer.from_guest({});
	const auto counts = counters.read();
	require(counts.tcp_attempts == 1 && counts.dhcp_discovers == 1);
}

void anAnswerForAnotherConnectionIsIgnored()
{
	NetworkCounters counters;
	TrafficObserver observer{counters};
	observer.from_guest(guest_syn(internet, 40000), start);
	/* Another guest port, another remote port, another remote address, and
	 * an unreachable error that quotes a connection nobody attempted. */
	observer.from_network(reset_for(internet, 40001));
	observer.from_network(reset_for(internet, 40000, 80));
	observer.from_network(reset_for(elsewhere, 40000));
	observer.from_network(unreachable_for(internet, 40009, 0));
	auto counts = counters.read();
	require(counts.tcp_attempts == 1 && counts.tcp_reset == 0 &&
		counts.tcp_unreachable == 0 && counts.tcp_unanswered() == 1);
	/* The attempt nothing answered is the difference of the counts. */
	observer.from_network(syn_ack_for(internet, 40000));
	require(counters.read().tcp_unanswered() == 0);
}

void aRetransmittedSynIsNotAnotherAttempt()
{
	NetworkCounters counters;
	TrafficObserver observer{counters};
	observer.from_guest(guest_syn(internet, 40000), start);
	for (const int seconds : {1, 3, 7, 15, 31, 63, 127})
		observer.from_guest(guest_syn(internet, 40000),
				    start + std::chrono::seconds{seconds});
	require(counters.read().tcp_attempts == 1 &&
		counters.read().tcp_unanswered() == 1);
	/* The guest gave up long ago: a SYN from the same port is a new try. */
	observer.from_guest(guest_syn(internet, 40000),
			    start + TrafficObserver::retransmission_window);
	require(counters.read().tcp_attempts == 2);
	/* Another destination from the same port is another attempt too. */
	observer.from_guest(guest_syn(elsewhere, 40000),
			    start + std::chrono::seconds{1});
	require(counters.read().tcp_attempts == 3);
}

void everyConnectionHandedToSlirpIsCounted()
{
	NetworkCounters counters;
	TrafficObserver observer{counters};
	/* The virtual host included: SLiRP connects to it too. Without a guest
	 * forward on port 53 there is no exception either. */
	for (const Address &destination :
	     {gateway, Address{10, 0, 3, 1}, Address{127, 0, 0, 1},
	      Address{224, 0, 0, 1}})
		observer.from_guest(guest_syn(destination, 40000), start);
	require(counters.read().tcp_attempts == 4);
}

void onlyAConnectionRequestIsAnAttempt()
{
	NetworkCounters counters;
	TrafficObserver observer{counters};
	for (const std::uint8_t flags :
	     {static_cast<std::uint8_t>(tcp_syn | tcp_ack),
	      static_cast<std::uint8_t>(tcp_syn | tcp_rst),
	      static_cast<std::uint8_t>(tcp_ack),
	      static_cast<std::uint8_t>(tcp_ack | tcp_psh),
	      static_cast<std::uint8_t>(tcp_rst), std::uint8_t{0}})
		observer.from_guest(
			tcp_frame(guest, internet, 40000, 443, flags), start);
	require(counters.read().tcp_attempts == 0);
	/* Not IPv4, not TCP, truncated, a later fragment. */
	Bytes arp(42, 0);
	put16(arp, 12, 0x0806);
	observer.from_guest(arp, start);
	Bytes udp = ip_frame(guest, internet, 17, Bytes(20, 0));
	observer.from_guest(udp, start);
	Bytes truncated = guest_syn(internet, 40000);
	truncated.resize(40);
	observer.from_guest(truncated, start);
	Bytes fragment = guest_syn(internet, 40000);
	put16(fragment, 20, 0x00b9);
	observer.from_guest(fragment, start);
	observer.from_guest({}, start);
	observer.from_network({});
	require(counters.read().tcp_attempts == 0);
}

void aFullTableGivesUpItsOldestAttempt()
{
	NetworkCounters counters;
	TrafficObserver observer{counters};
	constexpr auto limit = TrafficObserver::pending_limit;
	for (std::size_t index = 0; index < limit; ++index)
		observer.from_guest(
			guest_syn(internet, static_cast<std::uint16_t>(50000 + index)),
			start + std::chrono::milliseconds{index});
	require(counters.read().tcp_attempts == limit);
	/* One more attempt takes the oldest one's place... */
	observer.from_guest(guest_syn(internet, 60000),
			    start + std::chrono::seconds{1});
	require(counters.read().tcp_attempts == limit + 1);
	/* ...so the oldest can no longer be answered, and the newest can. */
	observer.from_network(syn_ack_for(internet, 50000));
	require(counters.read().tcp_established == 0);
	observer.from_network(syn_ack_for(internet, 60000));
	observer.from_network(syn_ack_for(internet, 50001));
	require(counters.read().tcp_established == 2);
}

void aNewLinkForgetsTheAttemptsOfTheLast()
{
	NetworkCounters counters;
	TrafficObserver observer{counters};
	observer.from_guest(guest_syn(internet, 40000), start);
	observer.reset();
	observer.from_network(syn_ack_for(internet, 40000));
	require(counters.read().tcp_attempts == 1 &&
		counters.read().tcp_established == 0);
	/* A counter never goes back: the provider's reading only grows. */
	observer.from_guest(guest_syn(internet, 40000), start);
	require(counters.read().tcp_attempts == 2);
}

} // namespace

int main()
{
	everyCountIsANumber();
	aResolverErrorHasAnOutcome();
	outcomesAreCountedApart();
	dhcpExchangesAreCounted();
	aConnectionThatIsAnsweredIsCountedOnce();
	aResetAndAnUnreachableNetworkAreClassed();
	onlyNetAndHostUnreachableAreUnreachable();
	dnsOverTcpToTheGuestForwardIsNotAnAttempt();
	dnsOverTcpIsAnAttemptWhereNothingAnswersIt();
	theClockIsReadForASynAlone();
	anAnswerForAnotherConnectionIsIgnored();
	aRetransmittedSynIsNotAnotherAttempt();
	everyConnectionHandedToSlirpIsCounted();
	onlyAConnectionRequestIsAnAttempt();
	aFullTableGivesUpItsOldestAttempt();
	aNewLinkForgetsTheAttemptsOfTheLast();
	return EXIT_SUCCESS;
}
