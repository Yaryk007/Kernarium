// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace uml::cpu::arm64::gadgets::builder_detail {

/* Where the decode group of each of the builder's recent instructions begins,
 * which is where a run head goes in front of a run (program_builder.cpp). An
 * instruction that an earlier instruction's group took in, the second of a
 * pair a decoder fuses or a move of a row behind the first, has no group of
 * its own, nor has one the builder has not reached: no run that holds such an
 * instruction is grouped, whatever words its group shares with others. */
class GroupStarts final {
public:
	/* The recent instructions it holds, which the longest run and the
	 * instruction behind it must fit. */
	static constexpr std::size_t ring = 32;

	/* Instruction `index`'s own group begins at word `word`. */
	void begin(std::size_t index, std::size_t word) noexcept
	{
		starts_[index % ring] = static_cast<std::uint32_t>(word);
	}

	/* Instruction `index` is part of an earlier instruction's group. */
	void join(std::size_t index) noexcept
	{
		starts_[index % ring] = none;
	}

	/* Where instruction `index`'s own group begins, or a word past any
	 * block's when it has none. */
	[[nodiscard]] std::size_t operator[](std::size_t index) const noexcept
	{
		return starts_[index % ring];
	}

	/* The groups of instructions [first, last) moved `words` words on. */
	void shift(std::size_t first, std::size_t last, std::size_t words) noexcept
	{
		for (auto index = first; index < last; ++index)
			starts_[index % ring] += static_cast<std::uint32_t>(words);
	}

	/* Whether each of instructions [first, last) is a group of its own of
	 * exactly `words` words, the last of them followed by word `next`. */
	[[nodiscard]] bool grouped(std::size_t first, std::size_t last,
		std::size_t words, std::size_t next) const noexcept
	{
		for (auto index = first; index < last; ++index) {
			const auto start = starts_[index % ring];
			const auto after = index + 1 == last ? next :
				std::size_t{starts_[(index + 1) % ring]};
			if (start == none || after != std::size_t{start} + words)
				return false;
		}
		return true;
	}

private:
	static constexpr std::uint32_t none =
		std::numeric_limits<std::uint32_t>::max();

	static constexpr std::array<std::uint32_t, ring> nothing() noexcept
	{
		std::array<std::uint32_t, ring> starts{};
		starts.fill(none);
		return starts;
	}

	std::array<std::uint32_t, ring> starts_ = nothing();
};

} // namespace uml::cpu::arm64::gadgets::builder_detail
