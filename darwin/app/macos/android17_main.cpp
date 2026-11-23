// SPDX-License-Identifier: GPL-2.0-or-later

#include "launcher.hpp"
#include "block/android_boot.hpp"
#include "block/android_identity.hpp"
#include "camera/camera_layout.hpp"
#include "options.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

/* The guest RAM when the launch states no memory limit; with one, the shared
 * default for that limit (main.cpp). */
constexpr std::string_view android_default_guest_memory{"mem=4G"};
constexpr std::string_view release_option{"--android-release="};
constexpr std::string_view data_disk_option{"--android-data-disk="};
constexpr std::string_view data_gib_option{"--android-data-gib="};
constexpr std::string_view renamed_disk_option{"--android-disk="};
constexpr std::string_view boot_mode_option{"--android-boot-mode="};
/* A bare flag: the normal boot's ramdisk is the release's own Magisk-patched
 * one, which only a release that carries root has. */
constexpr std::string_view root_option{"--android-root"};
constexpr std::string_view identity_option{"--android-identity="};
constexpr std::string_view serial_option{"--android-serialno="};
constexpr std::string_view net_mac_option{"--net-mac="};
constexpr std::string_view dpi_option{"--gpu-dpi="};
constexpr std::string_view adb_authorization_prefix{
	"androidboot.kernarium.adb_auth="};
/* The hardware SKU that tells the guest what the host has (the iOS runtime's
 * KernariumHostFeatures.hpp states it from the boot request). A Mac has no
 * Taptic Engine, so this launcher states none itself; a run that wants the
 * guest's vibrator for a CTS, VTS or protocol check passes the argument, which
 * is forwarded as given. A release's own command line may not carry it, nor
 * the camera layout (the vendor SKU), which a run passes itself or has
 * composed (--camera-layout=auto). */
constexpr std::string_view hardware_sku_prefix{
	"androidboot.product.hardware.sku="};
/* Makes the guest list a device-orientation sensor, which Android's window
 * manager then follows in place of the accelerometer (the iOS runtime's
 * KernariumHostFeatures.hpp states it too). Only a run that feeds the sensor
 * passes it, as the orientation check does; a release's own command line may
 * not carry it. */
constexpr std::string_view orientation_prefix{
	"androidboot.kernarium.orientation="};
/* Makes the guest list a magnetometer and declare the compass feature (the iOS
 * runtime's KernariumHostFeatures.hpp states it too). Only a run that feeds
 * the field passes it; a release's own command line may not carry it. */
constexpr std::string_view compass_prefix{
	"androidboot.kernarium.compass="};
/* Tells the guest its display is a 120 Hz one (the iOS runtime's
 * KernariumHostFeatures.hpp states it too). This launcher's display keeps its
 * 60 Hz mode and passes none; a release's own command line may not carry it. */
constexpr std::string_view high_refresh_prefix{
	"androidboot.kernarium.high_refresh="};

struct AndroidOptions {
	std::filesystem::path release;
	std::filesystem::path data_disk;
	/* 0 keeps an existing data disk's capacity and gives a new one the
	 * release's default. */
	std::uint32_t data_gib = 0;
	uml::android::BootMode boot_mode = uml::android::BootMode::automatic;
	bool boot_mode_set = false;
	/* --android-root: boot the release's root ramdisk. */
	uml::android::RootMode root = uml::android::RootMode::off;
	/* The EDID density the GPU device advertises; 0 advertises none. */
	std::uint32_t dots_per_inch = 0;
	/* --android-identity=: the file that holds this installation's serial
	 * number and eth0 address, made when absent. Empty keeps the guest's
	 * fixed values. */
	std::filesystem::path identity_file;
	/* --android-serialno= and --net-mac=: values a run states for itself, which
	 * exclude the identity file. */
	std::optional<std::string> serial;
	bool net_mac_stated = false;
	std::vector<std::string> forwarded;
};

uml::android::BootMode parse_boot_mode(std::string_view value)
{
	if (value == "auto")
		return uml::android::BootMode::automatic;
	if (value == "normal")
		return uml::android::BootMode::normal;
	if (value == "recovery")
		return uml::android::BootMode::recovery;
	throw std::invalid_argument("Android boot mode must be auto, normal, or recovery");
}

std::uint32_t parse_data_gib(std::string_view text)
{
	std::uint32_t gib = 0;
	const auto [end, error] =
		std::from_chars(text.data(), text.data() + text.size(), gib);
	if (error != std::errc{} || end != text.data() + text.size() || gib == 0)
		throw std::invalid_argument(
			"--android-data-gib must be a positive number of GiB");
	return gib;
}

std::string environment_value(const char *name)
{
	const char *value = std::getenv(name);
	return value == nullptr ? std::string{} : std::string{value};
}

AndroidOptions parse_options(int argc, char *const argv[])
{
	AndroidOptions options;
	options.forwarded.reserve(static_cast<std::size_t>(argc) + 32U);
	options.forwarded.emplace_back(argv[0] == nullptr ? "uml-darwin-android17" :
							 argv[0]);
	bool has_gpu = false;
	bool has_gpu_display = false;
	bool has_audio = false;
	bool has_battery = false;
	bool has_gnss = false;
	bool has_video = false;
	bool has_video_encoder = false;
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument =
			argv[index] == nullptr ? std::string_view{} : argv[index];
		/* UML joins its arguments with spaces, so one that holds several
		 * words, or quotes, carries each of them to Android. */
		if (uml::android::carriesPrefix(argument, adb_authorization_prefix))
			throw std::invalid_argument(
				"macOS fixes Android ADB authorization at boot");
		if (uml::android::carriesPrefix(argument,
						uml::android::lcdDensityPrefix))
			throw std::invalid_argument(
				"the Android density follows --gpu-dpi");
		if (uml::android::carriesPrefix(
			    argument, uml::android::hwTimeoutMultiplierPrefix))
			throw std::invalid_argument(
				"the Android timeout multiplier follows ncpus=");
		if (uml::android::carriesPrefix(argument,
						uml::android::serialPrefix))
			throw std::invalid_argument(
				"the Android serial number follows --android-serialno or "
				"--android-identity");
		if (argument.starts_with(identity_option)) {
			if (!options.identity_file.empty())
				throw std::invalid_argument(
					"--android-identity may be specified only once");
			options.identity_file = argument.substr(identity_option.size());
			if (!options.identity_file.is_absolute())
				throw std::invalid_argument(
					"--android-identity must be an absolute path");
			continue;
		}
		if (argument.starts_with(serial_option)) {
			if (options.serial)
				throw std::invalid_argument(
					"--android-serialno may be specified only once");
			options.serial = std::string{argument.substr(serial_option.size())};
			if (!uml::android::validSerial(*options.serial))
				throw std::invalid_argument(
					"--android-serialno must be 6 to 20 letters and digits");
			continue;
		}
		options.net_mac_stated |= argument.starts_with(net_mac_option);
		if (argument.starts_with(dpi_option))
			options.dots_per_inch = uml::android::parseDotsPerInch(
				argument.substr(dpi_option.size()));
		if (argument.starts_with(release_option)) {
			if (!options.release.empty())
				throw std::invalid_argument(
					"--android-release may be specified only once");
			options.release = argument.substr(release_option.size());
			if (options.release.empty())
				throw std::invalid_argument(
					"--android-release requires a path");
			continue;
		}
		if (argument.starts_with(renamed_disk_option))
			throw std::invalid_argument(
				"--android-disk is now --android-data-disk, the instance's "
				"writable disk; the release carries the read-only system disk");
		if (argument.starts_with(data_disk_option)) {
			if (!options.data_disk.empty())
				throw std::invalid_argument(
					"--android-data-disk may be specified only once");
			options.data_disk = argument.substr(data_disk_option.size());
			if (options.data_disk.empty())
				throw std::invalid_argument(
					"--android-data-disk requires a path");
			continue;
		}
		if (argument.starts_with(data_gib_option)) {
			if (options.data_gib != 0)
				throw std::invalid_argument(
					"--android-data-gib may be specified only once");
			options.data_gib = parse_data_gib(
				argument.substr(data_gib_option.size()));
			continue;
		}
		if (argument == root_option) {
			if (options.root == uml::android::RootMode::on)
				throw std::invalid_argument(
					"--android-root may be specified only once");
			options.root = uml::android::RootMode::on;
			continue;
		}
		if (argument.starts_with(boot_mode_option)) {
			if (options.boot_mode_set)
				throw std::invalid_argument(
					"--android-boot-mode may be specified only once");
			options.boot_mode = parse_boot_mode(
				argument.substr(boot_mode_option.size()));
			options.boot_mode_set = true;
			continue;
		}
		has_gpu |= argument == "--gpu";
		has_gpu_display |= argument.starts_with("--gpu-display=");
		has_audio |= argument == "--audio" ||
			     argument.starts_with("--audio-output=") ||
			     argument.starts_with("--audio-input=");
		has_battery |= argument == "--battery";
		has_gnss |= argument == "--gnss" || argument.starts_with("--location=");
		has_video |= argument == "--video";
		has_video_encoder |= argument == "--video-encoder";
		options.forwarded.emplace_back(argument);
	}
	if (options.release.empty())
		options.release = environment_value("UML_ANDROID17_RELEASE");
	if (options.data_disk.empty())
		options.data_disk = environment_value("UML_ANDROID17_DATA_DISK");
	if (!environment_value("UML_ANDROID17_DISK").empty())
		throw std::invalid_argument(
			"UML_ANDROID17_DISK is now UML_ANDROID17_DATA_DISK, the "
			"instance's writable disk");
	if (!options.boot_mode_set) {
		const auto environment_mode = environment_value("UML_ANDROID17_BOOT_MODE");
		if (!environment_mode.empty())
			options.boot_mode = parse_boot_mode(environment_mode);
	}
	if (!options.identity_file.empty() &&
	    (options.serial || options.net_mac_stated))
		throw std::invalid_argument(
			"--android-identity excludes --android-serialno and --net-mac: "
			"a run states its values or reads them from the file");
	if (!has_gpu)
		options.forwarded.emplace_back("--gpu");
	if (!has_gpu_display)
		options.forwarded.emplace_back("--gpu-display=native");
	if (!has_audio)
		options.forwarded.emplace_back("--audio");
	if (!has_battery)
		options.forwarded.emplace_back("--battery");
	if (!has_gnss)
		options.forwarded.emplace_back("--gnss");
	if (!has_video)
		options.forwarded.emplace_back("--video");
	if (!has_video_encoder)
		options.forwarded.emplace_back("--video-encoder");
	return options;
}

std::filesystem::path validated_data_disk(
	const std::filesystem::path &candidate)
{
	if (!candidate.is_absolute())
		throw std::invalid_argument("--android-data-disk must be absolute");
	return candidate;
}

std::filesystem::path validated_release_root(
	const std::filesystem::path &candidate)
{
	if (!candidate.is_absolute())
		throw std::invalid_argument("--android-release must be absolute");
	struct stat candidate_details {};
	if (lstat(candidate.c_str(), &candidate_details) != 0 ||
	    !S_ISDIR(candidate_details.st_mode))
		throw std::invalid_argument(
			"Android release must be a real local directory");
	std::error_code error;
	auto release = std::filesystem::canonical(candidate, error);
	if (error)
		throw std::invalid_argument("Android release cannot be resolved");
	return release;
}

void print_usage()
{
	std::fputs(
		"Android 17 options:\n"
			"  --android-release=/absolute/release (system disk, layout, command line)\n"
			"  --android-data-disk=/absolute/data.img (the instance's writable disk;\n"
			"      created when absent)\n"
			"  --android-data-gib=N (capacity of a new data disk, or growth of an\n"
			"      existing one; a larger disk is kept, never shrunk)\n"
			"  --android-boot-mode=auto|normal|recovery\n"
			"  --android-root (the normal boot's ramdisk is the release's own with\n"
			"      Magisk patched in, which only a release that carries root has;\n"
			"      recovery keeps its own)\n"
			"  --android-identity=/absolute/identity.json (this installation's serial\n"
			"      number and eth0 address, made when absent or when the data disk is\n"
			"      new; without it, or --android-serialno and --net-mac, the guest\n"
			"      keeps its fixed values)\n"
			"  --android-serialno=S (6 to 20 letters and digits, for a run that states\n"
			"      its own; excludes --android-identity)\n"
			"  --net-mac=aa:bb:cc:dd:ee:ff (eth0's unicast address; excludes\n"
			"      --android-identity)\n"
			"  --gpu-dpi=N (also the guest density; 213 without it)\n"
			"  ncpus=N (also the guest's timeout multiplier: 3 from six vCPUs,\n"
			"      5 below)\n"
			"  --virtio-console-socket=/absolute/console.sock\n"
			"  --aot=auto|on|off (ahead-of-time code for the image's libraries, "
			"which a\n"
			"      build that carries it runs unless DARWIN_UML_AOT=0 or "
			"--aot=off)\n"
			"The same values may be supplied through UML_ANDROID17_RELEASE, "
			"UML_ANDROID17_DATA_DISK, "
			"and UML_ANDROID17_BOOT_MODE.\n"
			"The guest has exactly the release's system disk and this data disk; "
			"ubd, initrd, root and boot-device arguments are refused.\n"
			"The boot mode picks the first boot's ramdisk. A guest that "
			"restarts itself boots again in the same process, and each "
			"restart picks the ramdisk in auto mode; --android-root applies "
			"to every normal boot.\n",
		stderr);
}

/* The kernel arguments that attach the guest's two disks and give it `boot`'s
 * ramdisk, appended to the host's arguments. */
std::vector<std::string> boot_command_line(
	const std::vector<std::string> &host_arguments,
	const uml::android::Storage &storage, const uml::android::BootSelection &boot,
	std::uint32_t dots_per_inch, const std::optional<std::string> &serial)
{
	auto forwarded = host_arguments;
	for (auto &argument : storage.kernelArguments(boot))
		forwarded.push_back(std::move(argument));
	/* The release the guest boots, as its files state it: the ahead-of-time
	 * component runs only with the release it was made from. A release that
	 * states none gets no such code. */
	if (const auto identity = storage.releaseIdentity())
		if (auto argument = uml::emulator::aot_release_argument(
			    identity->id, identity->manifestSha256,
			    identity->systemSha256);
		    !argument.empty())
			forwarded.push_back(std::move(argument));
	std::vector<std::string_view> launcher_prefixes{
		adb_authorization_prefix,
		hardware_sku_prefix,
		uml::camera::layoutArgumentPrefix,
		orientation_prefix,
		compass_prefix,
		high_refresh_prefix,
		uml::android::lcdDensityPrefix,
		uml::android::hwTimeoutMultiplierPrefix};
	/* A run that has a serial number of its own drops the release's fixed
	 * one; a run with none leaves the release's command line as it is. */
	if (serial)
		launcher_prefixes.push_back(uml::android::serialPrefix);
	for (auto &argument : uml::android::readKernelCommandLine(
		     storage.release() / uml::android::releaseCommandLine,
		     launcher_prefixes))
		forwarded.push_back(std::move(argument));
	/* Android init imports this as ro.boot.lcd_density, which the vendor
	 * init script publishes as ro.sf.lcd_density: a guest without it has an
	 * empty property. */
	forwarded.emplace_back(uml::android::lcdDensityArgument(dots_per_inch));
	/* The framework scales its ANR, watchdog and input timeouts by this: the
	 * vendor init script publishes it as ro.hw_timeout_multiplier. It follows
	 * the vCPU count the launch boots with, which main.cpp defaults when the
	 * host states none. */
	forwarded.emplace_back(uml::android::hwTimeoutMultiplierArgument(
		uml::emulator::launch_guest_cpu_count(host_arguments)));
	forwarded.emplace_back(std::string{adb_authorization_prefix} + "0");
	/* Android init imports this as ro.boot.serialno and ro.serialno. */
	if (serial)
		forwarded.emplace_back(uml::android::serialArgument(*serial));
	std::fprintf(
		stderr,
			"Android rootfs %s, %s boot, system disk %s, data disk %s\n",
			storage.release().c_str(),
			boot.mode == uml::android::BootMode::recovery
				? "recovery"
				: boot.ramdiskFile.empty() ? "normal" : "normal (root)",
			storage.systemDisk().c_str(), storage.dataDisk().c_str());
	return forwarded;
}

/* What the identity file did, without the values: they are the installation's
 * own and this host message carries neither. */
void announce_identity(uml::android::IdentityOutcome outcome)
{
	switch (outcome) {
	case uml::android::IdentityOutcome::created:
		std::fputs("Android: made the installation's identity\n", stderr);
		break;
	case uml::android::IdentityOutcome::replaced:
		std::fputs("Android: replaced an identity file that was not valid\n",
			   stderr);
		break;
	case uml::android::IdentityOutcome::unsaved:
		std::fputs("Android: the identity file cannot be kept; this start "
			   "uses values it will not keep\n",
			   stderr);
		break;
	case uml::android::IdentityOutcome::fixed:
	case uml::android::IdentityOutcome::stated:
	case uml::android::IdentityOutcome::kept:
		break;
	}
}

void announce_data_disk(const uml::android::PreparedDataDisk &prepared,
			std::uint32_t requested_gib)
{
	switch (prepared.change) {
	case uml::android::DataDiskChange::created:
		std::fprintf(stderr, "Android: created a %u GiB data disk\n",
			     prepared.capacityGiB);
		break;
	case uml::android::DataDiskChange::grown:
		std::fprintf(stderr, "Android: grew the data disk to %u GiB\n",
			     prepared.capacityGiB);
		break;
	case uml::android::DataDiskChange::none:
		if (requested_gib != 0 && requested_gib < prepared.capacityGiB)
			std::fprintf(stderr,
				     "Android: the data disk is %u GiB, above the "
				     "%u GiB requested, and is kept\n",
				     prepared.capacityGiB, requested_gib);
		break;
	}
}

/* There is no bootloader to take the request `reboot bootloader` leaves, and
 * left in place it stops init from writing the next `reboot recovery`'s. */
void say_bootloader_consumed()
{
	std::fputs("Android: the guest asked for the bootloader; there is none, "
		   "so the request is cleared and the guest boots normally\n",
		   stderr);
}

} // namespace

int main(int argc, char *argv[], char *envp[])
{
	try {
		const bool information_only = uml::app::informational(argc, argv);
		if (uml::app::has_argument(argc, argv, "--help"))
			print_usage();
		auto options = parse_options(argc, argv);
		uml::app::LaunchConfiguration configuration{
			.rootfs_path = nullptr,
			.default_guest_memory = android_default_guest_memory,
			.top_byte_ignore = true,
			.input_mode = uml::app::DisplayInput::touchscreen,
			.console_reports_boot_completion = true,
			.states_aot_release = true,
		};
		if (!information_only) {
			if (options.release.empty() || options.data_disk.empty())
				throw std::invalid_argument(
					"Android launch requires release and data disk paths");
			const auto release = validated_release_root(options.release);
			const auto data_disk = validated_data_disk(options.data_disk);
			/* The host's own arguments follow argv[0]; none may attach a
			 * disk. */
			uml::android::rejectStorageArguments(
				std::span{options.forwarded}.subspan(1));
			/* The installation is locked from the data disk's preparation
			 * to the identity's resolution, so that a start that creates
			 * the disk and a start that finds it do not interleave, and
			 * the tools' reset waits for both. A new data disk is a new
			 * installation, whatever file lies beside it. */
			const auto installation = uml::android::prepareInstallation(
				uml::android::readLayout(
					release / uml::android::releaseLayout),
				data_disk, options.data_gib,
				{.file = options.identity_file,
				 .stated = {options.serial, std::nullopt}});
			const auto &prepared_disk = installation.disk;
			const auto &resolved = installation.identity;
			announce_data_disk(prepared_disk, options.data_gib);
			announce_identity(resolved.outcome);
			if (resolved.identity.ethernetMac)
				options.forwarded.emplace_back(
					std::string{net_mac_option} +
					uml::android::formatMac(
						*resolved.identity.ethernetMac));
			const auto serial = resolved.identity.serial;
			const uml::android::Storage storage{release, data_disk};
			if (storage.consumeBootloaderCommand())
				say_bootloader_consumed();
			const auto boot =
				storage.selectBoot(options.boot_mode, options.root);
			const auto host_arguments = options.forwarded;
			options.forwarded =
				boot_command_line(
					host_arguments, storage, boot, options.dots_per_inch,
					serial);
			/* The mode the user gave picks the first boot's ramdisk only: a
			 * guest that restarts has said what it wants in the disk's
			 * bootloader message. */
			configuration.restart_command_line =
				[host_arguments, storage, serial,
				 dots_per_inch = options.dots_per_inch,
				 root = options.root] {
					const auto next = storage.selectRestartBoot(root);
					if (next.bootloaderConsumed)
						say_bootloader_consumed();
					return boot_command_line(
						host_arguments, storage, next.boot, dots_per_inch,
						serial);
				};
		}
		return uml::app::runEmbeddedKernel(
			options.forwarded, envp, configuration);
	} catch (const std::exception &error) {
		std::fprintf(stderr, "Unable to configure Android guest: %s\n",
			     error.what());
		return 1;
	}
}
