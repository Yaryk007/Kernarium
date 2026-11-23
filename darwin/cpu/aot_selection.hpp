/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace uml::cpu {

/* Whether a run enters ahead-of-time code (cpu/arm64/aot): `automatic` is on
 * in a build that has the component, for the release it was made from, unless
 * DARWIN_UML_AOT=0; `off` and `on` are a launch's own word (--aot=), which the
 * environment does not override. */
enum class AotSelection : unsigned {
	automatic,
	off,
	on,
};

/* The release a launch boots, as the release's own files state it: its ID,
 * the SHA-256 of its manifest and the SHA-256 of its system partition. The
 * component's blob names the release it was made from the same way, and
 * runs only with that one. */
struct AotRelease final {
	std::string_view id;
	std::array<std::uint8_t, 32> manifest_sha256{};
	std::array<std::uint8_t, 32> system_sha256{};
};

} // namespace uml::cpu
