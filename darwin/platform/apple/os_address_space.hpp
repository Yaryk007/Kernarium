// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <mach/mach.h>

#include <algorithm>
#include <cstdint>
#include <optional>

namespace uml::platform {

/* One mapped range of a process's address space, [begin, end). */
struct AddressRange final {
	std::uint64_t begin;
	std::uint64_t end;
};

/* The widest run of address space from `floor` up to `ceiling` that no mapped
 * range holds. `next_range(address)` answers the first mapped range that ends
 * after `address`, in the order of address, or nothing where none lies at or
 * above it; a range may begin before `address`. Zero where every byte is
 * mapped. */
template<class NextRange>
[[nodiscard]] constexpr std::uint64_t largest_free_run(std::uint64_t floor,
	std::uint64_t ceiling, NextRange &&next_range) noexcept
{
	std::uint64_t cursor = floor;
	std::uint64_t widest = 0;
	while (cursor < ceiling) {
		const std::optional<AddressRange> range = next_range(cursor);
		if (!range || range->begin >= ceiling) {
			widest = std::max(widest, ceiling - cursor);
			break;
		}
		if (range->end <= cursor)
			break;
		if (range->begin > cursor)
			widest = std::max(widest, range->begin - cursor);
		cursor = range->end;
	}
	return widest;
}

/* The highest address from which a process has none of the small span the
 * system gives an application signed without Extended Virtual Addressing: 512
 * GiB. Such a process can map only up to a high address of 454 GiB, above a 384
 * GiB guard region, one run of about 6 GiB; with the entitlement the system
 * gives it up to 512 GiB, and a process that is not an iOS application on a
 * device (the Simulator's is a macOS process) has far more. */
inline constexpr std::uint64_t unlimited_address_space_bytes =
	std::uint64_t{512} << 30;

/* Whether the process whose highest mappable address is `max_address` (task_vm_info's) is
 * limited to the small span: only there is the room for a native kernel
 * window a fact to measure and to bound the guest's memory by. Every other
 * process, and one that cannot say (0), has the address space it always had. */
[[nodiscard]] constexpr bool address_space_is_limited(
	std::uint64_t max_address) noexcept
{
	return max_address != 0 && max_address < unlimited_address_space_bytes;
}

/* The widest run of address space from `floor` up to `ceiling` that no range
 * of this process's own map holds, walked with vm_region. Nothing where the
 * system does not answer. Nothing is reserved or changed. */
[[nodiscard]] inline std::optional<std::uint64_t> os_largest_free_address_run(
	std::uint64_t floor, std::uint64_t ceiling) noexcept
{
	bool answered = true;
	const auto widest = largest_free_run(floor, ceiling,
		[&answered](std::uint64_t address) -> std::optional<AddressRange> {
			vm_address_t region = static_cast<vm_address_t>(address);
			vm_size_t size = 0;
			vm_region_basic_info_data_64_t information{};
			mach_msg_type_number_t information_count =
				VM_REGION_BASIC_INFO_COUNT_64;
			mach_port_t object = MACH_PORT_NULL;
			const kern_return_t result = vm_region_64(mach_task_self(),
				&region, &size, VM_REGION_BASIC_INFO_64,
				reinterpret_cast<vm_region_info_t>(&information),
				&information_count, &object);
			if (object != MACH_PORT_NULL)
				(void)mach_port_deallocate(mach_task_self(), object);
			if (result == KERN_INVALID_ADDRESS)
				return std::nullopt;
			if (result != KERN_SUCCESS || size == 0) {
				answered = false;
				return std::nullopt;
			}
			return AddressRange{region, std::uint64_t{region} + size};
		});
	if (!answered)
		return std::nullopt;
	return widest;
}

/* The highest address this process may map (task_vm_info's max_address), or
 * zero where the system does not answer. */
[[nodiscard]] inline std::uint64_t os_max_address() noexcept
{
	task_vm_info_data_t info{};
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO,
		    reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS ||
	    count < TASK_VM_INFO_COUNT)
		return 0;
	return info.max_address;
}

/* What the system gives this process, where the system limits it
 * (address_space_is_limited): the widest run of address space it has free from
 * `floor` up to the highest address it may map, which is the largest
 * reservation a native kernel window can still be given there, since
 * mmap(NULL, n) succeeds for the n that fit in some run. The system sets that
 * highest address by the process's signing, so the figure is a fact of the
 * process, not a constant of the device. Zero is a measurement: the process has
 * not the room for the smallest guest's window. Nothing where the system does
 * not answer and for every process that is not limited: the Simulator, a macOS
 * process, an application with Extended Virtual Addressing. Their address space
 * is theirs to use as it was before this was measured.
 *
 * `max_address` names where the process's highest address comes from, the
 * system's own answer by default: a test states one, since no test process is
 * limited by the system.
 *
 * Header-only and free of the rest of the platform, so the application, which
 * does not link the runtime, reads the figure the way the runtime does. */
[[nodiscard]] inline std::optional<std::uint64_t> os_limited_free_address_run(
	std::uint64_t floor,
	std::uint64_t (*max_address)() noexcept = os_max_address) noexcept
{
	const std::uint64_t highest = max_address();
	if (!address_space_is_limited(highest))
		return std::nullopt;
	return os_largest_free_address_run(floor, highest);
}

} // namespace uml::platform
