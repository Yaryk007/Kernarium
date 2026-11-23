// SPDX-License-Identifier: GPL-2.0-or-later
//
// The rule that keeps the guest off the host itself: the table of the host's
// addresses, which guest frames the provider refuses because they address one
// of them, which frames make the provider look the table up again, and what a
// refusal looks like on the wire. Every rule is a pure function of bytes and
// of the table, so these tests need no socket and no guest.

#include "dns_frames.hpp"
#include "host_addresses.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <utility>
#include <vector>

namespace {

using uml::testing::require;
namespace dns = uml::network::dns;
using Bytes = std::vector<std::uint8_t>;
using dns::FrameAction;
using dns::Ipv4Address;
using uml::network::HostAddresses;

constexpr Ipv4Address guest{10, 0, 2, 15};
constexpr Ipv4Address lan_host{192, 0, 2, 5};
constexpr Ipv4Address lan_broadcast{192, 0, 2, 255};
constexpr Ipv4Address loopback{127, 0, 0, 1};
constexpr Ipv4Address other_machine{192, 0, 2, 77};
constexpr Ipv4Address gateway{10, 0, 2, 2};
constexpr Ipv4Address name_server{10, 0, 2, 3};
/* An address of the virtual network that SLiRP serves nothing at. */
constexpr Ipv4Address virtual_host{10, 0, 2, 77};
constexpr dns::MacAddress guest_mac{0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
constexpr dns::MacAddress slirp_mac{0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};
/* What the provider serves itself. */
constexpr dns::Servers servers{name_server, gateway, true, true};

constexpr std::uint8_t protocol_icmp = 1;
constexpr std::uint8_t protocol_tcp = 6;
constexpr std::uint8_t protocol_udp = 17;
constexpr std::uint8_t tcp_fin = 0x01;
constexpr std::uint8_t tcp_syn = 0x02;
constexpr std::uint8_t tcp_rst = 0x04;
constexpr std::uint8_t tcp_psh = 0x08;
constexpr std::uint8_t tcp_ack = 0x10;
constexpr std::uint8_t tcp_urg = 0x20;
constexpr std::uint8_t tcp_ece = 0x40;
constexpr std::uint8_t tcp_cwr = 0x80;

std::uint16_t get16(const Bytes &bytes, std::size_t offset)
{
	return static_cast<std::uint16_t>((bytes[offset] << 8) | bytes[offset + 1]);
}

std::uint32_t get32(const Bytes &bytes, std::size_t offset)
{
	return (static_cast<std::uint32_t>(get16(bytes, offset)) << 16) |
		get16(bytes, offset + 2);
}

void put16(Bytes &bytes, std::size_t offset, std::uint16_t value)
{
	bytes[offset] = static_cast<std::uint8_t>(value >> 8);
	bytes[offset + 1] = static_cast<std::uint8_t>(value);
}

void put32(Bytes &bytes, std::size_t offset, std::uint32_t value)
{
	put16(bytes, offset, static_cast<std::uint16_t>(value >> 16));
	put16(bytes, offset + 2, static_cast<std::uint16_t>(value));
}

/* The Internet checksum of `bytes` after `sum`; zero over a valid packet. */
std::uint16_t checksum(const std::uint8_t *bytes, std::size_t size,
		       std::uint32_t sum = 0)
{
	for (std::size_t index = 0; index + 1 < size; index += 2)
		sum += static_cast<std::uint32_t>(bytes[index] << 8) |
			bytes[index + 1];
	if ((size & 1U) != 0)
		sum += static_cast<std::uint32_t>(bytes[size - 1]) << 8;
	while ((sum >> 16) != 0)
		sum = (sum & 0xffff) + (sum >> 16);
	return static_cast<std::uint16_t>(~sum);
}

std::span<const std::uint8_t> view(const Bytes &bytes)
{
	return {bytes.data(), bytes.size()};
}

/* An Ethernet frame from the guest. */
Bytes ip_frame(const Ipv4Address &destination, std::uint8_t protocol,
	       const Bytes &transport, std::uint16_t fragment = 0x4000)
{
	Bytes frame(14 + 20 + transport.size(), 0);
	std::copy(slirp_mac.begin(), slirp_mac.end(), frame.begin());
	std::copy(guest_mac.begin(), guest_mac.end(), frame.begin() + 6);
	put16(frame, 12, 0x0800);
	frame[14] = 0x45;
	put16(frame, 16, static_cast<std::uint16_t>(20 + transport.size()));
	put16(frame, 18, 0x1234);
	put16(frame, 20, fragment);
	frame[22] = 64;
	frame[23] = protocol;
	std::copy(guest.begin(), guest.end(), frame.begin() + 26);
	std::copy(destination.begin(), destination.end(), frame.begin() + 30);
	put16(frame, 24, checksum(&frame[14], 20));
	std::copy(transport.begin(), transport.end(), frame.begin() + 34);
	return frame;
}

Bytes udp_datagram(std::uint16_t source_port, std::uint16_t destination_port,
		   std::size_t payload = 11)
{
	Bytes udp(8 + payload, 0x61);
	put16(udp, 0, source_port);
	put16(udp, 2, destination_port);
	put16(udp, 4, static_cast<std::uint16_t>(udp.size()));
	put16(udp, 6, 0);
	return udp;
}

Bytes tcp_segment(std::uint16_t source_port, std::uint16_t destination_port,
		  std::uint32_t sequence, std::uint32_t acknowledgement,
		  std::uint8_t flags, std::size_t payload = 0)
{
	Bytes tcp(20 + payload, 0);
	put16(tcp, 0, source_port);
	put16(tcp, 2, destination_port);
	put32(tcp, 4, sequence);
	put32(tcp, 8, acknowledgement);
	tcp[12] = 5 << 4;
	tcp[13] = flags;
	put16(tcp, 14, 65535);
	return tcp;
}

HostAddresses host_with(std::vector<std::uint32_t> addresses)
{
	HostAddresses host;
	host.assign_ipv4(std::move(addresses));
	return host;
}

constexpr std::uint32_t value(const Ipv4Address &address)
{
	return (static_cast<std::uint32_t>(address[0]) << 24) |
		(static_cast<std::uint32_t>(address[1]) << 16) |
		(static_cast<std::uint32_t>(address[2]) << 8) | address[3];
}

HostAddresses the_host()
{
	return host_with({value(loopback), value(lan_host), value(lan_broadcast),
			  value(lan_host)});
}

dns::Classified classify(const Bytes &frame, const HostAddresses &host)
{
	return dns::classify_host_bound_frame(view(frame), host, servers);
}

bool needs_check(const Bytes &frame)
{
	return dns::needs_host_check(view(frame), servers);
}

void theTableHoldsEachAddressOnceAndAnswersByLookup()
{
	const auto host = the_host();
	require(host.size() == 3);
	require(host.contains(loopback) && host.contains(lan_host) &&
		host.contains(lan_broadcast));
	require(!host.contains(other_machine) && !host.contains(gateway));
	require(HostAddresses{}.empty() && !HostAddresses{}.contains(lan_host));
	/* Neither of the two addresses SLiRP answers for is an interface's. */
	const auto odd = host_with({0, 0xffffffffU, value(lan_host)});
	require(odd.size() == 1 && !odd.contains({255, 255, 255, 255}) &&
		!odd.contains({0, 0, 0, 0}));
	/* A table assigned again replaces what it held. */
	auto replaced = the_host();
	replaced.assign_ipv4({value(other_machine)});
	require(replaced.contains(other_machine) && !replaced.contains(lan_host));
}

void theSystemListsTheLoopbackAddress()
{
	const auto host = uml::network::read_host_addresses();
	require(host.has_value());
	require(host->contains(loopback));
	require(!host->contains({255, 255, 255, 255}));
}

void aSynToAHostAddressIsReset()
{
	const auto frame = ip_frame(lan_host, protocol_tcp,
		tcp_segment(40000, 5555, 1000, 0, tcp_syn));
	const auto classified = classify(frame, the_host());
	require(classified.action == FrameAction::reset);
	require(classified.endpoints.server_address == lan_host);
	require(classified.endpoints.guest_address == guest);
	require(classified.endpoints.guest_port == 40000 &&
		classified.endpoints.server_port == 5555);
	require(classified.endpoints.guest_mac == guest_mac);
	const auto reset = dns::tcp_reset_frame(classified, 77);
	require(reset.size() == 14 + 20 + 20);
	require(std::equal(guest_mac.begin(), guest_mac.end(), reset.begin()));
	require(std::equal(lan_host.begin(), lan_host.end(), reset.begin() + 26));
	require(std::equal(guest.begin(), guest.end(), reset.begin() + 30));
	require(checksum(&reset[14], 20) == 0);
	require(get16(reset, 34) == 5555 && get16(reset, 36) == 40000);
	/* RFC 9293: a reset to a SYN acknowledges it. */
	require(reset[47] == (tcp_rst | tcp_ack) && get32(reset, 42) == 1001);
}

void everyAddressTheHostHoldsIsRefused()
{
	const auto host = the_host();
	for (const auto &address : {loopback, lan_host, lan_broadcast})
		require(classify(ip_frame(address, protocol_tcp,
					  tcp_segment(40000, 80, 1, 0, tcp_syn)),
				 host).action == FrameAction::reset);
}

void onlyAConnectAttemptIsRefused()
{
	const auto host = the_host();
	/* SLiRP opens a socket for a segment whose SYN, FIN, RST, URG and ACK
	 * flags are exactly SYN; a PSH, ECE or CWR beside it changes nothing. */
	for (const std::uint8_t flags :
	     {tcp_syn, static_cast<std::uint8_t>(tcp_syn | tcp_psh),
	      static_cast<std::uint8_t>(tcp_syn | tcp_ece | tcp_cwr),
	      static_cast<std::uint8_t>(tcp_syn | tcp_ece | tcp_cwr | tcp_psh)}) {
		const auto frame = ip_frame(lan_host, protocol_tcp,
			tcp_segment(40003, 80, 700, 0, flags, 4));
		const auto classified = classify(frame, host);
		require(classified.action == FrameAction::reset);
		/* The reset acknowledges the SYN and the payload after it. */
		const auto reset = dns::tcp_reset_frame(classified, 1);
		require(reset[47] == (tcp_rst | tcp_ack) &&
			get32(reset, 42) == 700 + 1 + 4);
	}
}

void everyOtherSegmentGoesToSlirp()
{
	/* The guest's side of a flow the host opened, through an address of the
	 * host's own, to a forward: SLiRP serves it. A flow it does not know is
	 * reset by SLiRP itself. Nothing here connects to the host. */
	const auto host = the_host();
	for (const std::uint8_t flags :
	     {static_cast<std::uint8_t>(tcp_syn | tcp_ack),
	      static_cast<std::uint8_t>(tcp_syn | tcp_ack | tcp_ece),
	      tcp_ack, static_cast<std::uint8_t>(tcp_ack | tcp_psh),
	      static_cast<std::uint8_t>(tcp_fin | tcp_ack),
	      static_cast<std::uint8_t>(tcp_syn | tcp_fin), tcp_fin, tcp_rst,
	      static_cast<std::uint8_t>(tcp_rst | tcp_ack),
	      static_cast<std::uint8_t>(tcp_syn | tcp_rst),
	      static_cast<std::uint8_t>(tcp_syn | tcp_urg), tcp_urg, tcp_psh,
	      std::uint8_t{0}}) {
		const auto frame = ip_frame(lan_host, protocol_tcp,
			tcp_segment(40001, 80, 500, 9000, flags, 100));
		require(classify(frame, host).action == FrameAction::pass);
		require(classify(ip_frame(loopback, protocol_tcp,
					  tcp_segment(40001, 80, 500, 9000,
						      flags, 100)),
				 host).action == FrameAction::pass);
	}
	/* A segment that is cut short is still dropped, not passed. */
	auto cut = ip_frame(lan_host, protocol_tcp,
		tcp_segment(1, 2, 3, 4, tcp_ack));
	cut.resize(cut.size() - 12);
	put16(cut, 16, static_cast<std::uint16_t>(cut.size() - 14));
	put16(cut, 24, 0);
	put16(cut, 24, checksum(&cut[14], 20));
	require(classify(cut, host).action == FrameAction::drop);
}

void aResetIsNeverAnswered()
{
	/* A connect attempt is the only segment the provider answers, and one
	 * that carries RST is not that; a reset frame is still never built for
	 * a segment that has the flag. */
	dns::Classified segment;
	segment.tcp_flags = tcp_rst;
	require(dns::tcp_reset_frame(segment, 1).empty());
}

void aDatagramToAHostAddressGetsAPortUnreachable()
{
	const auto frame = ip_frame(lan_host, protocol_udp, udp_datagram(41000, 5353));
	const auto classified = classify(frame, the_host());
	require(classified.action == FrameAction::unreachable);
	require(classified.quote.size() == 28);
	const auto icmp = dns::icmp_unreachable_frame(classified, 9);
	require(icmp.size() == 14 + 20 + 8 + 28);
	require(std::equal(guest_mac.begin(), guest_mac.end(), icmp.begin()));
	require(icmp[23] == protocol_icmp);
	/* From the address the datagram was sent to, back to the guest. */
	require(std::equal(lan_host.begin(), lan_host.end(), icmp.begin() + 26));
	require(std::equal(guest.begin(), guest.end(), icmp.begin() + 30));
	require(checksum(&icmp[14], 20) == 0);
	/* Destination unreachable, port unreachable, a valid checksum. */
	require(icmp[34] == 3 && icmp[35] == 3);
	require(checksum(&icmp[34], icmp.size() - 34) == 0);
	/* The quote is the datagram's IP header and its first eight bytes, which
	 * hold the ports a socket matches the error by. */
	require(std::equal(frame.begin() + 14, frame.begin() + 14 + 28,
			   icmp.begin() + 42));
	require(get16(icmp, 42 + 20) == 41000 && get16(icmp, 42 + 22) == 5353);
}

void whateverElseIsAddressedToTheHostIsDropped()
{
	const auto host = the_host();
	Bytes echo(8, 0);
	echo[0] = 8;
	put16(echo, 2, checksum(echo.data(), echo.size()));
	require(classify(ip_frame(lan_host, protocol_icmp, echo), host).action ==
		FrameAction::drop);
	/* A fragment has no ports to answer by. */
	require(classify(ip_frame(lan_host, protocol_udp, udp_datagram(1, 2),
				  0x2000), host).action == FrameAction::drop);
	require(classify(ip_frame(lan_host, protocol_udp, udp_datagram(1, 2),
				  0x00b9), host).action == FrameAction::drop);
	require(classify(ip_frame(lan_host, protocol_tcp,
				  tcp_segment(1, 2, 3, 0, tcp_syn), 0x2000),
			 host).action == FrameAction::drop);
	require(classify(ip_frame(lan_host, protocol_tcp, Bytes(8, 0), 0x0003),
			 host).action == FrameAction::drop);
	/* A packet SLiRP would refuse is no reason to pass it on. */
	auto bad_checksum = ip_frame(lan_host, protocol_tcp,
		tcp_segment(1, 2, 3, 4, tcp_syn));
	bad_checksum[24] ^= 0x5a;
	require(classify(bad_checksum, host).action == FrameAction::drop);
	auto cut = ip_frame(lan_host, protocol_tcp,
		tcp_segment(1, 2, 3, 4, tcp_syn));
	cut.resize(cut.size() - 4);
	put16(cut, 16, static_cast<std::uint16_t>(cut.size() - 14));
	put16(cut, 24, 0);
	put16(cut, 24, checksum(&cut[14], 20));
	require(classify(cut, host).action == FrameAction::drop);
	auto short_datagram = ip_frame(lan_host, protocol_udp, Bytes(6, 0));
	require(classify(short_datagram, host).action == FrameAction::drop);
	require(classify(ip_frame(lan_host, 47, Bytes(16, 0)), host).action ==
		FrameAction::drop);
}

void everyOtherAddressGoesToSlirp()
{
	const auto host = the_host();
	require(classify(ip_frame(other_machine, protocol_tcp,
				  tcp_segment(40000, 80, 1, 0, tcp_syn)),
			 host).action == FrameAction::pass);
	require(classify(ip_frame(other_machine, protocol_udp, udp_datagram(1, 2)),
			 host).action == FrameAction::pass);
	/* The two addresses SLiRP answers for are not the host's, and neither
	 * is the broadcast address the guest's DHCP goes to. */
	require(classify(ip_frame({255, 255, 255, 255}, protocol_udp,
				  udp_datagram(68, 67)), host).action ==
		FrameAction::pass);
	/* An empty table refuses nothing. */
	require(classify(ip_frame(lan_host, protocol_tcp,
				  tcp_segment(40000, 80, 1, 0, tcp_syn)),
			 HostAddresses{}).action == FrameAction::pass);
}

/* SLiRP connects a flow to any address of the virtual network but the
 * gateway as given (with its DNS proxy off, to the name server too), so a host
 * address in 10.0.2.0/24 is the host's like any other, the gateway's and the
 * name server's included. */
void aHostAddressInTheVirtualNetworkIsTheHosts()
{
	const auto host = host_with({value(gateway), value(name_server),
				     value(virtual_host)});
	for (const auto &address : {gateway, name_server, virtual_host}) {
		const auto syn = classify(
			ip_frame(address, protocol_tcp,
				 tcp_segment(40000, 80, 1, 0, tcp_syn)), host);
		require(syn.action == FrameAction::reset);
		require(syn.endpoints.server_address == address);
		const auto datagram = classify(
			ip_frame(address, protocol_udp, udp_datagram(40000, 9)), host);
		require(datagram.action == FrameAction::unreachable);
		require(datagram.endpoints.server_address == address);
		/* Everything else is dropped, as for any host address. */
		require(classify(ip_frame(address, 47, Bytes(16, 0)), host).action ==
			FrameAction::drop);
		/* A segment of a flow is SLiRP's. */
		require(classify(ip_frame(address, protocol_tcp,
					  tcp_segment(40000, 80, 1, 2, tcp_ack)),
				 host).action == FrameAction::pass);
	}
	/* An address the host does not hold is SLiRP's whatever the network. */
	const auto neighbour = host_with({value(virtual_host)});
	for (const auto &address : {gateway, name_server, Ipv4Address{10, 0, 2, 78}}) {
		require(classify(ip_frame(address, protocol_tcp,
					  tcp_segment(40000, 80, 1, 0, tcp_syn)),
				 neighbour).action == FrameAction::pass);
		require(classify(ip_frame(address, protocol_udp,
					  udp_datagram(40000, 9)),
				 neighbour).action == FrameAction::pass);
	}
}

/* What SLiRP and the provider give at the gateway and the name server needs
 * no host socket, so it is theirs even when the host holds the address. */
void whatTheProviderServesAtItsOwnAddressesIsNotRefused()
{
	const auto host = host_with({value(gateway), value(name_server),
				     value(virtual_host)});
	const auto passes = [&](const Bytes &frame) {
		return classify(frame, host).action == FrameAction::pass &&
			!needs_check(frame);
	};
	const auto refused = [&](const Bytes &frame) {
		return classify(frame, host).action != FrameAction::pass &&
			needs_check(frame);
	};
	for (const auto &address : {gateway, name_server}) {
		/* An echo, also when it arrives in fragments. */
		require(passes(ip_frame(address, protocol_icmp, Bytes(8, 0))));
		require(passes(ip_frame(address, protocol_icmp, Bytes(8, 0), 0x2000)));
		require(passes(ip_frame(address, protocol_icmp, Bytes(8, 0), 0x00b9)));
		/* DNS over both transports. */
		require(passes(ip_frame(address, protocol_udp, udp_datagram(40000, 53))));
		require(passes(ip_frame(address, protocol_tcp,
					tcp_segment(40000, 53, 1, 0, tcp_syn))));
		/* A fragment shows no port. */
		require(refused(ip_frame(address, protocol_udp, udp_datagram(40000, 53),
					 0x2000)));
		require(refused(ip_frame(address, protocol_udp, udp_datagram(40000, 53),
					 0x00b9)));
		/* Neither is DNS by any other port, nor another protocol. */
		require(refused(ip_frame(address, protocol_udp, udp_datagram(53, 40000))));
		require(refused(ip_frame(address, protocol_tcp,
					 tcp_segment(53, 40000, 1, 0, tcp_syn))));
		require(refused(ip_frame(address, 47, Bytes(16, 0))));
	}
	/* DHCP is the gateway's. */
	require(passes(ip_frame(gateway, protocol_udp, udp_datagram(68, 67))));
	require(refused(ip_frame(name_server, protocol_udp, udp_datagram(68, 67))));
	require(refused(ip_frame(gateway, protocol_tcp,
				 tcp_segment(40000, 67, 1, 0, tcp_syn))));
	require(refused(ip_frame(gateway, protocol_udp, udp_datagram(67, 68))));
	/* The other address of the virtual network serves nothing. */
	require(refused(ip_frame(virtual_host, protocol_icmp, Bytes(8, 0))));
	require(refused(ip_frame(virtual_host, protocol_udp, udp_datagram(40000, 53))));
	require(refused(ip_frame(virtual_host, protocol_tcp,
				 tcp_segment(40000, 53, 1, 0, tcp_syn))));
	require(refused(ip_frame(virtual_host, protocol_udp, udp_datagram(68, 67))));
}

void framesThatAreNotIpv4AreLeftToSlirp()
{
	Bytes arp(42, 0);
	std::copy(guest_mac.begin(), guest_mac.end(), arp.begin() + 6);
	put16(arp, 12, 0x0806);
	std::copy(lan_host.begin(), lan_host.end(), arp.begin() + 38);
	require(classify(arp, the_host()).action == FrameAction::pass);
	require(classify(Bytes(10, 0), the_host()).action == FrameAction::pass);
}

void onlyAFrameJudgedByTheTableMakesTheProviderLookItUp()
{
	/* A connect attempt, and a datagram, to anything, whatever the
	 * destination is: an address of the virtual network that serves nothing
	 * included. */
	for (const auto &destination : {lan_host, other_machine, loopback,
					virtual_host, Ipv4Address{10, 0, 2, 255},
					Ipv4Address{255, 255, 255, 255},
					Ipv4Address{224, 0, 0, 251}}) {
		require(needs_check(ip_frame(destination, protocol_tcp,
					     tcp_segment(1, 2, 3, 0, tcp_syn))));
		require(needs_check(ip_frame(destination, protocol_tcp,
					     tcp_segment(1, 2, 3, 0,
							 tcp_syn | tcp_psh))));
		require(needs_check(ip_frame(destination, protocol_udp,
					     udp_datagram(1, 2))));
	}
	/* What the provider serves at its own addresses is not: DNS at the
	 * gateway and the name server, DHCP at the gateway, an echo to either. */
	for (const auto &destination : {gateway, name_server}) {
		require(!needs_check(ip_frame(destination, protocol_tcp,
					      tcp_segment(1, 53, 3, 0, tcp_syn))));
		require(!needs_check(ip_frame(destination, protocol_udp,
					      udp_datagram(1, 53))));
		require(!needs_check(ip_frame(destination, protocol_icmp,
					      Bytes(8, 0))));
		require(needs_check(ip_frame(destination, protocol_tcp,
					     tcp_segment(1, 2, 3, 0, tcp_syn))));
		require(needs_check(ip_frame(destination, protocol_udp,
					     udp_datagram(1, 2))));
	}
	require(!needs_check(ip_frame(gateway, protocol_udp, udp_datagram(68, 67))));
	require(needs_check(ip_frame(name_server, protocol_udp, udp_datagram(68, 67))));
	/* A segment of a flow passes whatever the table holds. */
	for (const std::uint8_t flags :
	     {static_cast<std::uint8_t>(tcp_syn | tcp_ack), tcp_ack, tcp_rst,
	      static_cast<std::uint8_t>(tcp_fin | tcp_ack)})
		require(!needs_check(ip_frame(lan_host, protocol_tcp,
					      tcp_segment(1, 2, 3, 4, flags))));
	/* Everything the table decides about does: ICMP, the other protocols,
	 * and every fragment, whatever the first fragment's flags byte holds. */
	require(needs_check(ip_frame(lan_host, protocol_icmp, Bytes(8, 0))));
	require(needs_check(ip_frame(lan_host, 47, Bytes(16, 0))));
	for (const std::uint16_t fragment : {0x2000, 0x00b9, 0x20b9, 0x6000}) {
		require(needs_check(ip_frame(lan_host, protocol_tcp,
					     tcp_segment(1, 2, 3, 4, tcp_ack),
					     fragment)));
		require(needs_check(ip_frame(lan_host, protocol_tcp,
					     tcp_segment(1, 2, 3, 4, tcp_syn),
					     fragment)));
		require(needs_check(ip_frame(lan_host, protocol_tcp, Bytes(8, 0),
					     fragment)));
		require(needs_check(ip_frame(lan_host, protocol_udp,
					     udp_datagram(1, 2), fragment)));
		require(needs_check(ip_frame(lan_host, protocol_icmp, Bytes(8, 0),
					     fragment)));
		/* A fragment to the gateway shows no port, so it is judged by the
		 * table unless it is ICMP. */
		require(needs_check(ip_frame(gateway, protocol_tcp,
					     tcp_segment(1, 53, 3, 4, tcp_syn),
					     fragment)));
		require(!needs_check(ip_frame(gateway, protocol_icmp, Bytes(8, 0),
					      fragment)));
	}
	/* The Don't Fragment bit is not a fragment. */
	require(!needs_check(ip_frame(lan_host, protocol_tcp,
				      tcp_segment(1, 2, 3, 4, tcp_ack), 0x4000)));
	require(!needs_check(ip_frame(lan_host, protocol_tcp,
				      tcp_segment(1, 2, 3, 4, tcp_ack), 0)));
	Bytes arp(42, 0);
	put16(arp, 12, 0x0806);
	require(!needs_check(arp));
	require(!needs_check(Bytes(10, 0)));
	/* A header that is cut short has no destination to judge. */
	auto cut = ip_frame(lan_host, protocol_udp, udp_datagram(1, 2));
	cut.resize(14 + 12);
	require(!needs_check(cut));
	/* A segment too short to carry its flags is judged by the table. */
	require(needs_check(ip_frame(lan_host, protocol_tcp, Bytes(10, 0))));
}

void aFrameThatNeedsNoLookUpIsJudgedAlikeWhateverTheTableHolds()
{
	/* The two rules agree: when needs_host_check says the table is not
	 * consulted, classify_host_bound_frame gives the same answer with the
	 * host's addresses and without them. */
	std::vector<Bytes> frames;
	for (const auto &destination : {lan_host, other_machine, gateway, loopback}) {
		for (unsigned int flags = 0; flags < 256; ++flags)
			for (const std::uint16_t fragment : {0x4000, 0x0000})
				frames.push_back(ip_frame(
					destination, protocol_tcp,
					tcp_segment(1, 2, 3, 4,
						    static_cast<std::uint8_t>(flags), 6),
					fragment));
		for (const std::uint16_t fragment :
		     {0x2000, 0x00b9, 0x20b9, 0x6000, 0x4000, 0x0000}) {
			frames.push_back(ip_frame(destination, protocol_udp,
						  udp_datagram(1, 2), fragment));
			frames.push_back(ip_frame(destination, protocol_icmp,
						  Bytes(8, 0), fragment));
			frames.push_back(ip_frame(destination, protocol_tcp,
						  Bytes(8, 0), fragment));
		}
	}
	std::size_t unjudged = 0;
	for (const auto &frame : frames) {
		if (needs_check(frame))
			continue;
		++unjudged;
		require(classify(frame, the_host()).action ==
			classify(frame, HostAddresses{}).action);
	}
	/* A third of the frames at least are in the class this test is about. */
	require(unjudged > frames.size() / 3);
}

void aTableThatHoldsEveryAddressRefusesWhatTheTableJudges()
{
	/* The provider judges by this table when it has not been able to read the
	 * host's addresses for a while: everything that would open a flow to
	 * another machine is refused, and what it serves at its own addresses
	 * still is not. */
	const auto &all = HostAddresses::everything();
	require(!all.empty() && all.contains(other_machine) &&
		all.contains(lan_host) && all.contains(virtual_host));
	/* An ordinary table is not affected, and tables compare apart. */
	require(!the_host().contains(other_machine));
	require(all != HostAddresses{});
	require(classify(ip_frame(other_machine, protocol_tcp,
				  tcp_segment(40000, 80, 1000, 0, tcp_syn)),
			 all).action == FrameAction::reset);
	require(classify(ip_frame(other_machine, protocol_udp, udp_datagram(1, 2)),
			 all).action == FrameAction::unreachable);
	require(classify(ip_frame(other_machine, protocol_icmp, Bytes(8, 0)),
			 all).action == FrameAction::drop);
	require(classify(ip_frame(gateway, protocol_icmp, Bytes(8, 0)), all).action ==
		FrameAction::pass);
	require(classify(ip_frame(name_server, protocol_udp, udp_datagram(40000, 53)),
			 all).action == FrameAction::pass);
	require(classify(ip_frame(gateway, protocol_udp, udp_datagram(68, 67)), all).action ==
		FrameAction::pass);
}

void aTableThatHoldsEveryAddressLeavesWhatNoTableHoldsAlone()
{
	/* A guest's DHCP discover is a broadcast from 0.0.0.0, and its group
	 * traffic goes to a multicast address: no interface holds either, so
	 * they are no more the host's under this table than under any other. */
	const auto &all = HostAddresses::everything();
	for (const auto &address : {Ipv4Address{255, 255, 255, 255},
				    Ipv4Address{0, 0, 0, 0},
				    Ipv4Address{224, 0, 0, 0},
				    Ipv4Address{224, 0, 0, 251},
				    Ipv4Address{239, 255, 255, 255}}) {
		require(!all.contains(address));
		require(classify(ip_frame(address, protocol_udp, udp_datagram(68, 67)),
				 all).action == FrameAction::pass);
		require(classify(ip_frame(address, protocol_tcp,
					  tcp_segment(40000, 80, 1, 0, tcp_syn)),
				 all).action == FrameAction::pass);
	}
	/* The addresses beside them are the host's. */
	for (const auto &address : {Ipv4Address{223, 255, 255, 255},
				    Ipv4Address{240, 0, 0, 1},
				    Ipv4Address{0, 0, 0, 1},
				    Ipv4Address{255, 255, 255, 254}})
		require(all.contains(address));
}

} // namespace

int main()
{
	theTableHoldsEachAddressOnceAndAnswersByLookup();
	theSystemListsTheLoopbackAddress();
	aSynToAHostAddressIsReset();
	everyAddressTheHostHoldsIsRefused();
	onlyAConnectAttemptIsRefused();
	everyOtherSegmentGoesToSlirp();
	aResetIsNeverAnswered();
	aDatagramToAHostAddressGetsAPortUnreachable();
	whateverElseIsAddressedToTheHostIsDropped();
	everyOtherAddressGoesToSlirp();
	aHostAddressInTheVirtualNetworkIsTheHosts();
	whatTheProviderServesAtItsOwnAddressesIsNotRefused();
	framesThatAreNotIpv4AreLeftToSlirp();
	onlyAFrameJudgedByTheTableMakesTheProviderLookItUp();
	aFrameThatNeedsNoLookUpIsJudgedAlikeWhateverTheTableHolds();
	aTableThatHoldsEveryAddressRefusesWhatTheTableJudges();
	aTableThatHoldsEveryAddressLeavesWhatNoTableHoldsAlone();
	return EXIT_SUCCESS;
}
