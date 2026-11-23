// SPDX-License-Identifier: GPL-2.0-or-later
#include "static_guest_memory.hpp"

#include "memory/mmu/code_holders.hpp"
#include "memory/mmu/execution_gate.hpp"
#include "static_guest_memory_testing.hpp"
#include "uml_darwin_static_mmu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace uml::memory {
namespace {

/* A holder word's address in a TLB entry's code-page slot carries bit 0, which
 * no code-page record's address does. */
constexpr std::uintptr_t holder_tag = 1;

[[nodiscard]] bool holds_holder(const void *slot) noexcept
{
	return (reinterpret_cast<std::uintptr_t>(slot) & holder_tag) != 0;
}

[[nodiscard]] void *tag_holder(code_holders::Word *word) noexcept
{
	return reinterpret_cast<void *>(
		reinterpret_cast<std::uintptr_t>(word) | holder_tag);
}

[[nodiscard]] code_holders::Word *untag_holder(void *slot) noexcept
{
	return reinterpret_cast<code_holders::Word *>(
		reinterpret_cast<std::uintptr_t>(slot) & ~holder_tag);
}

#if UML_STATIC_GUEST_MEMORY_TESTING
std::atomic<testing::FetchObserver> &fetch_observer()
{
	static std::atomic<testing::FetchObserver> observer{};
	return observer;
}
#endif

} // namespace

#if UML_STATIC_GUEST_MEMORY_TESTING
void testing::set_fetch_observer(FetchObserver observer) noexcept
{
	fetch_observer().store(observer, std::memory_order_release);
}
#endif

void StaticGuestMemory::set_generation(unsigned long generation) noexcept
{
	prepare(generation, darwin_uml_static_vm_changes(generation));
}

void StaticGuestMemory::prepare(
	unsigned long generation, std::uint64_t changes) noexcept
{
	generation_ = generation;
	changes_ = changes;
	fault_recorded_ = false;
}

bool StaticGuestMemory::read(
	std::uint64_t address, std::span<std::byte> destination)
{
	return copy_from_guest(address, destination, ::uml::memory::MemoryAccess::read);
}

bool StaticGuestMemory::fetch(
	std::uint64_t address, std::span<std::byte> destination)
{
	return copy_from_guest(address, destination, ::uml::memory::MemoryAccess::execute);
}

bool StaticGuestMemory::write(
	std::uint64_t address, std::span<const std::byte> source)
{
	/* Validate every page before changing physical memory.  This keeps a
	 * cross-page ARM store restartable when its second page faults.
	 */
	std::array<std::byte *, 2> destinations{};
	std::array<std::size_t, 2> lengths{};
	std::size_t count = 0;
	std::size_t completed = 0;
	while (completed < source.size()) {
		if (count == destinations.size())
			return record_fault(
				address + completed, ::uml::memory::MemoryAccess::write);
		const auto current = address + completed;
		const auto length = std::min<std::size_t>(
			source.size() - completed,
			page_size - (current & (page_size - 1)));
		auto *destination = translate(current, ::uml::memory::MemoryAccess::write);
		if (destination == nullptr)
			return false;
		destinations[count] = destination;
		lengths[count] = length;
		++count;
		completed += length;
	}
	completed = 0;
	for (std::size_t index = 0; index < count; ++index) {
		std::memcpy(
			destinations[index], source.data() + completed, lengths[index]);
		completed += lengths[index];
	}
	return true;
}

std::byte *StaticGuestMemory::atomic_pointer(
	std::uint64_t address, std::size_t size, ::uml::memory::MemoryAccess access)
{
	if (size == 0 || size > page_size - (address & (page_size - 1)))
		return nullptr;
	return translate(address, access);
}

bool StaticGuestMemory::is_executable(std::uint64_t address) noexcept
{
	/* This no-fault probe also primes the physical code-page handle used by
	 * executable-store publication and stale-block validation. */
	return code_page_handle(address) != nullptr;
}

bool StaticGuestMemory::code_page_stamp(std::uint64_t address,
	void *execution_context,
	::uml::memory::CodePageStamp &stamp) noexcept
{
	(void)execution_context;
	::uml::memory::ExtendedCodePageStamp extended;
	if (code_page_handle(address, &extended) == nullptr)
		return false;
	stamp = extended.stamp;
	return true;
}

bool StaticGuestMemory::extended_code_page_stamp(std::uint64_t address,
	void *execution_context,
	::uml::memory::ExtendedCodePageStamp &stamp) noexcept
{
	(void)execution_context;
	return code_page_handle(address, &stamp) != nullptr;
}

void StaticGuestMemory::invalidate_code(std::uint64_t address)
{
	darwin_uml_static_vm_invalidate_code(
		static_cast<unsigned long>(address));
}

void StaticGuestMemory::invalidate_code(
	std::uint64_t address, void *execution_context)
{
	if (execution_context != nullptr) {
		if (auto *code_page = code_page_handle(address);
		    code_page != nullptr)
			darwin_uml_static_vm_invalidate_code_page(code_page);
		if (execution_context_publish_code_change(
			    execution_context, address))
			return;
	}
	invalidate_code(address);
}

#if DARWIN_UML_X86_64_GUEST
::uml::memory::FrameHold StaticGuestMemory::hold_code_frame(
	std::uint64_t address, void *execution_context,
	::uml::memory::FrameDebt *debts, std::size_t capacity,
	bool global_ok) noexcept
{
	::uml::memory::FrameHold result{};
	/* The bridge writes into locations of its own type. */
	std::array<darwin_uml_static_frame_location, 16> small{};
	std::vector<darwin_uml_static_frame_location> large;
	darwin_uml_static_frame_location *owed = small.data();
	if (capacity > small.size()) {
		try {
			large.resize(capacity);
			owed = large.data();
		} catch (...) {
			/* No room for the locations: the debt is every write
			 * translation, whatever its size. */
			capacity = 0;
			global_ok = true;
		}
	}
	capacity = std::min(capacity, large.empty() ? small.size() : large.size());
	darwin_uml_static_frame_hold hold{};
	hold.global_ok = global_ok ? 1 : 0;
	const auto status = darwin_uml_static_vm_hold_code_frame(
		execution_context, generation_, static_cast<unsigned long>(address),
		owed, capacity, &hold);
	result.record_identity = hold.record_identity;
	result.debt_sequence = hold.debt_sequence;
	result.owed = hold.owed;
	result.record = hold.record;
	result.alias_changes = hold.alias_changes;
	if (status == DARWIN_UML_STATIC_FRAME_OWED)
		for (std::size_t index = 0; index < std::min(hold.owed, capacity); ++index)
			debts[index] = {owed[index].generation, owed[index].page};
	switch (status) {
	case DARWIN_UML_STATIC_FRAME_SETTLED:
		result.result = ::uml::memory::FrameHoldResult::settled;
		break;
	case DARWIN_UML_STATIC_FRAME_OWED:
		result.result = ::uml::memory::FrameHoldResult::owed;
		break;
	case DARWIN_UML_STATIC_FRAME_REFUSED:
		result.result = ::uml::memory::FrameHoldResult::refused;
		break;
	case DARWIN_UML_STATIC_FRAME_CROWDED:
		result.result = ::uml::memory::FrameHoldResult::crowded;
		break;
	case DARWIN_UML_STATIC_FRAME_GLOBAL:
		result.result = ::uml::memory::FrameHoldResult::global;
		break;
	default:
		result.result = ::uml::memory::FrameHoldResult::failed;
		break;
	}
	return result;
}

void StaticGuestMemory::clear_code_debt(std::uint64_t address,
	void *execution_context, const ::uml::memory::FrameHold &hold) noexcept
{
	darwin_uml_static_vm_clear_code_debt(execution_context, generation_,
		static_cast<unsigned long>(address), hold.record_identity,
		hold.debt_sequence);
}

bool StaticGuestMemory::frame_hold_unchanged(
	const ::uml::memory::FrameHold &hold) noexcept
{
	if (hold.record == nullptr)
		return false;
	const darwin_uml_static_frame_state state{nullptr, 0, hold.record,
		hold.alias_changes, 0};
	return darwin_uml_static_vm_frame_state_unchanged(&state) != 0;
}

bool StaticGuestMemory::frame_fill(std::uint64_t address,
	void *execution_context, ::uml::memory::FrameFill &fill) noexcept
{
	if (execution_context == nullptr)
		return false;
	/* The handle the executable-page probe cached: the frame's record. A page
	 * that cannot hold a translation has none, and its writers owe the
	 * holders of its frame a publication all the same. */
	const void *record = code_page_handle(address);
	darwin_uml_static_frame_state state{};
	if (darwin_uml_static_vm_execution_context_frame_fill(execution_context,
		    record, static_cast<unsigned long>(address), &state) != 0)
		return false;
	fill = {state.backing, state.offset, state.record, state.alias_changes,
		state.other_holder != 0};
	return true;
}

bool StaticGuestMemory::frame_fill_unchanged(
	const ::uml::memory::FrameFill &fill) noexcept
{
	const darwin_uml_static_frame_state state{fill.backing, fill.offset,
		fill.record, fill.alias_changes, fill.other_holder ? 1 : 0};
	return darwin_uml_static_vm_frame_state_unchanged(&state) != 0;
}

bool StaticGuestMemory::publish_frame_store(std::uint64_t address,
	std::uint64_t length, void *execution_context, std::uint64_t *own,
	std::size_t capacity,
	::uml::memory::FramePublication &publication) noexcept
{
	darwin_uml_static_frame_publication done{};
	const auto status = darwin_uml_static_vm_publish_frame_store(
		execution_context, generation_, static_cast<unsigned long>(address),
		static_cast<unsigned long>(length), own, capacity, &done);
	publication = {done.overflow != 0, done.own_count, done.own_records};
	return status > 0;
}

bool StaticGuestMemory::frame_own_holders(std::uint64_t address,
	std::uint64_t length, void *execution_context, std::uint64_t *own,
	std::size_t capacity,
	::uml::memory::FramePublication &publication) noexcept
{
	darwin_uml_static_frame_publication done{};
	const auto status = darwin_uml_static_vm_frame_own_holders(
		execution_context, generation_, static_cast<unsigned long>(address),
		static_cast<unsigned long>(length), own, capacity, &done);
	publication = {done.overflow != 0, done.own_count, 0};
	return status > 0;
}

std::uint64_t StaticGuestMemory::flush_serial(
	void *execution_context) noexcept
{
	return darwin_uml_static_vm_execution_context_flush_serial(
		execution_context);
}
#endif

bool StaticGuestMemory::validate_access(
	std::uint64_t address, std::size_t size, ::uml::memory::MemoryAccess access)
{
	std::size_t completed = 0;
	while (completed < size) {
		const auto current = address + completed;
		const auto length = std::min<std::size_t>(
			size - completed, page_size - (current & (page_size - 1)));
		if (translate(current, access) == nullptr)
			return false;
		completed += length;
	}
	return true;
}

::uml::memory::MemoryFault StaticGuestMemory::last_fault() const noexcept
{
	return fault_;
}

bool StaticGuestMemory::has_fault() const noexcept
{
	return fault_recorded_;
}

constexpr std::size_t StaticGuestMemory::index_for(
	std::uint64_t address) noexcept
{
	const auto page = address >> DARWIN_UML_GUEST_PAGE_SHIFT;
	return static_cast<std::size_t>(
		(page ^ (page >> tlb_index_bits)) & (tlb_size - 1));
}

constexpr std::size_t StaticGuestMemory::access_index(
	::uml::memory::MemoryAccess access) noexcept
{
	return access == ::uml::memory::MemoryAccess::read ? 0 :
		access == ::uml::memory::MemoryAccess::write ? 1 : 2;
}

constexpr darwin_uml_static_access StaticGuestMemory::host_access(
	::uml::memory::MemoryAccess access) noexcept
{
	return access == ::uml::memory::MemoryAccess::read ? DARWIN_UML_STATIC_READ :
		access == ::uml::memory::MemoryAccess::write ? DARWIN_UML_STATIC_WRITE :
		DARWIN_UML_STATIC_EXECUTE;
}

std::byte *StaticGuestMemory::translate(
	std::uint64_t address, ::uml::memory::MemoryAccess access)
{
	const auto page = address & ~(page_size - 1);
	auto &entry = tlb_[index_for(address)];
	auto &base = entry.bases[access_index(access)];
	if (entry.page == page && entry.generation == generation_ &&
	    entry.changes == changes_ && base != nullptr)
		return base + (address & (page_size - 1));

	void *translated = nullptr;
	const int result = darwin_uml_static_vm_translate(
		generation_, static_cast<unsigned long>(page), host_access(access),
		&translated);
	if (result != DARWIN_UML_STATIC_TRANSLATED || translated == nullptr) {
		record_fault(address, access);
		return nullptr;
	}
	if (entry.page != page || entry.generation != generation_ ||
	    entry.changes != changes_) {
		entry = {};
		entry.page = page;
		entry.generation = generation_;
		entry.changes = changes_;
	}
	base = static_cast<std::byte *>(translated);
	return base + (address & (page_size - 1));
}

void *StaticGuestMemory::code_page_handle(std::uint64_t address,
	::uml::memory::ExtendedCodePageStamp *stamp) noexcept
{
	const auto page = address & ~(page_size - 1);
	auto &entry = tlb_[index_for(address)];
	darwin_uml_static_code_page_stamp observed{};
	if (entry.page == page && entry.generation == generation_ &&
	    entry.changes == changes_ && entry.code_page != nullptr &&
	    !holds_holder(entry.code_page)) {
		if (darwin_uml_static_vm_code_page_stamp(
			entry.code_page, &observed) != 0)
			return nullptr;
	} else {
		void *handle = nullptr;
		if (darwin_uml_static_vm_code_page(
			generation_, static_cast<unsigned long>(page), &handle,
			&observed) != DARWIN_UML_STATIC_TRANSLATED ||
		    handle == nullptr)
			return nullptr;
		if (entry.page != page || entry.generation != generation_ ||
		    entry.changes != changes_) {
			entry = {};
			entry.page = page;
			entry.generation = generation_;
			entry.changes = changes_;
		}
		entry.code_page = handle;
		entry.immutable =
			(observed.flags & DARWIN_UML_STATIC_STAMP_IMMUTABLE) != 0;
	}
	if (stamp != nullptr)
		*stamp = {{observed.identity, observed.version}, observed.epoch,
			entry.immutable &&
				(observed.flags & DARWIN_UML_STATIC_STAMP_NEVER_SHARE) == 0,
			entry.immutable};
	return entry.code_page;
}

std::byte *StaticGuestMemory::translate_execute(std::uint64_t address)
{
	const auto page = address & ~(page_size - 1);
	auto &entry = tlb_[index_for(address)];
	auto &base = entry.bases[access_index(::uml::memory::MemoryAccess::execute)];
	if (entry.page == page && entry.generation == generation_ &&
	    entry.changes == changes_ && base != nullptr &&
	    holds_holder(entry.code_page)) {
		register_holder(untag_holder(entry.code_page));
		return base + (address & (page_size - 1));
	}

	void *translated = nullptr;
	void *holder = nullptr;
	const int result = darwin_uml_static_vm_translate_execute(
		generation_, static_cast<unsigned long>(page), &translated,
		&holder);
	if (result != DARWIN_UML_STATIC_TRANSLATED || translated == nullptr) {
		record_fault(address, ::uml::memory::MemoryAccess::execute);
		return nullptr;
	}
	if (entry.page != page || entry.generation != generation_ ||
	    entry.changes != changes_) {
		entry = {};
		entry.page = page;
		entry.generation = generation_;
		entry.changes = changes_;
	}
	base = static_cast<std::byte *>(translated);
	/* A null word is cached too: the backing publishes to every generation,
	 * so there is nothing to register until the mapping changes. */
	entry.code_page = tag_holder(static_cast<code_holders::Word *>(holder));
	register_holder(static_cast<code_holders::Word *>(holder));
	return base + (address & (page_size - 1));
}

void StaticGuestMemory::register_holder(code_holders::Word *word) noexcept
{
	if (word == nullptr)
		return;
	const auto slot = code_holders::slot_of(generation_);
	const auto held = word->load(std::memory_order_relaxed);
	if (code_holders::holds(held, slot))
		return;
	register_holder_slow(*word, slot, held);
}

/* The word does not name this generation: add it, after a publication took
 * the word or before any translator came. The add is a sequentially
 * consistent exchange followed by a fence, both before the caller reads the
 * bytes: a publisher that stores the bytes and then misses this registration
 * is one whose store this fence orders before the read. The fence is paid
 * once per frame per publication, not per fetch. */
void StaticGuestMemory::register_holder_slow(code_holders::Word &word,
	std::uint32_t slot, std::uint64_t held) noexcept
{
	using code_holders::all;
	using code_holders::high;
	using code_holders::join;
	using code_holders::low;
	/* A generation the host no longer has holds nothing worth keeping. A
	 * negative answer (the host cannot say) keeps it. */
	const auto retired = [](std::uint32_t generation) {
		return darwin_uml_static_vm_generation_live(generation) == 0;
	};
	for (;;) {
		if (code_holders::holds(held, slot))
			return;
		std::uint64_t desired = all;
		if (slot != code_holders::slot_all) {
			if (low(held) == 0) {
				desired = join(slot, high(held));
			} else if (high(held) == 0) {
				desired = join(low(held), slot);
			} else {
				/* Both slots are taken: the way to `all` goes through the
				 * slots whose generations are gone. */
				const auto first = retired(low(held)) ? 0 : low(held);
				const auto second = retired(high(held)) ? 0 : high(held);
				if (first == 0)
					desired = join(slot, second);
				else if (second == 0)
					desired = join(first, slot);
			}
		}
		if (word.compare_exchange_weak(held, desired,
			    std::memory_order_seq_cst, std::memory_order_relaxed)) {
			using code_holders::detail::CounterStore;
			code_holders::count(&CounterStore::registrations);
			if (desired == all)
				code_holders::count(&CounterStore::overflows);
			std::atomic_thread_fence(std::memory_order_seq_cst);
			return;
		}
	}
}

bool StaticGuestMemory::copy_from_guest(
	std::uint64_t address, std::span<std::byte> destination,
	::uml::memory::MemoryAccess access)
{
	const bool registering = register_holders_ &&
		access == ::uml::memory::MemoryAccess::execute;
	std::size_t completed = 0;
	while (completed < destination.size()) {
		const auto current = address + completed;
		const auto length = std::min<std::size_t>(
			destination.size() - completed,
			page_size - (current & (page_size - 1)));
		const auto *source = registering ? translate_execute(current) :
			translate(current, access);
		if (source == nullptr)
			return false;
		std::memcpy(destination.data() + completed, source, length);
#if UML_STATIC_GUEST_MEMORY_TESTING
		if (registering)
			if (const auto observer =
				    fetch_observer().load(std::memory_order_acquire))
				observer(current);
#endif
		completed += length;
	}
	return true;
}

bool StaticGuestMemory::record_fault(
	std::uint64_t address, ::uml::memory::MemoryAccess access)
{
	fault_ = {address, access};
	fault_recorded_ = true;
	return false;
}

} // namespace uml::memory
