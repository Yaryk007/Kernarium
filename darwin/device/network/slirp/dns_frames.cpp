// SPDX-License-Identifier: GPL-2.0-or-later
#include "dns_frames.hpp"

#include <algorithm>

namespace uml::network::dns {
namespace {

constexpr std::size_t ethernet_header = 14;
constexpr std::uint16_t ethertype_ipv4 = 0x0800;
constexpr std::uint8_t protocol_icmp = 1;
constexpr std::uint8_t protocol_tcp = 6;
constexpr std::uint8_t protocol_udp = 17;
constexpr std::uint8_t icmp_destination_unreachable = 3;
constexpr std::uint8_t icmp_port_unreachable = 3;
constexpr std::uint16_t dns_port = 53;
/* The BOOTP server port, which SLiRP serves at the gateway's address. */
constexpr std::uint16_t dhcp_server_port = 67;
constexpr std::uint8_t tcp_fin = 0x01;
constexpr std::uint8_t tcp_syn = 0x02;
constexpr std::uint8_t tcp_rst = 0x04;
constexpr std::uint8_t tcp_ack = 0x10;
constexpr std::uint8_t tcp_urg = 0x20;
/* SLiRP sends every IPv4 frame from its gateway's MAC (if_encap4). */
constexpr MacAddress slirp_mac{0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};

/* SLiRP opens a socket for a segment whose flags, masked so, are exactly SYN
 * (tcp_input.c); a PSH, ECE or CWR beside the SYN does not change that. */
constexpr bool is_connect_attempt(std::uint8_t flags)
{
	return (flags & (tcp_syn | tcp_fin | tcp_rst | tcp_urg | tcp_ack)) ==
		tcp_syn;
}

std::uint16_t read16(std::span<const std::uint8_t> bytes, std::size_t offset)
{
	return static_cast<std::uint16_t>((bytes[offset] << 8) |
					  bytes[offset + 1]);
}

std::uint32_t read32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
	return (static_cast<std::uint32_t>(read16(bytes, offset)) << 16) |
		read16(bytes, offset + 2);
}

void write16(std::vector<std::uint8_t> &bytes, std::size_t offset,
	     std::uint16_t value)
{
	bytes[offset] = static_cast<std::uint8_t>(value >> 8);
	bytes[offset + 1] = static_cast<std::uint8_t>(value);
}

void write32(std::vector<std::uint8_t> &bytes, std::size_t offset,
	     std::uint32_t value)
{
	write16(bytes, offset, static_cast<std::uint16_t>(value >> 16));
	write16(bytes, offset + 2, static_cast<std::uint16_t>(value));
}

std::uint32_t sum16(std::span<const std::uint8_t> bytes, std::uint32_t sum)
{
	std::size_t index = 0;
	for (; index + 1 < bytes.size(); index += 2)
		sum += read16(bytes, index);
	if (index < bytes.size())
		sum += static_cast<std::uint32_t>(bytes[index]) << 8;
	return sum;
}

std::uint16_t fold(std::uint32_t sum)
{
	while ((sum >> 16) != 0)
		sum = (sum & 0xffff) + (sum >> 16);
	return static_cast<std::uint16_t>(~sum);
}

/* Whether an IPv4 packet is a service that SLiRP or the provider gives at the
 * gateway's or the name server's own address without a host socket: ICMP
 * (SLiRP answers an echo itself and drops every other type, so none reaches a
 * host socket), DNS (the provider's, or SLiRP's guest forward for TCP), and
 * DHCP at the gateway (SLiRP's bootp). Any other flow to those addresses is no
 * service: SLiRP would connect it to the literal address
 * on the host's network, as for any address of the virtual network but the
 * gateway (which it refuses), so the table judges it like any other. A
 * fragment shows no port, so only an ICMP one is a service. `ip` starts at the
 * IPv4 header. */
bool is_ipv4_service(const Servers &servers, std::span<const std::uint8_t> ip)
{
	const Ipv4Address destination{ip[16], ip[17], ip[18], ip[19]};
	const bool gateway = destination == servers.gateway;
	if (!gateway && destination != servers.name_server)
		return false;
	const std::uint8_t protocol = ip[9];
	if (protocol == protocol_icmp)
		return true;
	const std::size_t header = (ip[0] & 0x0f) * 4U;
	if ((protocol != protocol_udp && protocol != protocol_tcp) ||
	    header < 20 || ip.size() < header + 4 ||
	    (read16(ip, 6) & 0x3fff) != 0)
		return false;
	const std::uint16_t port = read16(ip, header + 2);
	return port == dns_port ||
		(gateway && protocol == protocol_udp && port == dhcp_server_port);
}

/* The transport checksum over the IPv4 pseudo-header and `segment`. */
std::uint16_t transport_checksum(const Ipv4Address &source,
				 const Ipv4Address &destination,
				 std::uint8_t protocol,
				 std::span<const std::uint8_t> segment)
{
	std::uint32_t sum = sum16(source, 0);
	sum = sum16(destination, sum);
	sum += protocol;
	sum += static_cast<std::uint32_t>(segment.size());
	return fold(sum16(segment, sum));
}

/* Ethernet and IPv4 headers for a packet from the server to the guest;
 * returns the frame with `transport_length` zeroed bytes after them. */
std::vector<std::uint8_t> ip_frame(const Endpoints &endpoints,
				   std::uint8_t protocol,
				   std::size_t transport_length,
				   std::uint16_t identification)
{
	std::vector<std::uint8_t> frame(ethernet_header + 20 + transport_length);
	std::ranges::copy(endpoints.guest_mac, frame.begin());
	std::ranges::copy(slirp_mac, frame.begin() + 6);
	write16(frame, 12, ethertype_ipv4);
	auto *ip = frame.data() + ethernet_header;
	ip[0] = 0x45;
	write16(frame, ethernet_header + 2,
		static_cast<std::uint16_t>(20 + transport_length));
	write16(frame, ethernet_header + 4, identification);
	write16(frame, ethernet_header + 6, 0x4000); /* DF */
	ip[8] = 64;
	ip[9] = protocol;
	std::ranges::copy(endpoints.server_address, ip + 12);
	std::ranges::copy(endpoints.guest_address, ip + 16);
	write16(frame, ethernet_header + 10,
		fold(sum16({ip, 20}, 0)));
	return frame;
}

} // namespace

Classified classify_guest_frame(std::span<const std::uint8_t> frame,
				const Servers &servers)
{
	Classified result;
	if (frame.size() < ethernet_header + 20 ||
	    read16(frame, 12) != ethertype_ipv4)
		return result;
	const auto ip = frame.subspan(ethernet_header);
	if ((ip[0] >> 4) != 4)
		return result;
	const std::size_t header = (ip[0] & 0x0f) * 4U;
	const Ipv4Address destination{ip[16], ip[17], ip[18], ip[19]};
	/* Multicast stays on the virtual link. A guest's mDNS or SSDP leaving
	 * the host would be the application addressing the local network: it
	 * would raise the Local Network alert at boot, since Android
	 * advertises its adb service there, and it could only advertise
	 * addresses behind SLiRP's NAT that no peer can reach. */
	if ((destination[0] & 0xf0) == 0xe0) {
		result.action = FrameAction::drop;
		return result;
	}
	const bool to_name_server = destination == servers.name_server;
	if (!to_name_server && destination != servers.gateway)
		return result;
	/* From here a malformed packet to the name server is dropped rather
	 * than passed: SLiRP would send what it makes of it off the host. */
	const auto refuse = [&]() {
		if (to_name_server)
			result.action = FrameAction::drop;
		return result;
	};
	const std::size_t total = read16(ip, 2);
	if (header < 20 || total < header || total > ip.size())
		return refuse();
	/* As SLiRP's ip_input would drop it: a packet the name server then
	 * answers must be one SLiRP would have accepted. */
	if (fold(sum16(ip.first(header), 0)) != 0)
		return refuse();
	const std::uint16_t fragment = read16(ip, 6);
	const bool fragmented = (fragment & 0x3fff) != 0;
	const std::uint8_t protocol = ip[9];
	const auto transport = ip.subspan(header, total - header);

	auto &endpoints = result.endpoints;
	std::copy_n(frame.begin() + 6, 6, endpoints.guest_mac.begin());
	std::copy_n(frame.begin(), 6, endpoints.server_mac.begin());
	std::copy_n(ip.begin() + 12, 4, endpoints.guest_address.begin());
	endpoints.server_address = destination;

	if (protocol == protocol_udp) {
		/* A fragment's ports are unknown past the first; DNS queries
		 * are never fragmented, so no fragment reaches SLiRP's proxy. */
		if (fragmented) {
			if (to_name_server ||
			    ((fragment & 0x1fff) == 0 && transport.size() >= 4 &&
			     read16(transport, 2) == dns_port))
				result.action = FrameAction::drop;
			return result;
		}
		if (transport.size() < 8)
			return refuse();
		const std::uint16_t length = read16(transport, 4);
		if (length < 8 || length > transport.size())
			return refuse();
		if (read16(transport, 2) != dns_port)
			return refuse();
		const auto datagram = transport.first(length);
		if (read16(transport, 6) != 0 &&
		    transport_checksum(endpoints.guest_address, destination,
				       protocol_udp, datagram) != 0) {
			result.action = FrameAction::drop;
			return result;
		}
		endpoints.guest_port = read16(transport, 0);
		endpoints.server_port = dns_port;
		result.payload = datagram.subspan(8);
		result.action = FrameAction::query;
		return result;
	}
	if (protocol != protocol_tcp || fragmented || transport.size() < 20) {
		if (!to_name_server)
			return result;
		return protocol == protocol_icmp ? result : /* SLiRP answers it */
			refuse();
	}
	const std::uint16_t port = read16(transport, 2);
	/* SLiRP's guest forward serves DNS over TCP, where it is installed. The
	 * gateway's other ports are SLiRP's own; the name server has none. */
	if (port == dns_port ?
	    (to_name_server ? servers.name_server_stream :
			      servers.gateway_stream) :
	    !to_name_server)
		return result;
	const std::size_t offset = (transport[12] >> 4) * 4U;
	if (offset < 20 || offset > transport.size())
		return refuse();
	endpoints.guest_port = read16(transport, 0);
	endpoints.server_port = port;
	result.sequence = read32(transport, 4);
	result.acknowledgement = read32(transport, 8);
	result.tcp_flags = transport[13];
	result.segment_length = transport.size() - offset;
	if ((result.tcp_flags & tcp_syn) != 0)
		++result.segment_length;
	if ((result.tcp_flags & tcp_fin) != 0)
		++result.segment_length;
	result.action = FrameAction::reset;
	return result;
}

Classified classify_host_bound_frame(std::span<const std::uint8_t> frame,
				     const HostAddresses &host,
				     const Servers &servers)
{
	Classified result;
	if (host.empty() || frame.size() < ethernet_header + 20 ||
	    read16(frame, 12) != ethertype_ipv4)
		return result;
	const auto ip = frame.subspan(ethernet_header);
	if ((ip[0] >> 4) != 4)
		return result;
	const Ipv4Address destination{ip[16], ip[17], ip[18], ip[19]};
	if (!host.contains(destination) || is_ipv4_service(servers, ip))
		return result;
	/* From here the packet is for the host and SLiRP never sees it: what is
	 * not refused below is dropped. */
	result.action = FrameAction::drop;
	const std::size_t header = (ip[0] & 0x0f) * 4U;
	const std::size_t total = read16(ip, 2);
	if (header < 20 || total < header || total > ip.size() ||
	    fold(sum16(ip.first(header), 0)) != 0 ||
	    (read16(ip, 6) & 0x3fff) != 0)
		return result;
	const std::uint8_t protocol = ip[9];
	const auto transport = ip.subspan(header, total - header);

	auto &endpoints = result.endpoints;
	std::copy_n(frame.begin() + 6, 6, endpoints.guest_mac.begin());
	std::copy_n(frame.begin(), 6, endpoints.server_mac.begin());
	std::copy_n(ip.begin() + 12, 4, endpoints.guest_address.begin());
	endpoints.server_address = destination;

	if (protocol == protocol_udp) {
		if (transport.size() < 8)
			return result;
		const std::uint16_t length = read16(transport, 4);
		if (length < 8 || length > transport.size())
			return result;
		endpoints.guest_port = read16(transport, 0);
		endpoints.server_port = read16(transport, 2);
		result.quote = ip.first(header + 8);
		result.action = FrameAction::unreachable;
		return result;
	}
	if (protocol != protocol_tcp || transport.size() < 20)
		return result;
	const std::size_t offset = (transport[12] >> 4) * 4U;
	if (offset < 20 || offset > transport.size())
		return result;
	/* A segment of a flow, which SLiRP serves or resets itself, is not
	 * refused here. */
	if (!is_connect_attempt(transport[13]))
		return {};
	endpoints.guest_port = read16(transport, 0);
	endpoints.server_port = read16(transport, 2);
	result.sequence = read32(transport, 4);
	result.acknowledgement = read32(transport, 8);
	result.tcp_flags = transport[13];
	result.segment_length = transport.size() - offset + 1;
	result.action = FrameAction::reset;
	return result;
}

bool needs_host_check(std::span<const std::uint8_t> frame,
		      const Servers &servers)
{
	if (frame.size() < ethernet_header + 20 ||
	    read16(frame, 12) != ethertype_ipv4)
		return false;
	const auto ip = frame.subspan(ethernet_header);
	if ((ip[0] >> 4) != 4)
		return false;
	const std::size_t header = (ip[0] & 0x0f) * 4U;
	if (header < 20 || header > ip.size() || is_ipv4_service(servers, ip))
		return false;
	/* The one frame the table never changes the fate of is an unfragmented
	 * TCP segment that is not a connect attempt, which is passed whatever
	 * the table holds. A fragment is judged by the table, whatever it
	 * carries (the flags byte of a first fragment is not the segment's),
	 * and so are UDP, ICMP and the other protocols. */
	if (ip[9] == protocol_tcp && (read16(ip, 6) & 0x3fff) == 0 &&
	    ip.size() >= header + 14)
		return is_connect_attempt(ip[header + 13]);
	return true;
}

std::vector<std::uint8_t> udp_reply_frame(const Endpoints &endpoints,
					  std::span<const std::uint8_t> payload,
					  std::uint16_t identification)
{
	auto frame = ip_frame(endpoints, protocol_udp, 8 + payload.size(),
			      identification);
	const std::size_t udp = ethernet_header + 20;
	write16(frame, udp, endpoints.server_port);
	write16(frame, udp + 2, endpoints.guest_port);
	write16(frame, udp + 4, static_cast<std::uint16_t>(8 + payload.size()));
	std::ranges::copy(payload, frame.begin() + udp + 8);
	std::uint16_t checksum = transport_checksum(
		endpoints.server_address, endpoints.guest_address, protocol_udp,
		{frame.data() + udp, 8 + payload.size()});
	if (checksum == 0)
		checksum = 0xffff;
	write16(frame, udp + 6, checksum);
	return frame;
}

std::vector<std::uint8_t> tcp_reset_frame(const Classified &segment,
					  std::uint16_t identification)
{
	if ((segment.tcp_flags & tcp_rst) != 0)
		return {};
	auto frame = ip_frame(segment.endpoints, protocol_tcp, 20,
			      identification);
	const std::size_t tcp = ethernet_header + 20;
	write16(frame, tcp, segment.endpoints.server_port);
	write16(frame, tcp + 2, segment.endpoints.guest_port);
	frame[tcp + 12] = 5 << 4;
	if ((segment.tcp_flags & tcp_ack) != 0) {
		write32(frame, tcp + 4, segment.acknowledgement);
		frame[tcp + 13] = tcp_rst;
	} else {
		write32(frame, tcp + 8,
			segment.sequence +
				static_cast<std::uint32_t>(segment.segment_length));
		frame[tcp + 13] = tcp_rst | tcp_ack;
	}
	write16(frame, tcp + 16,
		transport_checksum(segment.endpoints.server_address,
				   segment.endpoints.guest_address,
				   protocol_tcp, {frame.data() + tcp, 20}));
	return frame;
}

std::vector<std::uint8_t> icmp_unreachable_frame(const Classified &datagram,
						 std::uint16_t identification)
{
	const std::size_t length = 8 + datagram.quote.size();
	auto frame = ip_frame(datagram.endpoints, protocol_icmp, length,
			      identification);
	const std::size_t icmp = ethernet_header + 20;
	frame[icmp] = icmp_destination_unreachable;
	frame[icmp + 1] = icmp_port_unreachable;
	std::ranges::copy(datagram.quote, frame.begin() + icmp + 8);
	write16(frame, icmp + 2,
		fold(sum16({frame.data() + icmp, length}, 0)));
	return frame;
}

} // namespace uml::network::dns
