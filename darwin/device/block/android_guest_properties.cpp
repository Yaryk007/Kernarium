// SPDX-License-Identifier: GPL-2.0-or-later

#include "android_guest_properties.hpp"

#include "android_gpt.hpp"
#include "boot_initramfs.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

namespace uml::android {
namespace {

constexpr std::array<std::string_view, 5> allowedKeys{
	"ro.product.manufacturer", "ro.product.model", "ro.soc.manufacturer",
	"ro.soc.model", "debug.disable_screen_decorations"};

/* PROP_VALUE_MAX less its terminator. */
constexpr std::size_t maximumValueLength = 91;

constexpr std::uint64_t maximumRamdiskBytes = 256ULL << 20;

bool validValue(std::string_view value) noexcept
{
	return !value.empty() && value.size() <= maximumValueLength &&
	       value.front() != ' ' && value.back() != ' ' &&
	       std::ranges::all_of(value, [](char character) {
		       return character >= ' ' && character <= '~';
	       });
}

} // namespace

bool validGuestProperties(std::string_view text) noexcept
{
	if (text.empty() || text.size() > maximumGuestPropertiesLength)
		return false;
	std::bitset<allowedKeys.size()> seen;
	while (!text.empty()) {
		const auto end = text.find('\n');
		if (end == std::string_view::npos)
			return false;
		const auto line = text.substr(0, end);
		text.remove_prefix(end + 1);
		const auto equals = line.find('=');
		if (equals == std::string_view::npos)
			return false;
		const auto key = std::ranges::find(allowedKeys, line.substr(0, equals));
		if (key == allowedKeys.end() || !validValue(line.substr(equals + 1)))
			return false;
		const auto index = static_cast<std::size_t>(key - allowedKeys.begin());
		if (seen.test(index))
			return false;
		seen.set(index);
	}
	return true;
}

BootSelection withGuestProperties(const Storage &storage,
				  const BootSelection &boot,
				  std::string_view properties,
				  const std::filesystem::path &output)
{
	if (properties.empty() || boot.kind() != RamdiskKind::stock)
		return boot;
	if (!validGuestProperties(properties))
		throw std::invalid_argument("the Android guest properties are not valid");
	validateUbdPath(output, "Android initrd");
	if (boot.ramdisk.size > maximumRamdiskBytes)
		throw std::invalid_argument("the Android boot ramdisk is too large");

	boot::InitramfsArchive archive;
	archive.addFile("/adb_debug.prop",
			std::as_bytes(std::span{properties}), 0644, 0);
	archive.addFile("/force_debuggable", {}, 0644, 0);
	const auto segment = archive.serialize();

	/* The release's ramdisk as it is, then zeros to a four-byte boundary and
	 * four more: legacy lz4 has no end of its own, and the kernel ends it at a
	 * zero chunk size. It skips zeros before the next archive, whatever the
	 * first one's format. */
	const auto size = static_cast<std::size_t>(boot.ramdisk.size);
	std::vector<std::byte> initrd((size + 3U) / 4U * 4U + 4U);
	File(storage.systemDisk()).read(boot.ramdisk.offset,
					std::span{initrd}.first(size));
	initrd.insert(initrd.end(), segment.begin(), segment.end());

	std::filesystem::remove(output);
	boot::writeNewFile(output, initrd);
	auto selected = boot;
	selected.initrdFile = output;
	selected.initrdSize = initrd.size();
	return selected;
}

} // namespace uml::android
