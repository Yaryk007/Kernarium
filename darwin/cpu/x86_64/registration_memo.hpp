// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/x86_64/code_page_registry.hpp"
#include "memory/guest_address_space.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace uml::cpu::x86_64 {

/* What an executor remembers of the frame registrations that settled, so that
 * it does not ask the address space again for a page whose holder it has
 * already registered.
 *
 * A registration of a page that is already held adds nothing and, while no
 * holder has been added to the frame since, owes nothing: debts arise only at
 * holder additions, which move the frame's change count. An entry therefore
 * stands for "this page is held, and nothing is owed" for as long as
 *   - the executor is still in the epoch the entry was made in: any mapping
 *     change of the generation, and any move to another generation, ends the
 *     epoch (`clear`), because a holder leaves its frame only with the page's
 *     mapping, and the execution gate keeps a mapping from changing under an
 *     executor that is inside the address space;
 *   - the generation's flush serial is the one the entry was made under,
 *     because a flush removes the generation's holders; and
 *   - the frame's change count is the one the registration left
 *     (`GuestAddressSpace::frame_hold_unchanged`).
 * Only a registration that settled is remembered: an unsettled one still owes
 * its debt, and a failed or refused one holds nothing. A registration the
 * address space answered globally (the frame's writers are not known or too
 * many to name) names no frame, and the frame's change count moves when its
 * writers become unknown, so no entry stands in for that answer.
 *
 * One executor's table, touched by its own thread only. */
class RegistrationMemo final {
public:
	/* Ends the epoch: every entry made so far is forgotten. */
	void clear() noexcept { ++epoch_; }

	/* Whether `page`'s last registration still stands, under `serial`. */
	[[nodiscard]] bool holds(::uml::memory::GuestAddressSpace &memory,
		std::uint64_t page, std::uint64_t serial) const noexcept
	{
		const auto &entry = entries_[slot_of(page)];
		if (entry.epoch != epoch_ || entry.page != page ||
		    entry.serial != serial)
			return false;
		::uml::memory::FrameHold hold;
		hold.record = entry.record;
		hold.alias_changes = entry.alias_changes;
		return memory.frame_hold_unchanged(hold);
	}

	/* Remembers a registration of `page` made under `serial`. */
	void remember(std::uint64_t page, std::uint64_t serial,
		const FrameRegistrationOutcome &outcome) noexcept
	{
		if (outcome.status != FrameRegistration::settled ||
		    outcome.record == nullptr)
			return;
		entries_[slot_of(page)] = {epoch_, page, serial, outcome.record,
			outcome.alias_changes};
	}

private:
	static constexpr std::size_t slot_count = 256;
	struct Entry final {
		std::uint64_t epoch{};
		std::uint64_t page{};
		std::uint64_t serial{};
		const void *record{};
		std::uint64_t alias_changes{};
	};
	[[nodiscard]] static std::size_t slot_of(std::uint64_t page) noexcept
	{
		return static_cast<std::size_t>(page >> 12U) & (slot_count - 1U);
	}

	/* Entries carry the epoch they were made in, and the epoch starts at 1,
	 * so a fresh table remembers nothing. */
	std::uint64_t epoch_{1};
	std::array<Entry, slot_count> entries_{};
};

} // namespace uml::cpu::x86_64
