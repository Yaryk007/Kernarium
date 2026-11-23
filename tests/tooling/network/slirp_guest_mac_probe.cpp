/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The guest's hardware address is the caller's: every installation of the
 * Android guest has an eth0 address of its own (SlirpProvider::set_mac), and
 * the user network must answer a guest that uses it as it answers one with the
 * default. The probe sets synthetic addresses, opens the provider and drives it
 * the way a guest does: the address open() reports, ARP, a DHCP exchange for
 * the lease and for a second interface (the guest's Wi-Fi address, which is a
 * different one), and a DNS query, each answered to the guest's own address.
 *
 * It never states a real installation's address.
 */
#include "provider.hpp"
#include "slirp_tcp_harness.hpp"
#include "uml_slirp_net.h"

#include <dns_sd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace slirp_harness;
using Address = std::array<std::uint8_t, 4>;
using Mac = std::array<std::uint8_t, 6>;
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;

constexpr Address gateway{10, 0, 2, 2};
constexpr Address name_server{10, 0, 2, 3};
constexpr Mac first_mac{0x02, 0x7b, 0x31, 0xc4, 0x90, 0x6d};
constexpr Mac second_mac{0x06, 0x11, 0x22, 0x33, 0x44, 0x55};

int failures;

void fail(const char *label, const std::string &message)
{
	std::fprintf(stderr, "%s: %s\n", label, message.c_str());
	failures++;
}

Bytes udp_frame(const Mac &source_mac, const Address &source,
		const Address &destination, std::uint16_t source_port,
		std::uint16_t destination_port, const Bytes &payload)
{
	const std::size_t udp_length = 8 + payload.size();
	Bytes frame(14 + 20 + udp_length, 0);
	for (std::size_t index = 0; index < 6; ++index) {
		frame[index] = 0xff;
		frame[6 + index] = source_mac[index];
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

/* A DHCP client message of `type` from the interface with address `mac`; a
 * REQUEST asks for `wanted`. */
Bytes dhcp_message(const Mac &mac, std::uint8_t type,
		   const std::optional<Address> &wanted)
{
	Bytes bootp(240, 0);
	bootp[0] = 1;
	bootp[1] = 1;
	bootp[2] = 6;
	put32(bootp, 4, 0x1badcafe);
	put16(bootp, 10, 0x8000);
	std::copy(mac.begin(), mac.end(), bootp.begin() + 28);
	put32(bootp, 236, 0x63825363U);
	bootp.insert(bootp.end(), {53, 1, type});
	if (wanted) {
		bootp.insert(bootp.end(), {50, 4, (*wanted)[0], (*wanted)[1],
					   (*wanted)[2], (*wanted)[3]});
		bootp.insert(bootp.end(), {54, 4, 10, 0, 2, 2});
	}
	bootp.push_back(255);
	return bootp;
}

/* The message type of a reply from SLiRP's DHCP server, or 0. */
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

Bytes arp_request(const Mac &mac)
{
	Bytes frame(42, 0);
	for (std::size_t index = 0; index < 6; ++index) {
		frame[index] = 0xff;
		frame[6 + index] = mac[index];
		frame[22 + index] = mac[index];
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

/* The yiaddr of a BOOTP reply and the client hardware address it echoes. */
Address offered_address(const Bytes &frame)
{
	return {frame[42 + 16], frame[42 + 17], frame[42 + 18], frame[42 + 19]};
}

Mac echoed_mac(const Bytes &frame)
{
	Mac mac{};
	std::copy_n(frame.begin() + 42 + 28, 6, mac.begin());
	return mac;
}

/* One DHCP exchange of the interface `mac`: a DISCOVER answered by an OFFER and
 * a REQUEST for the offered address answered by an ACK, both for that
 * interface. Returns the address leased. */
std::optional<Address> lease(Guest &guest, const Mac &mac, const char *label)
{
	const Address any{0, 0, 0, 0};
	const Address broadcast{255, 255, 255, 255};
	if (!guest.send(udp_frame(mac, any, broadcast, 68, 67,
				  dhcp_message(mac, 1, std::nullopt)))) {
		fail(label, "the provider refused the DISCOVER");
		return std::nullopt;
	}
	const auto offer = guest.receive([](const Bytes &frame) {
		return server_dhcp_type(frame) == 2;
	});
	if (!offer) {
		fail(label, "no OFFER");
		return std::nullopt;
	}
	if (echoed_mac(*offer) != mac)
		fail(label, "the OFFER is for another hardware address");
	const auto address = offered_address(*offer);
	if (!guest.send(udp_frame(mac, any, broadcast, 68, 67,
				  dhcp_message(mac, 3, address)))) {
		fail(label, "the provider refused the REQUEST");
		return std::nullopt;
	}
	const auto ack = guest.receive([](const Bytes &frame) {
		return server_dhcp_type(frame) == 5;
	});
	if (!ack) {
		fail(label, "no ACK");
		return std::nullopt;
	}
	if (echoed_mac(*ack) != mac || offered_address(*ack) != address)
		fail(label, "the ACK is not for the lease that was offered");
	return address;
}

void check_unset_address()
{
	uml::network::SlirpProvider provider;
	provider.set_dns_interface_for_testing(kDNSServiceInterfaceIndexLocalOnly);
	std::array<unsigned char, 6> mac{};
	unsigned int mtu = 0;
	if (provider.open(mac, mtu) != 0) {
		fail("default", "the provider did not open");
		return;
	}
	if (mac != uml::network::default_guest_mac || mtu != 1500)
		fail("default", "a provider no caller set an address on does not "
				"report the default address");
	provider.stop();
}

void check_refused_addresses()
{
	uml::network::SlirpProvider provider;
	const std::array<unsigned char, 6> multicast{0x03, 1, 2, 3, 4, 5};
	const std::array<unsigned char, 6> zero{};
	const std::array<unsigned char, 6> broadcast{0xff, 0xff, 0xff, 0xff, 0xff,
						     0xff};
	/* The addresses the router and the name server answer from. */
	const std::array<unsigned char, 6> gateway{0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};
	const std::array<unsigned char, 6> name_server{0x52, 0x55, 0x0a, 0x00, 0x02, 0x03};
	const std::array<unsigned char, 6> other_server{0x52, 0x55, 0x0a, 0x00, 0x02, 0xff};
	for (const auto &mac : {multicast, zero, broadcast, gateway, name_server,
				other_server})
		if (provider.set_mac(mac) != -EINVAL ||
		    uml_slirp_net_set_mac(&provider, mac.data()) != -EINVAL)
			fail("refused", "an address that cannot be the guest's was taken");
	/* Only that prefix: its neighbours are ordinary addresses. */
	const std::array<unsigned char, 6> neighbour{0x52, 0x55, 0x0a, 0x00, 0x03, 0x02};
	if (provider.set_mac(neighbour) != 0)
		fail("refused", "an address next to the router's was refused");
	if (provider.set_mac(uml::network::default_guest_mac) != 0)
		fail("refused", "the default address was refused");
	if (uml_slirp_net_set_mac(nullptr, first_mac.data()) != -EINVAL ||
	    uml_slirp_net_set_mac(&provider, nullptr) != -EINVAL)
		fail("refused", "a null argument was accepted");
	/* The refusals left the default. */
	std::array<unsigned char, 6> reported{};
	unsigned int mtu = 0;
	provider.set_dns_interface_for_testing(kDNSServiceInterfaceIndexLocalOnly);
	if (provider.open(reported, mtu) != 0 ||
	    reported != uml::network::default_guest_mac)
		fail("refused", "a refused address changed the default");
	provider.stop();
}

void check_set_address()
{
	uml::network::SlirpProvider provider;
	Guest guest{provider};
	provider.set_dns_interface_for_testing(kDNSServiceInterfaceIndexLocalOnly);
	if (uml_slirp_net_set_mac(&provider, first_mac.data()) != 0) {
		fail("set", "the C entry refused an address");
		return;
	}
	std::array<unsigned char, 6> mac{};
	unsigned int mtu = 0;
	if (provider.open(mac, mtu) != 0) {
		fail("open", "the provider did not open");
		return;
	}
	if (mac != first_mac)
		fail("open", "open() did not report the address set");
	/* The address belongs to the provider, not to one open: the next open, the
	 * guest's own after a link that came up again, reports it too. */
	provider.stop();
	if (provider.open(mac, mtu) != 0 || mac != first_mac)
		fail("reopen", "a link that came up again lost the address");
	/* An address set after the first open applies to the next. */
	if (provider.set_mac(second_mac) != 0) {
		fail("reset", "an address was refused");
		return;
	}
	if (provider.open(mac, mtu) != 0 || mac != second_mac)
		fail("reset", "the next open did not report the new address");
	if (provider.set_mac(first_mac) != 0 || provider.open(mac, mtu) != 0 ||
	    mac != first_mac)
		fail("reset", "the address did not change back");

	/* ARP: the gateway answers the guest by its own address. */
	(void)guest.send(arp_request(first_mac));
	const auto arp = guest.receive([](const Bytes &frame) {
		return frame.size() >= 42 && get16(frame, 12) == 0x0806 &&
			get16(frame, 20) == 2;
	});
	if (!arp) {
		fail("ARP", "no reply");
	} else {
		Mac target{};
		std::copy_n(arp->begin() + 32, 6, target.begin());
		Mac destination{};
		std::copy_n(arp->begin(), 6, destination.begin());
		if (target != first_mac || destination != first_mac)
			fail("ARP", "the reply is not for the guest's address");
	}

	/* DHCP: the interface leases the address it asks for, and a second
	 * interface with another address, as the guest's Wi-Fi has, is given the
	 * next one. */
	const auto first = lease(guest, first_mac, "DHCP of the first interface");
	if (first && *first != Address{10, 0, 2, 15})
		fail("DHCP", "the first interface was not given the lease address");
	const auto second = lease(guest, second_mac, "DHCP of the second interface");
	if (second && first && *second == *first)
		fail("DHCP", "the second interface was given the first's address");

	/* DNS: the name server answers the guest's own address. */
	const char *label = "DNS";
	(void)guest.send(udp_frame(first_mac, guest_ip, name_server, 43000, 53,
				   query_message(43000, "localhost")));
	const auto answer = guest.receive([](const Bytes &frame) {
		return frame.size() > 46 && frame[23] == IPPROTO_UDP &&
			get16(frame, 34) == 53 && get16(frame, 36) == 43000;
	});
	if (!answer) {
		fail(label, "no answer");
	} else {
		Mac destination{};
		std::copy_n(answer->begin(), 6, destination.begin());
		if (destination != first_mac)
			fail(label, "the answer is not for the guest's address");
		if (((*answer)[45] & 0x0f) != 0)
			fail(label, "the answer is not NOERROR");
	}
	provider.stop();
}

} // namespace

int main()
{
	check_unset_address();
	check_refused_addresses();
	check_set_address();
	if (failures != 0) {
		std::fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	std::puts("guest hardware address: ok");
	return 0;
}
