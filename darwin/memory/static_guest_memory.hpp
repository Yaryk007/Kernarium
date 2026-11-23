// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "memory/guest_address_space.hpp"
#include "memory/mmu/code_holders.hpp"
#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace uml::memory {

class StaticGuestMemory final : public ::uml::memory::GuestAddressSpace {
public:
	/* Whether an instruction fetch registers this memory's generation as a
	 * holder of the frame it reads, before it reads it, so that another
	 * process's code synchronization of the frame reaches the generation
	 * (mmu/code_holders.hpp). The arm64 engines' entry loop turns it on for
	 * the length of a run; the x86 engines never do, and nothing of theirs
	 * changes. */
	void set_code_holder_registration(bool enabled) noexcept
	{
		register_holders_ = enabled;
	}
	void set_generation(unsigned long generation) noexcept;
	void prepare(unsigned long generation, std::uint64_t changes) noexcept;

	[[nodiscard]] bool read(
		std::uint64_t address, std::span<std::byte> destination) override;
	[[nodiscard]] bool fetch(
		std::uint64_t address, std::span<std::byte> destination) override;
	[[nodiscard]] bool write(
		std::uint64_t address, std::span<const std::byte> source) override;
	[[nodiscard]] bool validate_access(
		std::uint64_t address, std::size_t size,
		::uml::memory::MemoryAccess access) override;
	[[nodiscard]] std::byte *atomic_pointer(
		std::uint64_t address, std::size_t size,
		::uml::memory::MemoryAccess access) override;
	[[nodiscard]] bool is_executable(std::uint64_t address) noexcept override;
	[[nodiscard]] bool code_page_stamp(std::uint64_t address,
		void *execution_context,
		::uml::memory::CodePageStamp &stamp) noexcept override;
	[[nodiscard]] bool extended_code_page_stamp(std::uint64_t address,
		void *execution_context,
		::uml::memory::ExtendedCodePageStamp &stamp) noexcept override;
	void invalidate_code(std::uint64_t address) override;
	void invalidate_code(
		std::uint64_t address, void *execution_context) override;
#if DARWIN_UML_X86_64_GUEST
	[[nodiscard]] ::uml::memory::FrameHold hold_code_frame(
		std::uint64_t address, void *execution_context,
		::uml::memory::FrameDebt *debts, std::size_t capacity,
		bool global_ok) noexcept override;
	void clear_code_debt(std::uint64_t address, void *execution_context,
		const ::uml::memory::FrameHold &hold) noexcept override;
	[[nodiscard]] bool frame_hold_unchanged(
		const ::uml::memory::FrameHold &hold) noexcept override;
	[[nodiscard]] bool frame_fill(std::uint64_t address,
		void *execution_context,
		::uml::memory::FrameFill &fill) noexcept override;
	[[nodiscard]] bool frame_fill_unchanged(
		const ::uml::memory::FrameFill &fill) noexcept override;
	[[nodiscard]] bool publish_frame_store(std::uint64_t address,
		std::uint64_t length, void *execution_context, std::uint64_t *own,
		std::size_t capacity,
		::uml::memory::FramePublication &publication) noexcept override;
	[[nodiscard]] bool frame_own_holders(std::uint64_t address,
		std::uint64_t length, void *execution_context, std::uint64_t *own,
		std::size_t capacity,
		::uml::memory::FramePublication &publication) noexcept override;
	[[nodiscard]] std::uint64_t flush_serial(
		void *execution_context) noexcept override;
#endif
	[[nodiscard]] ::uml::memory::MemoryFault last_fault() const noexcept override;
	[[nodiscard]] bool has_fault() const noexcept override;

private:
	static constexpr std::uint64_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
	static constexpr std::size_t tlb_index_bits = 13U;
	static constexpr std::size_t tlb_size = 1U << tlb_index_bits;

	struct TlbEntry final {
		std::uint64_t page{1};
		std::uint64_t generation{};
		std::uint64_t changes{};
		std::array<std::byte *, 3> bases{};
		void *code_page{};
		/* The kernel's immutable fact for the mapping code_page was
		 * asked through; a mapping change refreshes it with the entry. */
		bool immutable{};
	};

	[[nodiscard]] static constexpr std::size_t index_for(
		std::uint64_t address) noexcept;
	[[nodiscard]] static constexpr std::size_t access_index(
		::uml::memory::MemoryAccess access) noexcept;
	[[nodiscard]] static constexpr darwin_uml_static_access host_access(
		::uml::memory::MemoryAccess access) noexcept;
	[[nodiscard]] std::byte *translate(
		std::uint64_t address, ::uml::memory::MemoryAccess access);
	/* The execute arm of translate() for a memory that registers: it keeps
	 * the frame's holder word in the TLB entry's code-page slot, tagged so
	 * that a code-page handle is never mistaken for it, and checks the word
	 * at every fetch. The word, never a "registered" bit, is what is cached:
	 * a publication empties it and the next fetch must register again. */
	[[nodiscard]] std::byte *translate_execute(std::uint64_t address);
	void register_holder(::uml::memory::code_holders::Word *word) noexcept;
	void register_holder_slow(::uml::memory::code_holders::Word &word,
		std::uint32_t slot, std::uint64_t held) noexcept;
	[[nodiscard]] void *code_page_handle(
		std::uint64_t address,
		::uml::memory::ExtendedCodePageStamp *stamp = nullptr) noexcept;
	[[nodiscard]] bool copy_from_guest(
		std::uint64_t address, std::span<std::byte> destination,
		::uml::memory::MemoryAccess access);
	bool record_fault(std::uint64_t address, ::uml::memory::MemoryAccess access);

	unsigned long generation_{};
	std::uint64_t changes_{};
	std::array<TlbEntry, tlb_size> tlb_{};
	::uml::memory::MemoryFault fault_{};
	bool fault_recorded_{};
	bool register_holders_{};
};

} // namespace uml::memory
