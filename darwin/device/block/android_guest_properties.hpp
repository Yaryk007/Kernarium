// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef UML_DEVICE_BLOCK_ANDROID_GUEST_PROPERTIES_HPP
#define UML_DEVICE_BLOCK_ANDROID_GUEST_PROPERTIES_HPP

#include "android_boot.hpp"

#include <cstddef>
#include <filesystem>
#include <string_view>

namespace uml::android {

/* Property values a launch gives an Android guest over the image's own, such
 * as the name of the device it runs on. First-stage init reads them from
 * `/adb_debug.prop` of the ramdisk when `/force_debuggable` is there and the
 * device is unlocked, which every release's command line says it is; init
 * loads that file last, so its values win. The release's files and its
 * security policy are unchanged, and nothing becomes debuggable. */
inline constexpr std::size_t maximumGuestPropertiesLength = 1024;

/* Whether `text` is lines of `key=value`, each ended by a newline, whose keys
 * are ones a launch may set (the device's model and maker, the SoC's, and
 * SystemUI's own switch for the painted display cutout), each at most once,
 * and whose values are 1 through 91 characters of printable ASCII that neither
 * start nor end with a space. */
[[nodiscard]] bool validGuestProperties(std::string_view text) noexcept;

/* `boot` with the properties in its ramdisk: writes the whole initrd to
 * `output`, replacing a file an earlier boot left, and returns the selection
 * that boots from it. Only the release's stock ramdisk takes them; a recovery
 * or root boot, and an empty text, come back as they were. Throws
 * std::invalid_argument for text that is not valid or a path a kernel argument
 * cannot carry, and std::system_error for an I/O failure. */
[[nodiscard]] BootSelection withGuestProperties(
	const Storage &storage, const BootSelection &boot,
	std::string_view properties, const std::filesystem::path &output);

} // namespace uml::android

#endif
