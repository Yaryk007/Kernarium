// SPDX-License-Identifier: GPL-2.0-or-later

/* What tells one installation of the Android guest from another: the serial
 * number and eth0's hardware address, their formats, the file that keeps them
 * beside the data disk, and when a start keeps, makes or replaces it. Every
 * value here is synthetic; no test states a value of a real installation. */

#include "android_boot.hpp"
#include "android_identity.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

namespace android = uml::android;
namespace fs = std::filesystem;
using uml::testing::require;
using uml::testing::requireThrows;

constexpr std::string_view goodContent{
	"{\n  \"ethernet_mac\": \"02:aa:bb:cc:dd:ee\",\n  \"schema\": 1,\n"
	"  \"serialno\": \"0123456789abcdef\"\n}\n"};

void writeText(const fs::path &path, std::string_view text)
{
	std::FILE *file = std::fopen(path.c_str(), "wb");
	require(file != nullptr);
	require(std::fwrite(text.data(), 1, text.size(), file) == text.size());
	require(std::fclose(file) == 0);
}

std::string readText(const fs::path &path)
{
	std::FILE *file = std::fopen(path.c_str(), "rb");
	require(file != nullptr);
	std::string text(static_cast<std::size_t>(fs::file_size(path)), '\0');
	require(std::fread(text.data(), 1, text.size(), file) == text.size());
	require(std::fclose(file) == 0);
	return text;
}

/* A random source that returns what the test lists, then repeats the last
 * row. */
android::RandomFill scripted(std::vector<std::uint8_t> bytes)
{
	return [bytes = std::move(bytes),
		position = std::size_t{0}](std::span<std::uint8_t> out) mutable {
		for (auto &byte : out)
			byte = position < bytes.size() ? bytes[position++] :
							   bytes.back();
	};
}

/* resolveIdentity as a launcher calls it: with the installation's lock. */
android::ResolvedIdentity resolve(android::IdentityRequest request)
{
	auto held = [&]() {
		if (request.file.empty() || request.stated.serial ||
		    request.stated.ethernetMac)
			return std::optional<android::InstallationLock>{};
		return android::acquireInstallationLock(request.file);
	}();
	request.lock = held ? &*held : nullptr;
	return android::resolveIdentity(request);
}

bool isLowerHex(std::string_view text)
{
	return std::ranges::all_of(text, [](char c) {
		return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
	});
}

void testSerialFormat()
{
	for (const std::string_view good :
	     {"0123456789abcdef", "ABCDEF", "a1b2c3", "Z9Z9Z9Z9Z9Z9Z9Z9Z9Z9"})
		require(android::validSerial(good), "a serial is refused");
	for (const std::string_view bad : std::initializer_list<std::string_view>{
	      "", "abcde", "abcdefghij0123456789a" /* 21 */, "unknown", "UNKNOWN",
	      "Unknown", "umlandroid-17", "abc def", "abcdef\n",
	      std::string_view{"abc\0def", 7}})
		require(!android::validSerial(bad), "a serial is taken");
	require(android::validSerial("unknown1"));
}

void testMacFormat()
{
	using android::MacAddress;
	require(android::validEthernetMac(MacAddress{0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee}));
	require(android::validEthernetMac(MacAddress{0xfe, 0xff, 0xff, 0xff, 0xff, 0xff}));
	require(android::validEthernetMac(MacAddress{0x0a, 0, 0, 0, 0, 1}));
	/* Multicast, universally administered, the platform's "no address", the
	 * user network's default and the legacy Wi-Fi address. */
	require(!android::validEthernetMac(MacAddress{0x03, 0xaa, 0xbb, 0xcc, 0xdd, 0xee}));
	require(!android::validEthernetMac(MacAddress{0x00, 0xaa, 0xbb, 0xcc, 0xdd, 0xee}));
	require(!android::validEthernetMac(MacAddress{0x01, 0xaa, 0xbb, 0xcc, 0xdd, 0xee}));
	require(android::validEthernetMac(MacAddress{0x52, 0xaa, 0xbb, 0xcc, 0xdd, 0xee}));
	require(!android::validEthernetMac(MacAddress{0x02, 0, 0, 0, 0, 0}));
	require(!android::validEthernetMac(android::defaultEthernetMac));
	require(!android::validEthernetMac(android::legacyWifiMac));
	require(android::defaultEthernetMac ==
		MacAddress{0x52, 0x54, 0x00, 0x12, 0x34, 0x56});
	/* The addresses the user network's router and servers answer from. */
	for (const std::uint8_t last : {0x00, 0x02, 0x03, 0x0f, 0xff})
		require(!android::validEthernetMac(
				MacAddress{0x52, 0x55, 0x0a, 0x00, 0x02, last}),
			"an address of the router is taken");
	require(android::validEthernetMac(MacAddress{0x52, 0x55, 0x0a, 0x00, 0x03, 0x02}));
	require(android::validEthernetMac(MacAddress{0x52, 0x55, 0x0a, 0x01, 0x02, 0x02}));

	const MacAddress mac{0x02, 0xaa, 0x0b, 0xcc, 0x0d, 0xee};
	require(android::formatMac(mac) == "02:aa:0b:cc:0d:ee");
	require(android::parseMac("02:aa:0b:cc:0d:ee") == mac);
	require(android::parseMac("02:AA:0B:CC:0D:EE") == mac);
	for (const std::string_view bad :
	     {"", "02:aa:0b:cc:0d", "02:aa:0b:cc:0d:ee:ff", "02-aa-0b-cc-0d-ee",
	      "02:aa:0b:cc:0d:eg", "2:aa:0b:cc:0d:ee", "02:aa:0b:cc:0d:e",
	      "02aa0bcc0dee", "+2:aa:0b:cc:0d:ee", " 2:aa:0b:cc:0d:ee",
	      "02:aa:0b:cc:0d:ee "})
		require(!android::parseMac(bad), "a MAC address is taken");
}

/* A generated identity has the shape the brief fixes, and the bits come from
 * the source. */
void testGeneration()
{
	/* A scripted source: the serial from the first eight bytes, the address from
	 * the next six. 0xff everywhere gives 0xfe as the address's first octet
	 * (locally administered, unicast) and 46 bits of the source. */
	auto identity = android::generateIdentity(scripted({
		0x00, 0x11, 0x22, 0x33, 0xaa, 0xbb, 0xcc, 0xdd,
		0xff, 0x01, 0x02, 0x03, 0x04, 0x05}));
	require(identity.serial == "00112233aabbccdd");
	require((identity.ethernetMac == android::MacAddress{0xfe, 0x01, 0x02, 0x03, 0x04, 0x05}));

	/* The first octet's 6 free bits survive; its two low bits are set. */
	identity = android::generateIdentity(scripted({
		1, 2, 3, 4, 5, 6, 7, 8, 0xb4, 9, 9, 9, 9, 9}));
	require(identity.ethernetMac->at(0) == 0xb6);

	/* An address the platform treats as none, the default or the legacy Wi-Fi
	 * address is not accepted: the source is asked again. */
	identity = android::generateIdentity(scripted({
		1, 2, 3, 4, 5, 6, 7, 8, 0x02, 0, 0, 0, 0, 0,
		1, 2, 3, 4, 5, 6, 7, 8, 0x52, 0x54, 0x00, 0x12, 0x34, 0x56,
		1, 2, 3, 4, 5, 6, 7, 8, 0x02, 0x15, 0xb2, 0, 0, 0,
		1, 2, 3, 4, 5, 6, 7, 8, 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02,
		9, 9, 9, 9, 9, 9, 9, 9, 0x06, 1, 2, 3, 4, 5}));
	require(identity.serial == "0909090909090909");
	require((identity.ethernetMac == android::MacAddress{0x06, 1, 2, 3, 4, 5}));
	/* A source that never gives one is an error, not a loop. */
	requireThrows<std::runtime_error>([] {
		(void)android::generateIdentity(scripted({0}));
	});

	/* With the system's source: every identity is well formed and no two of a
	 * thousand are alike (64 serial bits, 46 address bits). */
	std::set<std::string> serials;
	std::set<std::string> addresses;
	for (int index = 0; index < 1000; ++index) {
		const auto made = android::generateIdentity(android::systemRandom);
		require(made.serial && made.serial->size() == 16U &&
			isLowerHex(*made.serial));
		require(android::validSerial(*made.serial));
		require(made.ethernetMac &&
			android::validEthernetMac(*made.ethernetMac));
		require(((*made.ethernetMac)[0] & 0x03U) == 0x02U);
		serials.insert(*made.serial);
		addresses.insert(android::formatMac(*made.ethernetMac));
	}
	require(serials.size() == 1000U && addresses.size() == 1000U);
}

void testFileRoundTrip(const fs::path &directory)
{
	const auto file = directory / "identity.json";
	require(!android::readIdentityFile(file), "an absent file has no identity");
	require(android::identityFile("/instance/data.img") ==
		fs::path{"/instance/identity.json"});

	const android::InstallationIdentity identity{
		"0123456789abcdef", android::MacAddress{0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee}};
	android::writeIdentityFile(file, identity);
	require(readText(file) == goodContent);
	require(android::readIdentityFile(file) == identity);
	/* Mode 0600, and no temporary name is left beside it. */
	struct stat details {};
	require(stat(file.c_str(), &details) == 0);
	require((details.st_mode & 0777) == 0600);
	std::size_t entries = 0;
	for (const auto &entry : fs::directory_iterator(directory)) {
		(void)entry;
		++entries;
	}
	require(entries == 1U);

	/* A write replaces. */
	const android::InstallationIdentity other{
		"fedcba9876543210", android::MacAddress{0x06, 1, 2, 3, 4, 5}};
	android::writeIdentityFile(file, other);
	require(android::readIdentityFile(file) == other);

	/* A reader takes any whitespace and either case of the address. */
	writeText(file, "{\"serialno\":\"0123456789abcdef\",\r\n\t\"schema\" : 1 ,"
			"\"ethernet_mac\" : \"02:AA:BB:CC:DD:EE\"}");
	require(android::readIdentityFile(file) == identity);

	/* Values that are not an identity cannot be written. */
	requireThrows<std::invalid_argument>([&] {
		android::writeIdentityFile(file, {std::nullopt, identity.ethernetMac});
	});
	requireThrows<std::invalid_argument>([&] {
		android::writeIdentityFile(file, {"unknown", identity.ethernetMac});
	});
	requireThrows<std::invalid_argument>([&] {
		android::writeIdentityFile(
			file, {identity.serial, android::defaultEthernetMac});
	});
	/* A directory that is not there fails as a system error and leaves nothing. */
	requireThrows<std::system_error>([&] {
		android::writeIdentityFile(directory / "missing" / "identity.json",
					   identity);
	});
}

void testInvalidFiles(const fs::path &directory)
{
	const auto file = directory / "identity.json";
	const auto mac = "\"ethernet_mac\": \"02:aa:bb:cc:dd:ee\"";
	const auto serial = "\"serialno\": \"0123456789abcdef\"";
	const std::vector<std::string> bad{
		"",
		"{}",
		"[]",
		"not json",
		std::string{"{"} + mac + ", \"schema\": 1}",
		std::string{"{"} + serial + ", \"schema\": 1}",
		std::string{"{"} + mac + "," + serial + "}",
		std::string{"{"} + mac + "," + serial + ", \"schema\": 2}",
		std::string{"{"} + mac + "," + serial + ", \"schema\": \"1\"}",
		std::string{"{"} + mac + "," + serial + ", \"schema\": 01}",
		std::string{"{"} + mac + "," + serial + ", \"schema\": 1, \"extra\": 1}",
		std::string{"{"} + mac + "," + serial + ", \"schema\": 1," + mac + "}",
		std::string{"{"} + mac + "," + serial + ", \"schema\": 1} trailing",
		std::string{"{"} + mac + "," + serial + ", \"schema\": 1,}",
		"{\"ethernet_mac\": \"02:aa:bb:cc:dd:ee\", \"serialno\": "
		"\"0123456789abcdef\", \"schema\": 1} {}",
		/* Values: a serial that is none, a multicast, universal, zero,
		 * default and legacy address, and a malformed one. */
		"{\"ethernet_mac\": \"02:aa:bb:cc:dd:ee\", \"schema\": 1, "
		"\"serialno\": \"unknown\"}",
		"{\"ethernet_mac\": \"02:aa:bb:cc:dd:ee\", \"schema\": 1, "
		"\"serialno\": \"abc\"}",
		"{\"ethernet_mac\": \"02:aa:bb:cc:dd:ee\", \"schema\": 1, "
		"\"serialno\": 5}",
		"{\"ethernet_mac\": \"03:aa:bb:cc:dd:ee\", \"schema\": 1, "
		"\"serialno\": \"0123456789abcdef\"}",
		"{\"ethernet_mac\": \"00:aa:bb:cc:dd:ee\", \"schema\": 1, "
		"\"serialno\": \"0123456789abcdef\"}",
		"{\"ethernet_mac\": \"02:00:00:00:00:00\", \"schema\": 1, "
		"\"serialno\": \"0123456789abcdef\"}",
		"{\"ethernet_mac\": \"52:54:00:12:34:56\", \"schema\": 1, "
		"\"serialno\": \"0123456789abcdef\"}",
		"{\"ethernet_mac\": \"02:15:b2:00:00:00\", \"schema\": 1, "
		"\"serialno\": \"0123456789abcdef\"}",
		"{\"ethernet_mac\": \"52:55:0a:00:02:02\", \"schema\": 1, "
		"\"serialno\": \"0123456789abcdef\"}",
		"{\"ethernet_mac\": \"02:aa:bb:cc:dd\", \"schema\": 1, "
		"\"serialno\": \"0123456789abcdef\"}",
		"{\"ethernet_mac\": 2, \"schema\": 1, "
		"\"serialno\": \"0123456789abcdef\"}",
		/* An escape in a string. */
		"{\"ethernet_mac\": \"02:aa:bb:cc:dd:ee\", \"schema\": 1, "
		"\"serialno\": \"0123456789abc\\u0064f\"}",
	};
	for (const auto &text : bad) {
		writeText(file, text);
		requireThrows<std::invalid_argument>(
			[&] { (void)android::readIdentityFile(file); },
			("an invalid file is taken: " + text).c_str());
	}
	/* Too large. */
	writeText(file, std::string(5000, ' '));
	requireThrows<std::invalid_argument>(
		[&] { (void)android::readIdentityFile(file); });
	fs::remove(file);
	/* Not a regular file: a link, even to a good file, and a directory. */
	writeText(directory / "good.json", goodContent);
	fs::create_symlink(directory / "good.json", file);
	requireThrows<std::invalid_argument>(
		[&] { (void)android::readIdentityFile(file); });
	fs::remove(file);
	fs::create_directory(file);
	requireThrows<std::invalid_argument>(
		[&] { (void)android::readIdentityFile(file); });
}

void testResolution(const fs::path &directory)
{
	const auto file = directory / "identity.json";

	/* Without a file and with nothing stated the guest keeps its fixed
	 * values. */
	auto resolved = android::resolveIdentity({});
	require(resolved.outcome == android::IdentityOutcome::fixed &&
		!resolved.identity.serial && !resolved.identity.ethernetMac);

	/* Stated values are used as given, alone or together, and neither read
	 * nor written: not a file that is there, not one that is not. */
	writeText(file, goodContent);
	for (const android::InstallationIdentity &stated :
	     {android::InstallationIdentity{"abcdef123456", std::nullopt},
	      android::InstallationIdentity{
		      std::nullopt, android::MacAddress{0x0a, 1, 2, 3, 4, 5}},
	      android::InstallationIdentity{
		      "abcdef123456", android::MacAddress{0x0a, 1, 2, 3, 4, 5}}}) {
		resolved = resolve(
			{.file = file, .diskCreated = true, .stated = stated});
		require(resolved.outcome == android::IdentityOutcome::stated &&
			resolved.identity == stated);
		require(readText(file) == goodContent, "a stated value touched the file");
	}
	const auto absent = directory / "absent.json";
	resolved = resolve(
		{.file = absent, .stated = {"abcdef123456", std::nullopt}});
	require(resolved.outcome == android::IdentityOutcome::stated);
	require(!fs::exists(absent), "a stated value made a file");

	/* An installation without a file gets one; the next start keeps it. */
	fs::remove(file);
	const auto made = resolve({.file = file});
	require(made.outcome == android::IdentityOutcome::created);
	require(made.identity.serial && made.identity.ethernetMac);
	require(android::readIdentityFile(file) == made.identity);
	const auto again = resolve({.file = file});
	require(again.outcome == android::IdentityOutcome::kept &&
		again.identity == made.identity);
	/* A start that made the data disk anew is a new installation: new values,
	 * written over the file. */
	const auto fresh = resolve({.file = file, .diskCreated = true});
	require(fresh.outcome == android::IdentityOutcome::created);
	require(fresh.identity.serial != made.identity.serial &&
		fresh.identity.ethernetMac != made.identity.ethernetMac);
	require(android::readIdentityFile(file) == fresh.identity);
	require(resolve({.file = file}).identity == fresh.identity);

	/* Two installations differ. */
	const auto second = directory / "second" / "identity.json";
	fs::create_directory(second.parent_path());
	const auto other = resolve({.file = second});
	require(other.identity.serial != fresh.identity.serial &&
		other.identity.ethernetMac != fresh.identity.ethernetMac);

	/* A file that is not an identity is replaced; one that cannot be read is
	 * left and the start takes values it will not keep. */
	writeText(file, "{\"schema\": 2}");
	auto replaced = resolve({.file = file});
	require(replaced.outcome == android::IdentityOutcome::replaced);
	require(android::readIdentityFile(file) == replaced.identity);
	require(chmod(file.c_str(), 0) == 0);
	const auto before = replaced.identity;
	const auto unread = resolve({.file = file});
	require(unread.outcome == android::IdentityOutcome::unsaved);
	require(unread.identity.serial && unread.identity != before);
	require(chmod(file.c_str(), 0600) == 0);
	require(android::readIdentityFile(file) == before, "an unreadable file was replaced");

	/* A file that cannot be written does not fail the start. */
	const auto nowhere = directory / "nowhere" / "identity.json";
	const auto unsaved = resolve({.file = nowhere});
	require(unsaved.outcome == android::IdentityOutcome::unsaved);
	require(unsaved.identity.serial && unsaved.identity.ethernetMac);
	require(!fs::exists(nowhere.parent_path()));

	/* The random source is the request's. */
	fs::remove(file);
	const auto scriptedIdentity = resolve(
		{.file = file,
		 .random = scripted({0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89,
				     0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc})});
	require(scriptedIdentity.identity.serial == "abcdef0123456789");
	require((scriptedIdentity.identity.ethernetMac ==
		 android::MacAddress{0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc}));
}


/* The file is opened once and everything after is about the opened file: a
 * name that is a link, a pipe or a directory is not an identity, a pipe with no
 * writer does not block, a file cut short or grown past the bound is not one
 * and is not a system error, and a read that fails is. */
void testReadSafety(const fs::path &directory)
{
	const auto file = directory / "identity.json";
	/* Pipes must not hang a start; the alarm turns a hang into a failure. */
	alarm(60);

	/* A link, even to a good file: not followed. */
	writeText(directory / "target.json", goodContent);
	fs::create_symlink(directory / "target.json", file);
	requireThrows<std::invalid_argument>(
		[&] { (void)android::readIdentityFile(file); },
		"a link to a good file was followed");
	/* A start replaces the link itself and leaves the target alone. */
	const auto replacedLink = resolve({.file = file});
	require(replacedLink.outcome == android::IdentityOutcome::replaced);
	require(!fs::is_symlink(file) && android::readIdentityFile(file) == replacedLink.identity);
	require(readText(directory / "target.json") == goodContent, "a link's target was written");
	fs::remove(file);

	/* A pipe with no writer: not read, and not waited for. */
	require(mkfifo(file.c_str(), 0600) == 0);
	requireThrows<std::invalid_argument>(
		[&] { (void)android::readIdentityFile(file); },
		"a pipe was read");
	const auto overPipe = resolve({.file = file});
	require(overPipe.outcome == android::IdentityOutcome::replaced ||
		overPipe.outcome == android::IdentityOutcome::unsaved);
	fs::remove(file);

	/* A directory. */
	fs::create_directory(file);
	requireThrows<std::invalid_argument>(
		[&] { (void)android::readIdentityFile(file); });
	fs::remove(file);

	/* The bound: a file of exactly 4096 bytes that is a good identity is read,
	 * one byte more is too large, and neither is a system error. */
	std::string padded{goodContent};
	padded.resize(4096, ' ');
	writeText(file, padded);
	require(android::readIdentityFile(file).has_value());
	padded.push_back(' ');
	writeText(file, padded);
	requireThrows<std::invalid_argument>(
		[&] { (void)android::readIdentityFile(file); });

	/* A file cut short is not an identity, whatever its length: no read error
	 * escapes, and a start replaces it. */
	for (const std::string_view part :
	     {"", "{", "{\"ethernet_mac\": \"02:aa:bb", "{\"ethernet_mac\": \"02:aa:bb:cc:dd:ee\","}) {
		writeText(file, part);
		requireThrows<std::invalid_argument>(
			[&] { (void)android::readIdentityFile(file); },
			("a short file is taken: " + std::string{part}).c_str());
	}
	writeText(file, goodContent.substr(0, 40));
	require(resolve({.file = file}).outcome == android::IdentityOutcome::replaced);

	/* What is read is the opened file: a name that is replaced after the open
	 * by a link to another good identity changes nothing. */
	fs::remove(file);
	writeText(file, goodContent);
	const int descriptor = open(file.c_str(), O_RDONLY | O_CLOEXEC);
	require(descriptor >= 0);
	std::string other{goodContent};
	other.replace(other.find("0123456789abcdef"), 16, "fedcba9876543210");
	writeText(directory / "other.json", other);
	fs::remove(file);
	fs::create_symlink(directory / "other.json", file);
	const auto opened = android::readIdentityDescriptor(descriptor, file);
	require(opened.serial == "0123456789abcdef",
		"the name's new target was read in place of the opened file");
	require(close(descriptor) == 0);
	fs::remove(file);

	/* A read that fails is the system's error, which a start answers with
	 * values it does not keep. */
	writeText(file, goodContent);
	const int writeOnly = open(file.c_str(), O_WRONLY | O_CLOEXEC);
	require(writeOnly >= 0);
	requireThrows<std::system_error>(
		[&] { (void)android::readIdentityDescriptor(writeOnly, file); },
		"a failed read was not the system's error");
	require(close(writeOnly) == 0);
	alarm(0);
}

/* Two starts of an installation with no identity file: the one that holds the
 * lock makes it, and the other reads what it made. */
void testTwoCreators(const fs::path &directory)
{
	const auto file = directory / "identity.json";
	std::atomic<bool> firstHolds{false};
	std::atomic<bool> secondTries{false};
	const auto scriptedBytes = [](std::uint8_t base) {
		return [base](std::span<std::uint8_t> out) {
			for (std::size_t index = 0; index < out.size(); ++index)
				out[index] = static_cast<std::uint8_t>(base + index);
		};
	};
	android::ResolvedIdentity first;
	android::ResolvedIdentity second;
	std::thread one{[&] {
		auto held = android::acquireInstallationLock(file);
		require(held.has_value());
		firstHolds = true;
		/* The first creator is slow: the second arrives while it works. */
		first = android::resolveIdentity(
			{.file = file,
			 .random = [&](std::span<std::uint8_t> out) {
				 while (!secondTries)
					 std::this_thread::yield();
				 std::this_thread::sleep_for(
					 std::chrono::milliseconds{300});
				 scriptedBytes(0x10)(out);
			 },
			 .lock = &*held});
	}};
	std::thread two{[&] {
		while (!firstHolds)
			std::this_thread::yield();
		secondTries = true;
		auto held = android::acquireInstallationLock(file);
		require(held.has_value());
		second = android::resolveIdentity(
			{.file = file, .random = scriptedBytes(0x80), .lock = &*held});
	}};
	one.join();
	two.join();
	require(first.outcome == android::IdentityOutcome::created,
		"the first creator did not make the identity");
	require(second.outcome == android::IdentityOutcome::kept,
		"the second creator made an identity of its own");
	require(first.identity == second.identity);
	require(android::readIdentityFile(file) == first.identity);

	/* The lock is waited for with a limit, and a lock of another installation
	 * is a defect. */
	{
		const auto held = android::acquireInstallationLock(file);
		require(held.has_value());
		try {
			(void)android::acquireInstallationLock(
				file, std::chrono::milliseconds{100});
			require(false, "a held installation was locked twice");
		} catch (const std::system_error &error) {
			require(error.code().value() == ETIMEDOUT);
		}
		const auto elsewhere = directory / "elsewhere";
		fs::create_directory(elsewhere);
		requireThrows<std::logic_error>([&] {
			(void)android::resolveIdentity(
				{.file = elsewhere / "identity.json", .lock = &*held});
		});
	}
	/* Released, it is free again; the file is never removed. */
	require(android::acquireInstallationLock(file).has_value());
	require(fs::exists(android::installationLockFile(file)));
	require(android::installationLockFile(file) == directory / "identity.lock");
}

/* Writers never share a temporary name, in one process or between processes:
 * nothing collides, nothing is left behind, and the file is always whole. */
void testWritersDoNotCollide(const fs::path &directory)
{
	const auto file = directory / "identity.json";
	std::atomic<bool> failed{false};
	const auto writer = [&](std::uint8_t base) {
		return [&, base] {
			for (int round = 0; round < 300 && !failed; ++round) {
				try {
					android::writeIdentityFile(
						file,
						{std::string{"serial"} + std::to_string(base) +
							 std::to_string(round % 10) + "xx",
						 android::MacAddress{0x02, base, 0, 0, 0,
								     static_cast<std::uint8_t>(round)}});
				} catch (...) {
					failed = true;
				}
			}
		};
	};
	std::thread one{writer(1)};
	std::thread two{writer(2)};
	one.join();
	two.join();
	require(!failed, "two writers collided");
	require(android::readIdentityFile(file).has_value());
	for (const auto &entry : fs::directory_iterator(directory))
		require(entry.path().filename() == "identity.json" ||
				entry.path().filename() == "identity.lock",
			"a writer left " + entry.path().filename().string());
}


/* A start's preparation of an installation: the lock is taken before the disk
 * is touched, the disk and the identity follow each other, and two starts of
 * one installation agree. */
void testPrepareInstallation(const fs::path &directory, const fs::path &fixture)
{
	const auto layout =
		android::readLayout(fixture / "release" / android::releaseLayout);
	const auto disk = directory / "data.img";
	const auto file = directory / "identity.json";

	/* A new installation: the disk and an identity. */
	auto first = android::prepareInstallation(layout, disk, 0, {.file = file});
	require(first.disk.change == android::DataDiskChange::created);
	require(first.identity.outcome == android::IdentityOutcome::created);
	require(fs::exists(disk) && android::readIdentityFile(file) == first.identity.identity);
	/* The next start finds both. */
	auto again = android::prepareInstallation(layout, disk, 0, {.file = file});
	require(again.disk.change == android::DataDiskChange::none);
	require(again.identity.outcome == android::IdentityOutcome::kept &&
		again.identity.identity == first.identity.identity);
	/* A larger disk keeps the identity. */
	const auto larger = layout.data.defaultGiB + 1U;
	require(larger <= layout.data.maximumGiB);
	auto grown = android::prepareInstallation(layout, disk, larger, {.file = file});
	require(grown.disk.change == android::DataDiskChange::grown &&
		grown.identity.outcome == android::IdentityOutcome::kept &&
		grown.identity.identity == first.identity.identity);
	/* An installation that has a disk and no file (one that predates the
	 * identity) gets one and keeps it. */
	fs::remove(file);
	auto lazily = android::prepareInstallation(layout, disk, 0, {.file = file});
	require(lazily.disk.change == android::DataDiskChange::none &&
		lazily.identity.outcome == android::IdentityOutcome::created);
	require(lazily.identity.identity != first.identity.identity);
	/* A disk made anew is a new installation, whatever file lies beside it. */
	fs::remove(disk);
	auto anew = android::prepareInstallation(layout, disk, 0, {.file = file});
	require(anew.disk.change == android::DataDiskChange::created &&
		anew.identity.outcome == android::IdentityOutcome::created &&
		anew.identity.identity != lazily.identity.identity);

	/* The lock comes before the disk: an installation that stays locked is
	 * refused before anything of its disk exists. */
	const auto busy = directory / "busy";
	fs::create_directory(busy);
	{
		const auto held = android::acquireInstallationLock(busy / "identity.json");
		require(held.has_value());
		try {
			(void)android::prepareInstallation(
				layout, busy / "data.img", 0,
				{.file = busy / "identity.json"},
				std::chrono::milliseconds{100});
			require(false, "a locked installation was prepared");
		} catch (const std::system_error &error) {
			require(error.code().value() == ETIMEDOUT);
		}
		require(!fs::exists(busy / "data.img"), "the disk was made before the lock was held");
		require(!fs::exists(busy / "identity.json"));
	}

	/* A start that states values has no file and takes no lock. */
	const auto stated = directory / "stated";
	fs::create_directory(stated);
	auto given = android::prepareInstallation(
		layout, stated / "data.img", 0,
		{.file = stated / "identity.json",
		 .stated = {"abcdef123456", std::nullopt}});
	require(given.disk.change == android::DataDiskChange::created &&
		given.identity.outcome == android::IdentityOutcome::stated);
	require(!fs::exists(stated / "identity.json") && !fs::exists(stated / "identity.lock"));
}

/* Two starts of one new installation at once: the disk is made once, and both
 * start with the identity that was made with it. */
void testTwoStartsOfOneInstallation(const fs::path &directory, const fs::path &fixture)
{
	const auto layout =
		android::readLayout(fixture / "release" / android::releaseLayout);
	for (int round = 0; round < 12; ++round) {
		const auto installation = directory / ("round-" + std::to_string(round));
		fs::create_directory(installation);
		android::PreparedInstallation one;
		android::PreparedInstallation two;
		std::atomic<bool> go{false};
		const auto start = [&](android::PreparedInstallation &result,
				       std::uint8_t base) {
			return [&, base] {
				while (!go)
					std::this_thread::yield();
				result = android::prepareInstallation(
					layout, installation / "data.img", 0,
					{.file = installation / "identity.json",
					 .random = [base](std::span<std::uint8_t> out) {
						 for (std::size_t index = 0; index < out.size(); ++index)
							 out[index] = static_cast<std::uint8_t>(base + index);
					 }});
			};
		};
		std::thread a{start(one, 0x10)};
		std::thread b{start(two, 0x80)};
		go = true;
		a.join();
		b.join();
		require(one.identity.identity == two.identity.identity,
			"two starts of one installation have different identities");
		require((one.disk.change == android::DataDiskChange::created) !=
				(two.disk.change == android::DataDiskChange::created),
			"the disk was not made exactly once");
		require(android::readIdentityFile(installation / "identity.json") ==
			one.identity.identity);
	}
}

} // namespace

int main(int argc, char *argv[])
{
	if (argc != 2) {
		std::fprintf(stderr, "usage: %s FIXTURE_DIRECTORY\n", argv[0]);
		return 2;
	}
	const fs::path fixtureRoot{argv[1]};
	const uml::testing::ScratchSubdirectory scratch{
		"uml-android-identity-tests.XXXXXX"};
	const auto fresh = [&](const char *name) {
		const auto path = scratch.path() / name;
		fs::create_directory(path);
		return path;
	};
	testSerialFormat();
	testMacFormat();
	testGeneration();
	testFileRoundTrip(fresh("round-trip"));
	testInvalidFiles(fresh("invalid"));
	testResolution(fresh("resolution"));
	testReadSafety(fresh("read-safety"));
	testTwoCreators(fresh("two-creators"));
	testWritersDoNotCollide(fresh("writers"));
	testPrepareInstallation(fresh("prepare"), fixtureRoot);
	testTwoStartsOfOneInstallation(fresh("two-starts"), fixtureRoot);
	return 0;
}
