// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef UML_DEVICE_BLOCK_ANDROID_RELEASE_IDENTITY_HPP
#define UML_DEVICE_BLOCK_ANDROID_RELEASE_IDENTITY_HPP

#include "android_layout.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace uml::android {

/* What a release says it is, by the two files that name it: the ID and the
 * SHA-256 of release-manifest.json, and the SHA-256 the layout gives the
 * system partition (lowercase hex). An ahead-of-time blob (cpu/arm64/aot)
 * names the release it was made from the same way and runs only with it. */
struct ReleaseIdentity {
	std::string id;
	std::string manifestSha256;
	std::string systemSha256;
};

/* The identity of the release rooted at `release`, whose system partition
 * `layout` describes. Empty when the manifest cannot be read, is not a
 * regular file of at most 4 MiB, or states no valid top-level release_id, or
 * the layout has no digest for the system partition: a release without an
 * identity gets no ahead-of-time code. */
std::optional<ReleaseIdentity> readReleaseIdentity(
	const std::filesystem::path &release, const Layout &layout) noexcept;

/* The top-level string member `key` of the JSON object `text`, which may hold
 * any JSON; empty when there is none, it is not a string with no escape, it
 * repeats or the document is malformed. Exposed for its test. */
std::optional<std::string> topLevelString(std::string_view text,
					  std::string_view key) noexcept;

} // namespace uml::android

#endif
