// SPDX-License-Identifier: GPL-2.0-or-later
#include "network_counters.hpp"

#include <dns_sd.h>

#include <algorithm>
#include <optional>

namespace uml::network {
namespace {

constexpr std::size_t ethernet_header = 14;
constexpr std::uint16_t ethertype_ipv4 = 0x0800;
constexpr std::uint8_t protocol_icmp = 1;
constexpr std::uint8_t protocol_tcp = 6;
constexpr std::uint8_t protocol_udp = 17;
constexpr std::uint8_t tcp_syn = 0x02;
constexpr std::uint8_t tcp_rst = 0x04;
constexpr std::uint8_t tcp_ack = 0x10;
constexpr std::uint8_t icmp_destination_unreachable = 3;
constexpr std::uint8_t icmp_host_unreachable = 1;
constexpr std::uint16_t bootp_server_port = 67;
constexpr std::uint16_t bootp_client_port = 68;
constexpr std::uint16_t dns_port = 53;
/* The BOOTP message is 236 bytes, then the magic cookie, then the options. */
constexpr std::size_t bootp_fixed = 236;
constexpr std::uint8_t dhcp_message_type_option = 53;
constexpr std::uint8_t dhcp_discover = 1;
constexpr std::uint8_t dhcp_request = 3;
constexpr std::uint8_t dhcp_ack = 5;

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

/* The IPv4 packet in an Ethernet frame, cut to its own length. */
struct Packet final {
	std::uint8_t protocol{};
	std::uint32_t source{};
	std::uint32_t destination{};
	std::span<const std::uint8_t> payload;
};

std::optional<Packet> ipv4_packet(std::span<const std::uint8_t> frame)
{
	if (frame.size() < ethernet_header + 20 ||
	    read16(frame, 12) != ethertype_ipv4)
		return std::nullopt;
	const auto ip = frame.subspan(ethernet_header);
	if ((ip[0] >> 4) != 4)
		return std::nullopt;
	const std::size_t header = (ip[0] & 0x0fU) * 4U;
	const std::size_t total = read16(ip, 2);
	/* A later fragment carries no transport header. */
	if (header < 20 || total < header || total > ip.size() ||
	    (read16(ip, 6) & 0x1fffU) != 0)
		return std::nullopt;
	return Packet{ip[9], read32(ip, 12), read32(ip, 16),
		      ip.subspan(header, total - header)};
}

/* The DHCP message type of a BOOTP payload, or 0. */
std::uint8_t dhcp_type(std::span<const std::uint8_t> bootp)
{
	if (bootp.size() < bootp_fixed + 4 ||
	    read32(bootp, bootp_fixed) != 0x63825363U)
		return 0;
	for (std::size_t index = bootp_fixed + 4; index < bootp.size();) {
		const std::uint8_t code = bootp[index++];
		if (code == 255)
			break;
		if (code == 0)
			continue;
		if (index >= bootp.size())
			break;
		const std::size_t length = bootp[index++];
		if (index + length > bootp.size())
			break;
		if (code == dhcp_message_type_option && length == 1)
			return bootp[index];
		index += length;
	}
	return 0;
}

} // namespace

DnsOutcome classify_dns_error(std::int32_t error) noexcept
{
	switch (error) {
	case kDNSServiceErr_NoError:
	case kDNSServiceErr_NoSuchName:
	case kDNSServiceErr_NoSuchRecord:
		return DnsOutcome::answered;
	case kDNSServiceErr_PolicyDenied:
	case kDNSServiceErr_NotPermitted:
		return DnsOutcome::policy_denied;
	case kDNSServiceErr_Timeout:
		return DnsOutcome::timed_out;
	default:
		return DnsOutcome::failed;
	}
}

void NetworkCounters::dns(DnsOutcome outcome) noexcept
{
	switch (outcome) {
	case DnsOutcome::answered:
		add(dns_answered_);
		break;
	case DnsOutcome::policy_denied:
		add(dns_policy_denied_);
		break;
	case DnsOutcome::timed_out:
		add(dns_timed_out_);
		break;
	case DnsOutcome::failed:
		add(dns_failed_);
		break;
	}
}

NetworkCounts NetworkCounters::read() const noexcept
{
	constexpr auto order = std::memory_order_relaxed;
	NetworkCounts counts;
	counts.dns_answered = dns_answered_.load(order);
	counts.dns_policy_denied = dns_policy_denied_.load(order);
	counts.dns_timed_out = dns_timed_out_.load(order);
	counts.dns_failed = dns_failed_.load(order);
	counts.dhcp_discovers = dhcp_discovers_.load(order);
	counts.dhcp_requests = dhcp_requests_.load(order);
	counts.dhcp_acks = dhcp_acks_.load(order);
	counts.tcp_attempts = tcp_attempts_.load(order);
	counts.tcp_established = tcp_established_.load(order);
	counts.tcp_unreachable = tcp_unreachable_.load(order);
	counts.tcp_reset = tcp_reset_.load(order);
	return counts;
}

void TrafficObserver::reset() noexcept
{
	pending_ = {};
}

TrafficObserver::Pending *TrafficObserver::find(
	std::uint32_t destination, std::uint16_t destination_port,
	std::uint16_t source_port) noexcept
{
	for (auto &entry : pending_)
		if (entry.used && entry.destination == destination &&
		    entry.destination_port == destination_port &&
		    entry.source_port == source_port)
			return &entry;
	return nullptr;
}

void TrafficObserver::connection_attempt(
	std::uint32_t destination, std::uint16_t destination_port,
	std::uint16_t source_port, Clock::time_point now) noexcept
{
	if (auto *entry = find(destination, destination_port, source_port);
	    entry != nullptr) {
		if (now - entry->first_seen < retransmission_window)
			return;
		entry->first_seen = now;
		counters_.tcp_attempt();
		return;
	}
	Pending *slot = nullptr;
	for (auto &entry : pending_)
		if (!entry.used) {
			slot = &entry;
			break;
		}
	/* A full table gives up its oldest attempt: one no answer reached. */
	if (slot == nullptr)
		slot = &*std::ranges::min_element(
			pending_, [](const Pending &left, const Pending &right) {
				return left.first_seen < right.first_seen;
			});
	*slot = {destination, destination_port, source_port, now, true};
	counters_.tcp_attempt();
}

void TrafficObserver::from_guest(std::span<const std::uint8_t> frame,
				 Clock::time_point now) noexcept
{
	observe_guest(frame, [now] { return now; });
}

void TrafficObserver::from_guest(std::span<const std::uint8_t> frame) noexcept
{
	observe_guest(frame, [] { return Clock::now(); });
}

template <typename Now>
void TrafficObserver::observe_guest(std::span<const std::uint8_t> frame,
				    Now now) noexcept
{
	const auto packet = ipv4_packet(frame);
	if (!packet)
		return;
	const auto &payload = packet->payload;
	if (packet->protocol == protocol_udp && payload.size() >= 8 + bootp_fixed) {
		if (read16(payload, 2) != bootp_server_port)
			return;
		switch (dhcp_type(payload.subspan(8))) {
		case dhcp_discover:
			counters_.dhcp_discover();
			break;
		case dhcp_request:
			counters_.dhcp_request();
			break;
		default:
			break;
		}
		return;
	}
	if (packet->protocol != protocol_tcp || payload.size() < 20)
		return;
	const std::uint8_t flags = payload[13];
	if ((flags & (tcp_syn | tcp_ack | tcp_rst)) != tcp_syn)
		return;
	const std::uint16_t port = read16(payload, 2);
	/* The guest forward answers DNS over TCP itself. */
	if (port == dns_port && serves_dns_stream(packet->destination))
		return;
	connection_attempt(packet->destination, port, read16(payload, 0),
			   now());
}

void TrafficObserver::from_network(
	std::span<const std::uint8_t> frame) noexcept
{
	const auto packet = ipv4_packet(frame);
	if (!packet)
		return;
	const auto &payload = packet->payload;
	switch (packet->protocol) {
	case protocol_udp:
		if (payload.size() >= 8 + bootp_fixed &&
		    read16(payload, 0) == bootp_server_port &&
		    read16(payload, 2) == bootp_client_port &&
		    dhcp_type(payload.subspan(8)) == dhcp_ack)
			counters_.dhcp_ack();
		return;
	case protocol_tcp: {
		if (payload.size() < 20)
			return;
		/* Only a reset or a SYN-ACK can answer a SYN; data segments, the
		 * bulk of the traffic, stop here. */
		const std::uint8_t flags = payload[13];
		const bool reset = (flags & tcp_rst) != 0;
		if (!reset && (flags & (tcp_syn | tcp_ack)) != (tcp_syn | tcp_ack))
			return;
		auto *entry = find(packet->source, read16(payload, 0),
				   read16(payload, 2));
		if (entry == nullptr)
			return;
		if (reset)
			counters_.tcp_reset();
		else
			counters_.tcp_established();
		*entry = {};
		return;
	}
	case protocol_icmp: {
		/* The error quotes the packet that caused it: its IP header and
		 * the first eight bytes of its TCP header, the two ports. */
		/* SLiRP sends net (0) and host (1) unreachable for a connect() that
		 * failed at once; any other code is not that. */
		if (payload.size() < 8 + 20 + 8 ||
		    payload[0] != icmp_destination_unreachable ||
		    payload[1] > icmp_host_unreachable)
			return;
		const auto quoted = payload.subspan(8);
		const std::size_t header = (quoted[0] & 0x0fU) * 4U;
		if ((quoted[0] >> 4) != 4 || header < 20 ||
		    quoted.size() < header + 8 || quoted[9] != protocol_tcp)
			return;
		auto *entry = find(read32(quoted, 16), read16(quoted, header + 2),
				   read16(quoted, header));
		if (entry == nullptr)
			return;
		counters_.tcp_unreachable();
		*entry = {};
		return;
	}
	default:
		return;
	}
}

} // namespace uml::network
