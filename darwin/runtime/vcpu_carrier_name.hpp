// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string_view>

namespace uml::runtime {

/* The vCPU number of a carrier's thread name, uml-vcpu-N-carrier with N in
 * decimal, and -1 for any other name. A number too large to be a vCPU is
 * returned as 1001, so no digit string overflows. The kernel names its host
 * threads (kernel/common/README.md); the carriers are the only ones that run
 * guest code. */
[[nodiscard]] constexpr int vcpu_carrier_index(std::string_view name) noexcept
{
	constexpr std::string_view prefix = "uml-vcpu-";
	constexpr std::string_view suffix = "-carrier";
	if (!name.starts_with(prefix) || !name.ends_with(suffix) ||
	    name.size() <= prefix.size() + suffix.size())
		return -1;
	name.remove_prefix(prefix.size());
	name.remove_suffix(suffix.size());
	int index = 0;
	for (const char digit : name) {
		if (digit < '0' || digit > '9')
			return -1;
		if (index <= 1000)
			index = index * 10 + (digit - '0');
	}
	return index > 1000 ? 1001 : index;
}

} // namespace uml::runtime
