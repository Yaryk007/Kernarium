// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "uml_darwin_guest.h"

#if DARWIN_UML_X86_64_GUEST

#include <cstdint>

namespace uml::memory {

/* Where a guest address space can write a frame of a backing: one page of
 * one generation's state. The state is named by an alias id (see
 * frame_accounting.hpp), not by its generation number, so a location outlives
 * neither its state nor a wrapped generation number. */
struct WriterLocation final {
	std::uint32_t alias;
	std::uint64_t page;

	friend bool operator==(
		const WriterLocation &, const WriterLocation &) = default;
};

} // namespace uml::memory

#endif
