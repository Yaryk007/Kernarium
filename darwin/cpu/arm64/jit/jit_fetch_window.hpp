// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace uml::cpu::arm64 {

[[nodiscard]] constexpr bool
obvious_translated_block_terminator(std::uint32_t instruction) noexcept {
  return (instruction & 0x7c00'0000U) == 0x1400'0000U ||
         (instruction & 0xff00'0000U) == 0x5400'0000U ||
         (instruction & 0x7e00'0000U) == 0x3400'0000U ||
         (instruction & 0x7e00'0000U) == 0x3600'0000U ||
         (instruction & 0xfe00'0000U) == 0xd600'0000U ||
         (instruction & 0xff00'0000U) == 0xd400'0000U;
}


/* Read a short run under one execute-permission lookup. Do not probe the
 * next page until the current page's instructions actually require it. */
template <typename Fetch>
[[nodiscard]] std::size_t fetch_a64_jit_window(std::uint64_t pc,
    std::span<std::uint32_t> words, std::size_t page_bytes, Fetch &&fetch) {
  std::size_t count = 0;
  while (count < words.size()) {
    const auto at = pc + count * sizeof(std::uint32_t);
    const auto remaining = page_bytes - (at & (page_bytes - 1));
    const auto length = std::clamp<std::size_t>(remaining / sizeof(std::uint32_t), 1, 8);
    auto run = words.subspan(count, std::min(length, words.size() - count));
    if (!fetch(at, std::as_writable_bytes(run))) {
      if (run.size() == 1)
        return count;
      run = run.first(1);
      if (!fetch(at, std::as_writable_bytes(run)))
        return count;
    }
    for (const auto instruction : run) {
      ++count;
      if (!obvious_translated_block_terminator(instruction))
        continue;
      // A compare/exchange retry can include two instructions after its exit.
      if (count == 3 && (words[0] & 0x3fe0'0000U) == 0x0840'0000U &&
          (words[1] & 0x7fe0'fc1fU) == 0x6b00'001fU &&
          (words[2] & 0xff00'001fU) == 0x5400'0001U) {
        while (count < std::min<std::size_t>(5, words.size()) &&
               fetch(pc + count * 4, std::as_writable_bytes(words.subspan(count, 1))))
          ++count;
      }
      return count;
    }
  }
  return count;
}

} // namespace uml::cpu::arm64
