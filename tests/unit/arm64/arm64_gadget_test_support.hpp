// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* What the gadget-structure tests compare a built program's words against:
 * a gadget's entry address and a vector register's CpuState offset; and the
 * blocks the link tests follow, laid out as the executor stores them. */

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/floating_controls.hpp"
#include "cpu/arm64/gadgets/afp_twin.hpp"
#include "cpu/arm64/gadgets/program.hpp"
#include "cpu/host_capabilities.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

extern "C" void uml_arm64_gadget_link_exit();

namespace uml::emulator::tests {

/* An extern "C" gadget entry, as the tests declare them. */
using Gadget = void (*)();

[[nodiscard]] inline std::uintptr_t address(Gadget gadget) noexcept
{
	return reinterpret_cast<std::uintptr_t>(gadget);
}

/* Words laid out as the executor stores a block a link names: behind the
 * header gadgets/program.hpp describes, holding the block's guest PC and
 * instruction count. The words stay where they are when the block moves. */
class LinkedBlock final {
public:
	LinkedBlock(std::uint64_t pc, std::size_t instruction_count,
		std::span<const std::uintptr_t> words)
		: words_(header_words + words.size())
	{
		words_[0] = pc;
		words_[1] = std::uintptr_t{instruction_count}
			<< ((::uml::cpu::arm64::gadgets::block_header_count -
				sizeof(std::uintptr_t)) * 8);
		std::copy(words.begin(), words.end(),
			words_.begin() + header_words);
	}

	explicit LinkedBlock(const ::uml::cpu::arm64::gadgets::Program &program)
		: LinkedBlock(program.guest_address(),
			program.instruction_count(), program.words())
	{}

	LinkedBlock(const LinkedBlock &) = delete;
	LinkedBlock &operator=(const LinkedBlock &) = delete;
	LinkedBlock(LinkedBlock &&) noexcept = default;
	LinkedBlock &operator=(LinkedBlock &&) noexcept = default;

	/* Point `link` at this block, as the executor links a record. */
	void link(::uml::cpu::arm64::gadgets::BlockLink &link) const noexcept
	{
		link.entry = words_[header_words];
		link.stream = words_.data() + header_words + 1;
	}

private:
	static constexpr std::size_t header_words =
		::uml::cpu::arm64::gadgets::block_header_bytes /
		sizeof(std::uintptr_t);
	std::vector<std::uintptr_t> words_;
};

/* Point `link` at nothing, as the executor makes and resets a record. */
inline void unlink(::uml::cpu::arm64::gadgets::BlockLink &link) noexcept
{
	link.entry = address(uml_arm64_gadget_link_exit);
	link.stream = ::uml::cpu::arm64::gadgets::unlinked_stream(link);
}

/* A link for `pc` that names nothing yet. Unlinked, a link names its own
 * words, so it stays where it was made. */
struct UnlinkedLink final {
	explicit UnlinkedLink(std::uint64_t pc) noexcept : link{.guest_pc = pc}
	{
		unlink(link);
	}
	UnlinkedLink(const UnlinkedLink &) = delete;
	UnlinkedLink &operator=(const UnlinkedLink &) = delete;

	::uml::cpu::arm64::gadgets::BlockLink link;
};

/* The word a default Builder emits for a selected entry: where the virtual
 * CPU implements FEAT_AFP on the host capabilities it translates for, the
 * entry's FEAT_AFP twin, and the entry itself elsewhere. */
[[nodiscard]] inline std::uintptr_t emitted_entry(std::uintptr_t entry)
{
	return ::uml::cpu::arm64::implements_afp(
		       ::uml::cpu::host_capabilities()) ?
		::uml::cpu::arm64::gadgets::afp_twin(entry) : entry;
}

/* Computed here rather than with the builder's helper, so a packed operand
 * is checked against an independent value. */
[[nodiscard]] constexpr std::uintptr_t vector_offset(unsigned reg) noexcept
{
	return offsetof(::uml::cpu::arm64::CpuState, vectors) +
		reg * sizeof(::uml::cpu::arm64::VectorRegister);
}

} // namespace uml::emulator::tests
