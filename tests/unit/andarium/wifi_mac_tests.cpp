// SPDX-License-Identifier: GPL-2.0-or-later
//
// The guest's Wi-Fi address as a permutation of eth0's
// (android/lineage-24/overlay/tools/kernarium_wifi_mac.h): the golden vectors
// that the guest runner's Python port reproduces, the permutation and its
// inverse, the properties of every result, the addresses it never gives, and
// what it does with inputs the host never generates.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "kernarium_wifi_mac.h"
#include "require.hpp"

using namespace kernarium_net;
using uml::testing::require;

namespace {

Mac parse(const std::string &text)
{
	require(text.size() == 17, "an address is 17 characters: " + text);
	Mac mac{};
	for (std::size_t index = 0; index < mac.size(); ++index)
		mac[index] = static_cast<unsigned char>(
			std::stoul(text.substr(index * 3, 2), nullptr, 16));
	return mac;
}

std::string text(const Mac &mac)
{
	char buffer[18];
	std::snprintf(buffer, sizeof(buffer), "%02x:%02x:%02x:%02x:%02x:%02x",
		mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	return buffer;
}

bool local_unicast(const Mac &mac)
{
	return (mac[0] & 0x03) == 0x02;
}

/* An address the host could have generated and the guest could have been given. */
bool host_could_give(const Mac &mac)
{
	return local_unicast(mac) && !WifiMacReserved(mac);
}

/* A cheap deterministic stream. */
struct Stream {
	std::uint64_t state = 0x9e3779b97f4a7c15ULL;
	std::uint64_t next()
	{
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		return state;
	}
	Mac host_address()
	{
		return WifiMacFromBits(next() & (kWifiMacValues - 1));
	}
};

/* What every result is: a locally administered unicast address that is neither eth0's
 * own, 02:00:00:00:00:00 nor an address of the host's virtual router. */
void require_valid(const Mac &wifi, const Mac &ethernet)
{
	const std::string where = " for " + text(ethernet);
	require(local_unicast(wifi), "not locally administered unicast" + where);
	require(PackMac(wifi) != PackMac(ethernet), "eth0's own address" + where);
	require(!WifiMacReserved(wifi), "a reserved address" + where);
}

void test_golden_vectors(const char *path)
{
	std::ifstream file(path);
	require(file.is_open(), std::string{"no golden vectors: "} + path);
	std::string line;
	int checked = 0;
	while (std::getline(file, line)) {
		if (line.empty() || line[0] == '#')
			continue;
		std::istringstream fields(line);
		std::string ethernet, wifi;
		require(static_cast<bool>(fields >> ethernet >> wifi), "a vector line: " + line);
		const Mac derived = DeriveWifiMac(parse(ethernet));
		require(text(derived) == wifi, "the derivation of " + ethernet + " changed");
		require_valid(derived, parse(ethernet));
		if (host_could_give(parse(ethernet)))
			require(EthernetFromWifiMac(derived) == parse(ethernet),
				"the inverse of the derivation of " + ethernet + " is another address");
		++checked;
	}
	require(checked >= 1000, "too few golden vectors");
}

void test_the_permutation()
{
	Stream stream;
	for (int index = 0; index < 200000; ++index) {
		const std::uint64_t bits = stream.next() & (kWifiMacValues - 1);
		const std::uint64_t permuted = WifiMacPermute(bits, kWifiMacKey);
		require(permuted < kWifiMacValues, "the permutation leaves the 46 bits");
		require(WifiMacUnpermute(permuted, kWifiMacKey) == bits, "the inverse does not round-trip");
		require(WifiMacPermute(WifiMacUnpermute(bits, kWifiMacKey), kWifiMacKey) == bits,
			"the permutation does not round-trip the other way");
	}
	/* All 2^20 values below a fixed prefix map to distinct values: no hash collides. */
	std::vector<std::uint64_t> images;
	images.reserve(1U << 20);
	for (std::uint64_t low = 0; low < (1U << 20); ++low)
		images.push_back(WifiMacPermute(0x2b5ULL << 20 | low, kWifiMacKey));
	std::sort(images.begin(), images.end());
	require(std::adjacent_find(images.begin(), images.end()) == images.end(),
		"two inputs share an image");
	/* The key is the permutation's: another key permutes differently. */
	require(WifiMacPermute(12345, kWifiMacKey) != WifiMacPermute(12345, kWifiMacKey ^ 1),
		"the key does not matter");
	/* Flag bits: only the 46 others are permuted. */
	for (const char *text_value : {"02:11:22:33:44:55", "03:11:22:33:44:55", "00:11:22:33:44:55",
				       "01:11:22:33:44:55"})
		require(WifiMacBits(parse(text_value)) == WifiMacBits(parse("02:11:22:33:44:55")));
	require(WifiMacFromBits(WifiMacBits(parse("fa:ce:b0:0c:12:34"))) == parse("fa:ce:b0:0c:12:34"));
}

void test_distinct_inputs_give_distinct_addresses()
{
	/* The pair of addresses that two outputs of a 64-bit hash had merged. */
	const Mac first = parse("0a:11:22:33:44:56");
	const Mac second = parse("62:a7:34:c8:24:f6");
	require(DeriveWifiMac(first) != DeriveWifiMac(second), "the review's pair shares an address");
	Stream stream;
	std::vector<std::uint64_t> outputs;
	constexpr int count = 300000;
	for (int index = 0; index < count; ++index) {
		const Mac ethernet = stream.host_address();
		if (WifiMacReserved(ethernet))
			continue;
		const Mac wifi = DeriveWifiMac(ethernet);
		require_valid(wifi, ethernet);
		require(DeriveWifiMac(ethernet) == wifi, "not a function");
		require(EthernetFromWifiMac(wifi) == ethernet, "the inverse does not round-trip");
		outputs.push_back(PackMac(wifi));
	}
	/* The inputs are random; if two repeat, so do their outputs, and that is no collision. */
	std::sort(outputs.begin(), outputs.end());
	outputs.erase(std::unique(outputs.begin(), outputs.end()), outputs.end());
	require(outputs.size() >= static_cast<std::size_t>(count) - 4, "outputs merged");
	/* Distinct over a structured set too: every address that differs from a base in the low 20 bits. */
	std::vector<std::uint64_t> wifis;
	for (std::uint64_t low = 0; low < (1U << 20); ++low) {
		const Mac ethernet = WifiMacFromBits(0x15a3ULL << 20 | low);
		if (!WifiMacReserved(ethernet))
			wifis.push_back(PackMac(DeriveWifiMac(ethernet)));
	}
	std::sort(wifis.begin(), wifis.end());
	require(std::adjacent_find(wifis.begin(), wifis.end()) == wifis.end(), "structured inputs merged");
	/* A flipped bit changes the result. */
	const Mac base = parse("0a:11:22:33:44:55");
	for (unsigned bit = 0; bit < kWifiMacBits; ++bit) {
		const Mac other = WifiMacFromBits(WifiMacBits(base) ^ (1ULL << bit));
		require(DeriveWifiMac(other) != DeriveWifiMac(base), "a flipped bit gives the same address");
	}
	/* The old default of a host without per-installation addresses is no special case. */
	require_valid(DeriveWifiMac(parse("52:54:00:12:34:56")), parse("52:54:00:12:34:56"));
}

void test_reserved_addresses_are_walked_past()
{
	/* The address that the cycle reaches just before each reserved one: its next step is
	 * the reserved address, which the walk skips. */
	std::vector<Mac> reserved{parse("02:00:00:00:00:00")};
	for (unsigned last = 0; last < 256; ++last) {
		Mac router = parse("52:55:0a:00:02:00");
		router[5] = static_cast<unsigned char>(last);
		reserved.push_back(router);
	}
	for (const Mac &target : reserved) {
		require(WifiMacReserved(target));
		const std::uint64_t position = WifiMacPermute(WifiMacBits(target), kWifiMacKey);
		const Mac before = WifiMacFromBits(
			WifiMacUnpermute((position + kWifiMacValues - 1) % kWifiMacValues, kWifiMacKey));
		if (WifiMacReserved(before))
			continue; /* A reserved run: the next address before it does the same. */
		require(WifiMacAt(before, kWifiMacKey, 1) == target);
		const Mac derived = DeriveWifiMac(before);
		require(derived != target, "a reserved address was given: " + text(target));
		require_valid(derived, before);
		std::uint64_t step = 2;
		while (WifiMacReserved(WifiMacAt(before, kWifiMacKey, step)))
			++step;
		require(derived == WifiMacAt(before, kWifiMacKey, step), "the walk took another step");
		require(EthernetFromWifiMac(derived) == before, "the inverse missed the address before the walk");
	}
	/* A run of reserved addresses, by a predicate of the test's, and a run as long as the bound. */
	const Mac ethernet = parse("0a:11:22:33:44:55");
	for (std::uint64_t run : {1ULL, 2ULL, 5ULL, 100ULL}) {
		const Mac derived = DeriveWifiMacWith(ethernet, kWifiMacKey, [&](const Mac &mac) {
			for (std::uint64_t step = 1; step <= run; ++step)
				if (mac == WifiMacAt(ethernet, kWifiMacKey, step))
					return true;
			return false;
		});
		require(derived == WifiMacAt(ethernet, kWifiMacKey, run + 1), "a run of reserved addresses");
	}
	const Mac last = DeriveWifiMacWith(ethernet, kWifiMacKey, [](const Mac &) { return true; });
	require(last == WifiMacAt(ethernet, kWifiMacKey, kWifiMacMaxSteps), "the walk is not bounded");
}

void test_inputs_the_host_never_gives()
{
	/* The flag bits are ignored: an address that is not locally administered unicast gives the
	 * result of its locally administered unicast twin. */
	require(DeriveWifiMac(parse("ff:ff:ff:ff:ff:ff")) == DeriveWifiMac(parse("fe:ff:ff:ff:ff:ff")));
	require(DeriveWifiMac(parse("00:00:00:00:00:00")) == DeriveWifiMac(parse("02:00:00:00:00:00")));
	require(DeriveWifiMac(parse("c3:12:34:56:78:9a")) == DeriveWifiMac(parse("c2:12:34:56:78:9a")));
	/* A reserved input gives the result of the unreserved address before it on the cycle. */
	for (const char *reserved : {"02:00:00:00:00:00", "52:55:0a:00:02:02", "52:55:0a:00:02:ff"}) {
		require(WifiMacReserved(parse(reserved)));
		const Mac derived = DeriveWifiMac(parse(reserved));
		require_valid(derived, parse(reserved));
		const Mac before = EthernetFromWifiMac(derived);
		require(!WifiMacReserved(before) && DeriveWifiMac(before) == derived,
			"a reserved input does not share the result of the address before it");
	}
}

} // namespace

int main(int argc, char *argv[])
{
	require(argc == 2, "the golden vectors' path is the argument");
	test_golden_vectors(argv[1]);
	test_the_permutation();
	test_distinct_inputs_give_distinct_addresses();
	test_reserved_addresses_are_walked_past();
	test_inputs_the_host_never_gives();
	std::puts("wifi mac tests passed");
	return 0;
}
