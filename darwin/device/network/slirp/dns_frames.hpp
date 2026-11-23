// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/*
 * The guest frames the provider answers itself instead of handing them to
 * SLiRP: DNS datagrams to the virtual name server or the gateway, the name
 * server's other traffic, which SLiRP would otherwise send to that literal
 * address on the host's network once its own DNS proxy is off, and the
 * traffic addressed to the host itself, which it refuses.
 */

#include "host_addresses.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace uml::network::dns {

using MacAddress = std::array<std::uint8_t, 6>;
using Ipv4Address = std::array<std::uint8_t, 4>;

/* The two ends of one guest datagram or segment, as the reply reverses them. */
struct Endpoints final {
	MacAddress guest_mac{};
	MacAddress server_mac{};
	Ipv4Address guest_address{};
	Ipv4Address server_address{};
	std::uint16_t guest_port{};
	std::uint16_t server_port{};
};

enum class FrameAction {
	pass,      /* SLiRP handles it */
	query,     /* a DNS datagram: `payload` goes to the resolver */
	reset,     /* a TCP segment to a port that nothing serves */
	unreachable, /* a UDP datagram to a port that nothing serves */
	drop,      /* anything else addressed to the name server, and multicast */
};

struct Classified final {
	FrameAction action{FrameAction::pass};
	Endpoints endpoints;
	std::span<const std::uint8_t> payload;
	/* For `reset`: the segment's sequence, acknowledgement and flags. */
	std::uint32_t sequence{};
	std::uint32_t acknowledgement{};
	std::uint8_t tcp_flags{};
	std::size_t segment_length{};
	/* For `unreachable`: the datagram's IP header and first eight bytes,
	 * which the ICMP error quotes. */
	std::span<const std::uint8_t> quote;
};

/*
 * The addresses the provider and SLiRP serve themselves. `name_server` is
 * SLiRP's virtual name server, which serves only DNS: UDP here, TCP through
 * SLiRP's guest forward. `gateway` answers DNS too, for guests configured with
 * the gateway as their server, and passes the rest to SLiRP. A `_stream` flag
 * says that the guest forward for DNS over TCP is installed at that address;
 * without it a TCP connection to port 53 is reset, because SLiRP, with its own
 * DNS proxy off, would connect to the literal address on the host's network.
 */
struct Servers final {
	Ipv4Address name_server{};
	Ipv4Address gateway{};
	bool name_server_stream{};
	bool gateway_stream{};
};

[[nodiscard]] Classified classify_guest_frame(
	std::span<const std::uint8_t> frame, const Servers &servers);

/*
 * A guest flow addressed to one of the host's own addresses is refused, for
 * SLiRP would connect it to a service of the machine the application runs
 * on: a TCP connect attempt is answered with a reset and a UDP datagram with
 * an ICMP port unreachable, whether or not anything listens, so the guest
 * learns nothing about the host's services. A connect attempt is a segment
 * whose SYN, FIN, RST, URG and ACK flags are exactly SYN, SLiRP's own test
 * for opening a socket. Every other TCP segment is `pass`, for SLiRP serves
 * the flow it belongs to or resets it itself, and the host's own client
 * reaches a forward through one of the host's addresses by a flow the host
 * opened. ICMP, fragments and a malformed packet addressed to the host are
 * dropped. A frame for any other address is `pass`. The virtual network is
 * no exception: an address of it that the table holds is the host's, and
 * SLiRP would connect a flow to it as it does to any other literal address.
 * The one exception is what SLiRP and the provider give at their own
 * addresses without a host socket: ICMP to the gateway or the name server
 * (SLiRP answers an echo itself and drops every other type), DNS (port 53) at
 * either, and DHCP (UDP port 67) at the gateway.
 * They are `pass` even when the host holds the address; every other flow to
 * those addresses is refused like one to any other address the host holds.
 * Run it after classify_guest_frame has passed the frame on.
 */
[[nodiscard]] Classified classify_host_bound_frame(
	std::span<const std::uint8_t> frame, const HostAddresses &host,
	const Servers &servers);

/*
 * Whether classify_host_bound_frame needs the host's current addresses for
 * `frame`, that is, whether the table can change its fate. It cannot for an
 * unfragmented TCP segment that is not a connect attempt, nor for a service
 * of `servers` (see above). It can for every other IPv4 frame: a connect
 * attempt, every fragment, every UDP datagram, ICMP and the other protocols.
 * The provider reads the addresses again for such a frame only, so it asks
 * the system nothing while the guest is quiet.
 */
[[nodiscard]] bool needs_host_check(std::span<const std::uint8_t> frame,
				    const Servers &servers);

/* One Ethernet frame carrying `payload` from the server back to the guest.
 * The payload must fit one 1500-byte IP packet. */
[[nodiscard]] std::vector<std::uint8_t> udp_reply_frame(
	const Endpoints &endpoints, std::span<const std::uint8_t> payload,
	std::uint16_t identification);

/* The RFC 9293 reset for a classified `reset` segment, or nothing for a
 * segment that carries RST itself. */
[[nodiscard]] std::vector<std::uint8_t> tcp_reset_frame(
	const Classified &segment, std::uint16_t identification);

/* The ICMP port unreachable (RFC 792) for a classified `unreachable`
 * datagram, from the address the datagram was sent to. */
[[nodiscard]] std::vector<std::uint8_t> icmp_unreachable_frame(
	const Classified &datagram, std::uint16_t identification);

} // namespace uml::network::dns
