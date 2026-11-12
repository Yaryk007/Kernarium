// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace uml::cpu::jit_memory {

/* Which granules of one range of JIT memory are in use, lowest free run first.
 * It holds no memory of its own and calls nothing that maps or publishes
 * code: the range's owner locks around it, and the owner is the only place
 * that touches executable memory.
 */
template <std::size_t Granules>
class GranuleSet final {
	static_assert(Granules % 64U == 0);

public:
	static constexpr std::size_t none = ~std::size_t{0};

	/* The first granule of the lowest run of `count` free granules among the
	 * first `limit`, or none. */
	[[nodiscard]] constexpr std::size_t find(
		std::size_t count, std::size_t limit) const noexcept
	{
		if (count == 0 || limit > Granules)
			return none;
		std::size_t run = 0;
		for (std::size_t i = 0; i < limit;) {
			const auto word = used_[i / 64U];
			if (i % 64U == 0 && word == ~std::uint64_t{0}) {
				run = 0;
				i += 64U;
				continue;
			}
			if (((word >> (i % 64U)) & 1U) != 0)
				run = 0;
			else if (++run == count)
				return i + 1U - count;
			++i;
		}
		return none;
	}

	constexpr void mark(
		std::size_t first, std::size_t count, bool used) noexcept
	{
		for (auto i = first; i < first + count && i < Granules; ++i) {
			const auto bit = std::uint64_t{1} << (i % 64U);
			if (used)
				used_[i / 64U] |= bit;
			else
				used_[i / 64U] &= ~bit;
		}
	}

	[[nodiscard]] constexpr std::size_t used(std::size_t limit) const noexcept
	{
		std::size_t total = 0;
		for (std::size_t i = 0; i < limit && i < Granules; ++i)
			total += static_cast<std::size_t>(
				(used_[i / 64U] >> (i % 64U)) & 1U);
		return total;
	}

private:
	std::array<std::uint64_t, Granules / 64U> used_{};
};

} // namespace uml::cpu::jit_memory
