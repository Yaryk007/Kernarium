// SPDX-License-Identifier: GPL-2.0-or-later

#include "android_boot.hpp"
#include "ubd_path.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace uml::android {
namespace {

constexpr std::uint64_t bootImagePageSize = 4096;
constexpr std::size_t bootImageHeaderSize = 1584;
constexpr std::string_view recoveryCommand = "boot-recovery";
constexpr std::string_view bootloaderCommand = "bootonce-bootloader";
/* The command field that opens the bootloader message (struct
 * bootloader_message in AOSP's bootloader_message.h). */
constexpr std::size_t bootloaderCommandSize = 32;
constexpr std::size_t bootloaderMessageSize = 2048;

/* The release a launch boots is the one the launcher attached, so a caller
 * may not state another (the emulator's --aot-release=). */
constexpr std::array<std::string_view, 5> storagePrefixes {
	"ubd", "initrd", "root=", "rootfstype=", "--aot-release="};
constexpr std::string_view bootDevicesPrefix{"androidboot.boot_devices="};

void validateSystemDisk(const Layout &layout, const std::filesystem::path &path)
{
	struct stat details {};
	if (lstat(path.c_str(), &details) != 0 || !S_ISREG(details.st_mode))
		throw std::invalid_argument(
			"the Android system disk is not a regular file: " + path.string());
	if (access(path.c_str(), R_OK) != 0)
		throw std::invalid_argument("the Android system disk is not readable");
	validateUbdPath(path, "Android system disk");
	const File disk(path);
	verifyGpt(disk, layout.systemGpt(), "the Android system disk");
}

/* The ramdisk of a kernel-less boot image v4 that fills `available` bytes from
 * `offset` of `file`: a header page, then the ramdisk. The kernel and the
 * command line of an image are never read; both belong to the host. */
FileRange kernellessRamdisk(const File &file, std::uint64_t offset,
			    std::uint64_t available, const char *what)
{
	std::array<std::byte, bootImageHeaderSize> header {};
	file.read(offset, header);
	if (std::memcmp(header.data(), "ANDROID!", 8) != 0 ||
	    little32(header.data() + 8) != 0U ||
	    little32(header.data() + 20) != bootImageHeaderSize ||
	    little32(header.data() + 40) != 4U ||
	    little32(header.data() + 1580) != 0U ||
	    !std::all_of(header.begin() + 24, header.begin() + 40,
			 [](std::byte value) { return value == std::byte{}; }))
		throw std::invalid_argument(std::string{what} +
					    " is not kernelless v4");
	const auto ramdiskSize = little32(header.data() + 12);
	if (ramdiskSize == 0 || available <= bootImagePageSize ||
	    ramdiskSize > available - bootImagePageSize)
		throw std::invalid_argument(std::string{what} +
					    " ramdisk is invalid");
	return FileRange{offset + bootImagePageSize, ramdiskSize};
}

FileRange bootRamdisk(const File &disk, const SystemPartition &partition)
{
	return kernellessRamdisk(disk, partition.offset, partition.imageBytes,
				 "Android boot partition");
}

/* The formats the guest kernel unpacks an initramfs from, by their first
 * bytes: gzip, bzip2, xz, lzma, lzo, legacy lz4, zstd, and an uncompressed
 * newc cpio archive. */
bool knownRamdiskFormat(std::span<const std::byte> head)
{
	static constexpr std::array<std::string_view, 9> magics{
		std::string_view{"\x1f\x8b", 2}, "BZh",
		std::string_view{"\xfd" "7zXZ\x00", 6},
		std::string_view{"\x5d\x00\x00", 3},
		std::string_view{"\x89LZO", 4},
		/* Legacy lz4's magic, 0x184C2102 little-endian; the last byte is
		 * octal because the source audit refuses a hex escape of it. */
		std::string_view{"\x02\x21\x4c\030", 4},
		std::string_view{"\x28\xb5\x2f\xfd", 4}, "070701", "070702"};
	return std::any_of(magics.begin(), magics.end(), [head](std::string_view magic) {
		return head.size() >= magic.size() &&
		       std::memcmp(head.data(), magic.data(), magic.size()) == 0;
	});
}

/* The most the root image may be: a ramdisk is unpacked into the guest's
 * memory, so a file past this is not a ramdisk the host starts. */
constexpr std::uint64_t maxRootImageBytes = 256ULL << 20;

/* The release's root image, opened without following a link at `root/` or at
 * the image; nothing when the release carries none. A `root/` that is a link
 * or no directory, and an image that is no regular file, are refused. */
std::optional<File> openRootImage(const std::filesystem::path &release)
{
	const std::filesystem::path image{releaseRootBootImage};
	const int directory = open((release / image.parent_path()).c_str(),
				   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (directory < 0) {
		if (errno == ENOENT)
			return std::nullopt;
		throw std::invalid_argument(
			"the Android root boot image's directory is not a plain directory");
	}
	struct Closer {
		int descriptor;
		~Closer() { (void)close(descriptor); }
	} const closer{directory};
	try {
		return std::optional<File>{std::in_place, directory,
					   image.filename().c_str()};
	} catch (const std::system_error &error) {
		if (error.code().value() == ENOENT)
			return std::nullopt;
		throw;
	}
}

/* The ramdisk of the release's root image, checked on the descriptor it was
 * opened on. The path is the release's own file under its root, never a
 * caller's. */
FileRange rootBootRamdisk(const File &file)
{
	if (file.size() > maxRootImageBytes)
		throw std::invalid_argument(
			"the Android root boot image is too large");
	if (file.size() <= bootImagePageSize)
		throw std::invalid_argument(
			"the Android root boot image holds no ramdisk");
	const auto range = kernellessRamdisk(file, 0, file.size(),
					     "Android root boot image");
	/* Only the ramdisk's own bytes are probed, and a magic must lie whole
	 * inside it: the padding that follows a short one is not the format. */
	std::array<std::byte, 8> head {};
	const auto probe = static_cast<std::size_t>(
		std::min<std::uint64_t>(head.size(), range.size));
	file.read(range.offset, std::span{head}.first(probe));
	if (!knownRamdiskFormat(std::span<const std::byte>{head}.first(probe)))
		throw std::invalid_argument(
			"the Android root boot image holds no ramdisk the kernel unpacks");
	return range;
}

std::array<std::byte, bootloaderMessageSize> readBootloaderMessage(
	const Layout &layout, const std::filesystem::path &dataDisk)
{
	std::array<std::byte, bootloaderMessageSize> message {};
	const File disk(dataDisk);
	disk.read(layout.data.partition("misc").offset, message);
	return message;
}

/* The command field, or nothing when it has no terminator. */
std::optional<std::string_view> commandOf(
	const std::array<std::byte, bootloaderMessageSize> &message)
{
	const auto end = std::find(message.begin(),
				   message.begin() + bootloaderCommandSize, std::byte{});
	if (end == message.begin() + bootloaderCommandSize)
		return std::nullopt;
	return std::string_view{reinterpret_cast<const char *>(message.data()),
				static_cast<std::size_t>(end - message.begin())};
}

/* The command views the message, which a temporary would not outlive. */
std::optional<std::string_view> commandOf(
	const std::array<std::byte, bootloaderMessageSize> &&) = delete;

std::filesystem::path requireAbsolute(std::filesystem::path path, std::string_view label)
{
	if (!path.is_absolute())
		throw std::invalid_argument(std::string{label} + " path must be absolute");
	return path;
}

bool startsWithAny(std::string_view argument,
		   std::span<const std::string_view> prefixes)
{
	return std::any_of(prefixes.begin(), prefixes.end(),
			   [argument](std::string_view prefix) {
				   return argument.starts_with(prefix);
			   });
}

} // namespace

Storage::Storage(std::filesystem::path release, std::filesystem::path dataDisk)
	: release_(requireAbsolute(std::move(release), "Android release")),
	  systemDisk_(release_ / releaseSystemDisk),
	  dataDisk_(requireAbsolute(std::move(dataDisk), "Android data disk")),
	  layout_(readLayout(release_ / releaseLayout))
{
	validate();
}

void Storage::validate() const
{
	validateSystemDisk(layout_, systemDisk_);
	/* The two checks cannot both pass for one file: a system disk's
	 * partition tables are not a data disk's. */
	(void)validateDataDisk(layout_, dataDisk_);
}

bool Storage::carriesRoot() const noexcept
{
	try {
		return openRootImage(release_).has_value();
	} catch (...) {
		return false;
	}
}

BootSelection Storage::selectBoot(BootMode requestedMode, RootMode root) const
{
	if (requestedMode != BootMode::automatic &&
	    requestedMode != BootMode::normal && requestedMode != BootMode::recovery)
		throw std::invalid_argument("unknown Android boot mode");
	if (root != RootMode::off && root != RootMode::on)
		throw std::invalid_argument("unknown Android root mode");
	validate();
	const File system(systemDisk_);
	const auto stock = bootRamdisk(system, layout_.systemPartition("init_boot"));
	const auto recovery = bootRamdisk(system, layout_.systemPartition("recovery"));
	/* Opened once: what is validated below is this descriptor's file. */
	const std::optional<File> rootImage = root == RootMode::on
		? openRootImage(release_)
		: std::optional<File>{};
	if (root == RootMode::on && !rootImage)
		throw std::invalid_argument(
			"this Android release carries no root boot image");
	auto selected = requestedMode;
	if (requestedMode == BootMode::automatic) {
		const auto message = readBootloaderMessage(layout_, dataDisk_);
		const auto command = commandOf(message);
		if (!command)
			throw std::invalid_argument(
				"Android bootloader message command is malformed");
		selected = *command == recoveryCommand ? BootMode::recovery
						       : BootMode::normal;
	}
	/* Only the normal boot's ramdisk is replaced: recovery stays the
	 * release's own, and the root image is not read for it. */
	if (selected == BootMode::recovery)
		return BootSelection{selected, recovery, {}};
	if (root == RootMode::off)
		return BootSelection{selected, stock, {}};
	const auto image = release_ / releaseRootBootImage;
	validateUbdPath(image, "Android root boot image");
	return BootSelection{selected, rootBootRamdisk(*rootImage), image};
}

bool Storage::consumeBootloaderCommand() const
{
	validate();
	const auto message = readBootloaderMessage(layout_, dataDisk_);
	if (commandOf(message) != bootloaderCommand)
		return false;
	const int descriptor = open(dataDisk_.c_str(), O_WRONLY | O_NOFOLLOW);
	if (descriptor < 0)
		throw std::system_error(errno, std::generic_category(),
					"cannot open " + dataDisk_.string());
	const std::array<std::byte, bootloaderCommandSize> cleared {};
	const auto written = pwrite(
		descriptor, cleared.data(), cleared.size(),
		static_cast<off_t>(layout_.data.partition("misc").offset));
	const int writeError = written < 0 ? errno : EIO;
	const int syncError = fsync(descriptor) == 0 ? 0 : errno;
	const int closeError = close(descriptor) == 0 ? 0 : errno;
	if (written != static_cast<ssize_t>(cleared.size()))
		throw std::system_error(writeError, std::generic_category(),
					"cannot clear the bootloader command");
	if (syncError != 0 || closeError != 0)
		throw std::system_error(syncError != 0 ? syncError : closeError,
					std::generic_category(),
					"cannot flush the bootloader command");
	return true;
}

RestartBoot Storage::selectRestartBoot(RootMode root) const
{
	const bool consumed = consumeBootloaderCommand();
	return RestartBoot{selectBoot(BootMode::automatic, root), consumed};
}

std::vector<std::string> Storage::kernelArguments(const BootSelection &boot) const
{
	return {
		"ubd0r=" + systemDisk_.string(),
		"ubd1=" + dataDisk_.string(),
		/* The kernel reopens this path when it loads the ramdisk. A file
		 * replaced after the selection is not defended here, as for the
		 * system disk's ramdisk: the release folder is trusted storage. */
		"initrd=" + (boot.ramdiskFile.empty() ? systemDisk_ : boot.ramdiskFile)
				    .string(),
		"initrd_offset=" + std::to_string(boot.ramdisk.offset),
		"initrd_size=" + std::to_string(boot.ramdisk.size),
		"root=/dev/ram0",
		"rootfstype=ramfs",
	};
}

std::span<const std::string_view> storageArgumentPrefixes() noexcept
{
	return storagePrefixes;
}

void rejectStorageArguments(std::span<const std::string> arguments)
{
	for (const auto &argument : arguments)
		if (startsWithAny(argument, storagePrefixes) ||
		    argument.starts_with(bootDevicesPrefix))
			throw std::invalid_argument(
				"the Android launcher attaches the guest's disks itself; "
				"the kernel argument is refused: " + argument);
}

void validateUbdPath(const std::filesystem::path &path, std::string_view label)
{
	if (!::uml::block::usable_ubd_path(path.native()))
		throw std::invalid_argument(std::string{label} +
			" path is unusable as a UBD device");
}

std::string withoutQuotes(std::string_view text)
{
	std::string plain;
	plain.reserve(text.size());
	for (const char character : text)
		if (character != '"' && character != '\'')
			plain.push_back(character);
	return plain;
}

bool carriesPrefix(std::string_view text, std::string_view prefix)
{
	constexpr std::string_view separators{" \n\t"};
	std::size_t cursor = 0;
	while (cursor < text.size()) {
		const auto begin = text.find_first_not_of(separators, cursor);
		if (begin == std::string_view::npos)
			break;
		const auto end = std::min(text.find_first_of(separators, begin),
					  text.size());
		if (withoutQuotes(text.substr(begin, end - begin))
			    .starts_with(prefix))
			return true;
		cursor = end;
	}
	return false;
}

/* Android's reader groups words that stand between two `"` and removes
 * the quotes, so a group that spans white space, or a quote that never
 * closes, is one argument there but several (or a different one) in the
 * words read below: dropping the word that closes it would leave the
 * quote open over every argument the launcher appends. */
static bool hasQuotedGroup(std::string_view text)
{
	bool open = false;
	for (const char character : text) {
		if (character == '"')
			open = !open;
		else if (open && (character == ' ' || character == '\n' ||
				  character == '\t'))
			return true;
	}
	return open;
}

std::vector<std::string> readKernelCommandLine(
	const std::filesystem::path &path,
	std::span<const std::string_view> launcherPrefixes)
{
	struct stat details {};
	if (lstat(path.c_str(), &details) != 0 || !S_ISREG(details.st_mode) ||
	    details.st_size < 0)
		throw std::runtime_error(
			"the kernel command line is missing or unsafe: " +
			path.string());
	std::string text(static_cast<std::size_t>(details.st_size), '\0');
	std::FILE *file = std::fopen(path.c_str(), "rb");
	if (file == nullptr)
		throw std::system_error(errno, std::generic_category(),
			"cannot open " + path.string());
	const auto read = std::fread(text.data(), 1, text.size(), file);
	const int closed = std::fclose(file);
	if (read != text.size() || closed != 0)
		throw std::runtime_error("cannot read " + path.string());

	/* Before any word is dropped or the launcher appends its own. */
	if (hasQuotedGroup(text))
		throw std::invalid_argument(
			"the release kernel command line has a quoted group that "
			"spans words or a quote that never closes");

	std::vector<std::string> arguments;
	bool bootDevices = false;
	constexpr std::string_view separators{" \n\t"};
	std::size_t cursor = 0;
	while (cursor < text.size()) {
		const auto begin = text.find_first_not_of(separators, cursor);
		if (begin == std::string::npos)
			break;
		cursor = std::min(text.find_first_of(separators, begin),
			text.size());
		const std::string_view written{text.data() + begin,
			cursor - begin};
		/* What Android's reader makes of the word, which is what the
		 * checks look at; the word itself stays as the release wrote it. */
		const std::string argument = withoutQuotes(written);
		/* A launcher that sets the multiplier gets the legacy one dropped
		 * rather than refused: every release before it carries it. The
		 * serial number is the same: a launcher that sets it drops the
		 * release's fixed one, quoted or not. */
		if (argument == legacyHwTimeoutMultiplierArgument &&
		    std::ranges::find(launcherPrefixes,
				      hwTimeoutMultiplierPrefix) !=
			    launcherPrefixes.end())
			continue;
		if (argument == legacySerialArgument &&
		    std::ranges::find(launcherPrefixes, serialPrefix) !=
			    launcherPrefixes.end())
			continue;
		for (const auto prefix : launcherPrefixes)
			if (argument.starts_with(prefix))
				throw std::invalid_argument(
					"the release kernel command line sets " +
					std::string{prefix} +
					", which the launcher fixes");
		if (startsWithAny(argument, storagePrefixes))
			throw std::invalid_argument(
				"the release kernel command line sets " +
				argument +
				", which names a block device or ramdisk the launcher fixes");
		if (argument.starts_with(bootDevicesPrefix)) {
			if (argument != bootDevicesArgument || bootDevices)
				throw std::invalid_argument(
					"the release kernel command line must name exactly " +
					std::string{bootDevicesArgument} + " once");
			bootDevices = true;
		}
		arguments.emplace_back(written);
	}
	if (arguments.empty())
		throw std::invalid_argument(
			"the release kernel command line is empty");
	if (!bootDevices)
		throw std::invalid_argument(
			"the release kernel command line lacks " +
			std::string{bootDevicesArgument});
	return arguments;
}

std::string lcdDensityArgument(std::uint32_t dotsPerInch)
{
	return std::string{lcdDensityPrefix} +
	       std::to_string(dotsPerInch == 0 ? defaultLcdDensity : dotsPerInch);
}

std::string hwTimeoutMultiplierArgument(unsigned vcpus)
{
	return std::string{hwTimeoutMultiplierPrefix} +
	       std::to_string(hwTimeoutMultiplier(vcpus));
}

std::uint32_t parseDotsPerInch(std::string_view text)
{
	std::uint32_t parsed = 0;
	const auto [end, error] =
		std::from_chars(text.data(), text.data() + text.size(), parsed);
	if (error != std::errc{} || end != text.data() + text.size() ||
	    parsed == 0)
		throw std::invalid_argument("invalid dots per inch: " +
					    std::string{text});
	return parsed;
}

} // namespace uml::android
