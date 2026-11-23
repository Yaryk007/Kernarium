// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef UML_DEVICE_BLOCK_ANDROID_IDENTITY_HPP
#define UML_DEVICE_BLOCK_ANDROID_IDENTITY_HPP

#include "android_data_disk.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

/* What tells one installation of the Android guest from another: the serial
 * number the guest reads as ro.serialno and the hardware address of its eth0.
 * Both are chosen at random once for an installation and kept beside the
 * instance's data disk, in a small file the host owns, so they follow the data
 * disk through an image update, a storage resize, a snapshot restore and the
 * guest's own factory reset, and go with it when the instance is erased.
 *
 * The host's own messages never contain the values, and they are not a tracked
 * fixture: a test makes its own. What the guest itself reports (an Android bug
 * report, the kernel's echo of its command line in the console) carries them as
 * on any Android device. They reach the guest as the kernel argument
 * `androidboot.serialno=` (readKernelCommandLine drops the release's own) and
 * as the hardware address the user network gives eth0. The guest's Wi-Fi
 * address is derived from the eth0 address by the guest's own network setup
 * tool and is not stored here. */
namespace uml::android {

using MacAddress = std::array<std::uint8_t, 6>;

/* The identity file's name; it sits in the directory of the data disk. */
inline constexpr std::string_view identityFileName{"identity.json"};

/* The file that holds the identity of the installation whose data disk is
 * `dataDisk`. */
std::filesystem::path identityFile(const std::filesystem::path &dataDisk);

/* The hardware address the user network gives eth0 when nothing sets one, and
 * the fixed Wi-Fi address that earlier images gave wlan0 whatever eth0 had:
 * neither is ever generated. */
inline constexpr MacAddress defaultEthernetMac{
	0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
inline constexpr MacAddress legacyWifiMac{0x02, 0x15, 0xb2, 0x00, 0x00, 0x00};

/* The first five bytes of the addresses the user network's own router and
 * servers answer from (52:55:0a:00:02:xx for 10.0.2.xx, the gateway and the
 * name server among them): a guest that had one would be told apart from them
 * by nothing. darwin/device/network/slirp/provider.hpp keeps the same prefix
 * for the provider's own refusal. */
inline constexpr std::array<std::uint8_t, 5> routerMacPrefix{
	0x52, 0x55, 0x0a, 0x00, 0x02};

/* What a launcher resolved for one run: the serial number for the kernel
 * command line and the address for eth0. A member that is empty leaves the
 * guest with what it has without one (the release's serial, the user
 * network's default address). */
struct InstallationIdentity {
	std::optional<std::string> serial;
	std::optional<MacAddress> ethernetMac;

	bool operator==(const InstallationIdentity &) const = default;
};

/* Whether `serial` can be a guest's serial number: 6 to 20 letters and digits
 * (the form the platform's own tests require of Build.SERIAL), and not
 * "unknown", the value an application that may not read it is given. */
[[nodiscard]] bool validSerial(std::string_view serial) noexcept;

/* Whether `mac` can be eth0's address: unicast, locally administered, and none
 * of 02:00:00:00:00:00 (the platform's "no address" value), the user network's
 * default, the legacy Wi-Fi address or an address of the user network's router
 * and servers (routerMacPrefix). */
[[nodiscard]] bool validEthernetMac(const MacAddress &mac) noexcept;

/* `aa:bb:cc:dd:ee:ff`, lower case. */
[[nodiscard]] std::string formatMac(const MacAddress &mac);

/* The address in the text of `aa:bb:cc:dd:ee:ff` (either case), or empty when
 * the text is anything else. */
[[nodiscard]] std::optional<MacAddress> parseMac(std::string_view text) noexcept;

/* Fills the span with random bytes. */
using RandomFill = std::function<void(std::span<std::uint8_t>)>;

/* The system's secure random source. */
void systemRandom(std::span<std::uint8_t> bytes);

/* A new identity: a serial number of 16 digits `0-9a-f` (64 random bits) and a
 * locally administered unicast address of 46 random bits that validEthernetMac
 * accepts. */
[[nodiscard]] InstallationIdentity generateIdentity(const RandomFill &random);

/* The identity file's content, or empty when `file` does not exist. The file
 * is opened once, without following a link and without waiting for a writer,
 * and what is read is the opened file: a name that is replaced meanwhile is
 * not read through. Throws std::invalid_argument for a file that is not a
 * valid identity (a regular file of at most 4096 bytes, schema 1, exactly the
 * three members, valid values; a link, a pipe, a directory and a file cut
 * short are not) and std::system_error when it cannot be read. */
[[nodiscard]] std::optional<InstallationIdentity> readIdentityFile(
	const std::filesystem::path &file);

/* readIdentityFile's reading of a descriptor that is already open, which
 * `name` only names in messages. */
[[nodiscard]] InstallationIdentity readIdentityDescriptor(
	int descriptor, const std::filesystem::path &name);

/* Replaces `file` with `identity`, which must hold both members valid: the
 * new content is written beside it under a name of its own, flushed and
 * renamed over it. Throws std::system_error. */
void writeIdentityFile(const std::filesystem::path &file,
		       const InstallationIdentity &identity);

/* The identity file's directory holds `identity.lock`, which serializes what
 * changes an installation's identity: a start's preparation of the data disk
 * and of its identity (the macOS launcher, the Kernarium runtime), and the
 * tools' reset, restore and create. It is an exclusive flock that is waited for
 * up to `limit`, and the file is never removed or replaced. Whoever holds it
 * sees the installation as no other process changes it, so the start that
 * reads an absent identity file is the only one that makes it, and the others
 * read what it made. std::system_error (ETIMEDOUT) when the limit passes. */
inline constexpr std::string_view lockFileName{"identity.lock"};

/* The lock file of the installation whose identity file is `identity`. */
std::filesystem::path installationLockFile(
	const std::filesystem::path &identity);

class InstallationLock final {
public:
	explicit InstallationLock(
		const std::filesystem::path &identity,
		std::chrono::milliseconds limit = std::chrono::seconds{60});
	~InstallationLock();
	InstallationLock(const InstallationLock &) = delete;
	InstallationLock &operator=(const InstallationLock &) = delete;

	const std::filesystem::path &file() const noexcept { return file_; }

private:
	std::filesystem::path file_;
	int descriptor_ = -1;
};

/* The installation's lock, or empty when it cannot be taken at all (the
 * directory is read-only or missing): the start then takes values it does not
 * keep, as it does for a file it cannot write. Throws std::system_error
 * (ETIMEDOUT) when another process holds it past `limit`, which refuses the
 * start. */
[[nodiscard]] std::optional<InstallationLock> acquireInstallationLock(
	const std::filesystem::path &identity,
	std::chrono::milliseconds limit = std::chrono::seconds{60});

/* What a launcher states about the identity. A value in `stated` is used as
 * given and nothing is read or written; the other member then stays unset. */
struct IdentityRequest {
	/* The identity file, or empty for none: the launcher then keeps the
	 * guest's fixed values unless a value is stated. */
	std::filesystem::path file;
	/* The data disk was created by this start, so no earlier identity
	 * belongs to it. */
	bool diskCreated = false;
	InstallationIdentity stated;
	RandomFill random = systemRandom;
	/* The lock of `file`'s installation, which the caller took before it
	 * prepared the data disk and holds until the identity is resolved. A
	 * request with a file and no lock (acquireInstallationLock gave none)
	 * reads and writes nothing and takes values it does not keep; a lock
	 * that is another installation's throws std::logic_error. */
	const InstallationLock *lock = nullptr;
};

enum class IdentityOutcome {
	/* Nothing to resolve: no file and no stated value. */
	fixed,
	/* The stated values, used as they are. */
	stated,
	/* The file's identity. */
	kept,
	/* A new identity, written: no file, or a new data disk. */
	created,
	/* A new identity, written over a file that was not a valid one. */
	replaced,
	/* A new identity that could not be written, used for this start only. */
	unsaved,
};

struct ResolvedIdentity {
	InstallationIdentity identity;
	IdentityOutcome outcome = IdentityOutcome::fixed;
};

/* The identity this start gives the guest. A stated value wins and touches no
 * file. Otherwise the file decides: its identity is kept, and a new one is
 * written when the file is absent or invalid, or the data disk is new. Under
 * the installation's lock the first start to find no identity makes it and
 * every other start reads that one. A file that cannot be written, or a lock
 * that was not taken, never fails the start: the new identity is used and the
 * outcome says so. */
[[nodiscard]] ResolvedIdentity resolveIdentity(const IdentityRequest &request);

/* A start's preparation of one installation, which is what both launchers do
 * before the guest's disks are attached: the installation is locked (unless the
 * request states values and has no file), the data disk is created, grown or
 * left (prepareDataDisk), and the identity is resolved for it, with `diskCreated`
 * and `lock` filled in. The lock is released when the call returns. Another
 * start of the installation, or the tools' reset, waits for it up to `lockLimit`;
 * a start that cannot lock the installation fails with std::system_error
 * (ETIMEDOUT) before it touches the disk. */
struct PreparedInstallation {
	PreparedDataDisk disk;
	ResolvedIdentity identity;
};

[[nodiscard]] PreparedInstallation prepareInstallation(
	const Layout &layout, const std::filesystem::path &dataDisk,
	std::uint32_t gib, IdentityRequest request,
	std::chrono::milliseconds lockLimit = std::chrono::seconds{60});

/* The kernel argument that gives the guest its serial number. Android init
 * imports it as ro.boot.serialno and ro.serialno, so a launcher that resolved
 * a serial sets it, and a release's command line may carry only the legacy
 * argument below, which readKernelCommandLine() then drops. */
inline constexpr std::string_view serialPrefix{"androidboot.serialno="};

/* The argument every release published before the launchers set the serial
 * carries. */
inline constexpr std::string_view legacySerialArgument{
	"androidboot.serialno=umlandroid17"};

/* `androidboot.serialno=` followed by `serial`. */
[[nodiscard]] std::string serialArgument(std::string_view serial);

} // namespace uml::android

#endif
