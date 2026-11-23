// SPDX-License-Identifier: GPL-2.0-or-later
#include "host_addresses.hpp"

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <iterator>
#include <memory>
#include <utility>

namespace uml::network {
namespace {

constexpr std::uint32_t ipv4_value(const HostIpv4 &address) noexcept
{
	return (static_cast<std::uint32_t>(address[0]) << 24) |
		(static_cast<std::uint32_t>(address[1]) << 16) |
		(static_cast<std::uint32_t>(address[2]) << 8) | address[3];
}

} // namespace

void HostAddresses::assign_ipv4(std::vector<std::uint32_t> addresses)
{
	assign_ipv4(std::move(addresses), {});
}

void HostAddresses::assign_ipv4(std::vector<std::uint32_t> assigned,
				std::vector<std::uint32_t> broadcast)
{
	/* The unspecified address and the limited broadcast address are no
	 * interface's: the guest's DHCP goes to the second, and SLiRP answers
	 * for it. */
	const auto tidy = [](std::vector<std::uint32_t> &addresses) {
		std::erase_if(addresses, [](std::uint32_t address) {
			return address == 0 || address == 0xffffffffU;
		});
		std::ranges::sort(addresses);
		addresses.erase(std::ranges::unique(addresses).begin(),
				addresses.end());
	};
	tidy(assigned);
	tidy(broadcast);
	std::vector<std::uint32_t> all;
	all.reserve(assigned.size() + broadcast.size());
	std::ranges::merge(assigned, broadcast, std::back_inserter(all));
	all.erase(std::ranges::unique(all).begin(), all.end());
	ipv4_ = std::move(all);
	assigned_ = std::move(assigned);
}

const HostAddresses &HostAddresses::everything() noexcept
{
	static const HostAddresses table = [] {
		HostAddresses result;
		result.all_ = true;
		return result;
	}();
	return table;
}

bool HostAddresses::contains(const HostIpv4 &address) const noexcept
{
	if (all_) {
		const auto value = ipv4_value(address);
		return value != 0 && value != 0xffffffffU &&
			(value & 0xf0000000U) != 0xe0000000U;
	}
	return std::ranges::binary_search(ipv4_, ipv4_value(address));
}

bool HostAddresses::assigned(const HostIpv4 &address) const noexcept
{
	if (all_)
		return contains(address);
	return std::ranges::binary_search(assigned_, ipv4_value(address));
}

bool HostAddresses::lost_since(const HostAddresses &earlier) const noexcept
{
	return !std::ranges::includes(assigned_, earlier.assigned_);
}

HostAddresses host_addresses_from(const ifaddrs *list)
{
	std::vector<std::uint32_t> assigned;
	std::vector<std::uint32_t> broadcast;
	const auto add = [](std::vector<std::uint32_t> &into,
			    const sockaddr *address) {
		if (address == nullptr || address->sa_family != AF_INET)
			return;
		into.push_back(ntohl(
			reinterpret_cast<const sockaddr_in *>(address)->sin_addr
				.s_addr));
	};
	for (const ifaddrs *entry = list; entry != nullptr;
	     entry = entry->ifa_next) {
		/* The family switch: an IPv6 address joins its own table here. */
		add(assigned, entry->ifa_addr);
		/* ifa_dstaddr is the broadcast address on a broadcast
		 * interface and the far end of a point-to-point link, which
		 * is not the host's. */
		if ((entry->ifa_flags & IFF_BROADCAST) != 0)
			add(broadcast, entry->ifa_dstaddr);
	}
	HostAddresses result;
	result.assign_ipv4(std::move(assigned), std::move(broadcast));
	return result;
}

std::optional<HostAddresses> read_host_addresses()
{
	ifaddrs *list = nullptr;
	if (::getifaddrs(&list) != 0)
		return std::nullopt;
	const std::unique_ptr<ifaddrs, decltype(&::freeifaddrs)> owned{
		list, &::freeifaddrs};
	return host_addresses_from(list);
}

} // namespace uml::network
