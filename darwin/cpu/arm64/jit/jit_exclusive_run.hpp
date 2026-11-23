// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/jit/jit_runtime_contract.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace uml::cpu::arm64 {

/*
 * The word classes of a native exclusive run, which begins at a block's
 * load-exclusive and ends at its store-exclusive:
 *
 *   ldxr Rt, [Xn]  M1 .. Mk  stxr Ws, Rd, [Xn]  (the retry)     0 <= k <= 4
 *
 * One decoder serves the executor's look-ahead, which decides how many words
 * of a block to fetch, and the compiler's admission, which decides what to
 * fuse, so the two can never disagree about a word. It judges one word alone;
 * what a run's words do to each other is the compiler's.
 */
enum class ExclusiveRunWord : unsigned char {
  other,
  /* LDXR, LDAXR of 1, 2, 4 or 8 bytes: no pair, no ordered-only load. */
  load_exclusive,
  /* STXR, STLXR of the same sizes. */
  store_exclusive,
  /* A register-only instruction that sets no register but those it names:
   * MOVZ, MOVN, MOVK; ADD, ADDS, SUB, SUBS immediate and shifted register;
   * AND, ORR, EOR, ANDS, BIC, ORN, EON, BICS in both immediate and shifted
   * forms. A reserved encoding, SP as an operand and a write to SP are not
   * one. */
  middle,
  /* A conditional branch the run may leave through: B.cond except AL and NV,
   * CBZ, CBNZ, TBZ, TBNZ. */
  side_exit,
};

/* Load, four middles, store and the retry that follows it. */
inline constexpr std::size_t maximum_exclusive_run_middles = 4;
inline constexpr std::size_t maximum_exclusive_run_words =
    maximum_exclusive_run_middles + 3U;

namespace detail {

/* Whether N:imms:immr names a bitmask immediate (DecodeBitMasks). */
[[nodiscard]] constexpr bool valid_bitmask_immediate(bool wide, unsigned n,
                                                     unsigned imms) noexcept {
  if (!wide && n != 0U)
    return false;
  const unsigned combined = (n << 6U) | (~imms & 0x3fU);
  if (combined < 2U)
    return false;
  const unsigned length = 31U - static_cast<unsigned>(std::countl_zero(combined));
  const unsigned levels = (1U << length) - 1U;
  return (imms & levels) != levels;
}

} // namespace detail

[[nodiscard]] constexpr ExclusiveRunWord
exclusive_run_word_kind(std::uint32_t word) noexcept {
  /* Scalar exclusives: size 001000 o2=0 L o1=0 Rs o0 Rt2 Rn Rt. */
  if ((word & 0x3fe0'0000U) == 0x0840'0000U)
    return ExclusiveRunWord::load_exclusive;
  if ((word & 0x3fe0'0000U) == 0x0800'0000U)
    return ExclusiveRunWord::store_exclusive;

  const bool wide = (word >> 31U) != 0U;
  const unsigned rd = word & 31U;
  const unsigned rn = (word >> 5U) & 31U;
  const unsigned opc = (word >> 29U) & 3U;

  /* Move wide immediate. */
  if ((word & 0x1f80'0000U) == 0x1280'0000U)
    return opc != 1U && (wide || ((word >> 22U) & 1U) == 0U)
               ? ExclusiveRunWord::middle
               : ExclusiveRunWord::other;
  /* ADD, SUB immediate: Rn and a non-flag Rd name SP at 31. */
  if ((word & 0x1f80'0000U) == 0x1100'0000U) {
    const bool sets_flags = (word & 0x2000'0000U) != 0U;
    return rn != 31U && (rd != 31U || sets_flags) ? ExclusiveRunWord::middle
                                                  : ExclusiveRunWord::other;
  }
  /* ADD, SUB shifted register: shift type 3 is reserved. */
  if ((word & 0x1f20'0000U) == 0x0b00'0000U) {
    const unsigned shift = (word >> 22U) & 3U;
    const unsigned amount = (word >> 10U) & 63U;
    return shift != 3U && (wide || amount < 32U) ? ExclusiveRunWord::middle
                                                 : ExclusiveRunWord::other;
  }
  /* AND, ORR, EOR, ANDS shifted register, with the inverted forms. */
  if ((word & 0x1f00'0000U) == 0x0a00'0000U) {
    const unsigned amount = (word >> 10U) & 63U;
    return wide || amount < 32U ? ExclusiveRunWord::middle
                                : ExclusiveRunWord::other;
  }
  /* AND, ORR, EOR, ANDS immediate: Rd names SP at 31 unless it sets flags. */
  if ((word & 0x1f80'0000U) == 0x1200'0000U) {
    const bool sets_flags = opc == 3U;
    return detail::valid_bitmask_immediate(wide, (word >> 22U) & 1U,
                                           (word >> 10U) & 0x3fU) &&
                   (rd != 31U || sets_flags)
               ? ExclusiveRunWord::middle
               : ExclusiveRunWord::other;
  }

  /* B.cond; BC.cond (bit 4) and the always conditions are not side exits. */
  if ((word & 0xff00'0010U) == 0x5400'0000U)
    return (word & 15U) < 14U ? ExclusiveRunWord::side_exit
                              : ExclusiveRunWord::other;
  /* CBZ, CBNZ, TBZ, TBNZ. */
  if ((word & 0x7e00'0000U) == 0x3400'0000U ||
      (word & 0x7e00'0000U) == 0x3600'0000U)
    return ExclusiveRunWord::side_exit;
  return ExclusiveRunWord::other;
}

static_assert(exclusive_run_word_kind(0xc85f'7c23U) ==
              ExclusiveRunWord::load_exclusive); // ldxr x3, [x1]
static_assert(exclusive_run_word_kind(0x885f'fe30U) ==
              ExclusiveRunWord::load_exclusive); // ldaxr w16, [x17]
static_assert(exclusive_run_word_kind(0xc805'7c22U) ==
              ExclusiveRunWord::store_exclusive); // stxr w5, x2, [x1]
static_assert(exclusive_run_word_kind(0xc87f'0c22U) ==
              ExclusiveRunWord::other); // ldxp: a pair
static_assert(exclusive_run_word_kind(0xc8df'fc23U) ==
              ExclusiveRunWord::other); // ldar x3, [x1]
static_assert(exclusive_run_word_kind(0xeb04'007fU) ==
              ExclusiveRunWord::middle); // cmp x3, x4
static_assert(exclusive_run_word_kind(0x9100'0463U) ==
              ExclusiveRunWord::middle); // add x3, x3, #1
static_assert(exclusive_run_word_kind(0x9100'03e3U) ==
              ExclusiveRunWord::other); // add x3, sp, #0
static_assert(exclusive_run_word_kind(0x1200'0020U) ==
              ExclusiveRunWord::middle); // and w0, w1, #1
static_assert(exclusive_run_word_kind(0x1200'003fU) ==
              ExclusiveRunWord::other); // and wsp, w1, #1
static_assert(exclusive_run_word_kind(0x1240'0020U) ==
              ExclusiveRunWord::other); // and w0, w1 with N=1, a 32-bit run
static_assert(exclusive_run_word_kind(0x5400'0061U) ==
              ExclusiveRunWord::side_exit); // b.ne
static_assert(exclusive_run_word_kind(0x5400'006eU) ==
              ExclusiveRunWord::other); // b.al
static_assert(exclusive_run_word_kind(0x5400'0071U) ==
              ExclusiveRunWord::other); // bc.ne
static_assert(exclusive_run_word_kind(0x35ff'ff85U) ==
              ExclusiveRunWord::side_exit); // cbnz w5
static_assert(exclusive_run_word_kind(0x3607'ff85U) ==
              ExclusiveRunWord::side_exit); // tbz w5, #0
static_assert(exclusive_run_word_kind(0xd503'201fU) ==
              ExclusiveRunWord::other); // nop
static_assert(exclusive_run_word_kind(0x1400'0000U) ==
              ExclusiveRunWord::other); // b

/*
 * Reads the instruction words of the block that starts at `pc` into `words`
 * through `fetch(address, word)`, which reports whether the word could be
 * fetched, and returns how many it read: through the first branch or other
 * obvious block end, as a translation does.
 *
 * A block that starts with a load-exclusive reaches its store-exclusive only
 * if the span does, and the first conditional branch of a lock sequence comes
 * before the store. So when the ordinary span ends at such a branch, with
 * nothing but admissible words between it and the load, the span goes on
 * through further admissible words to the first store-exclusive and the one
 * word after it, the retry. It stops short, keeping the ordinary span, when
 *
 *  - a word is not one of the run's, or an unconditional terminator comes;
 *  - the store-exclusive is not among words 1 to 5, which no run of at most
 *    four middles reaches;
 *  - a word, the retry included, lies past the guest page that holds the
 *    load, so the span is a function of one page's words and every executor
 *    fetches the same span for the same words;
 *  - a word cannot be fetched: speculation never raises a fault.
 *
 * A span that already holds its store-exclusive is never extended.
 */
template <typename Fetch>
[[nodiscard]] std::size_t
exclusive_run_span(std::uint64_t pc, std::span<std::uint32_t> words,
                   std::size_t ordinary, Fetch &&fetch) {
  if (ordinary < 2U ||
      exclusive_run_word_kind(words[0]) != ExclusiveRunWord::load_exclusive ||
      exclusive_run_word_kind(words[ordinary - 1U]) !=
          ExclusiveRunWord::side_exit)
    return ordinary;
  for (std::size_t index = 1; index < ordinary; ++index) {
    const auto kind = exclusive_run_word_kind(words[index]);
    if (kind != ExclusiveRunWord::middle &&
        kind != ExclusiveRunWord::side_exit)
      return ordinary;
  }
  const std::uint64_t page_end =
      (pc & ~(page_size - 1U)) + page_size;
  for (std::size_t index = ordinary;
       index <= maximum_exclusive_run_middles + 1U &&
       index + 1U < words.size();
       ++index) {
    const std::uint64_t address = pc + index * sizeof(std::uint32_t);
    std::uint32_t word{};
    if (address >= page_end || !fetch(address, word))
      return ordinary;
    words[index] = word;
    const auto kind = exclusive_run_word_kind(word);
    if (kind == ExclusiveRunWord::store_exclusive) {
      std::uint32_t retry{};
      const std::uint64_t retry_address = address + sizeof(std::uint32_t);
      if (retry_address >= page_end || !fetch(retry_address, retry))
        return ordinary;
      words[index + 1U] = retry;
      return index + 2U;
    }
    if (kind != ExclusiveRunWord::middle &&
        kind != ExclusiveRunWord::side_exit)
      return ordinary;
  }
  return ordinary;
}

/* The words of the block at `pc`: the ordinary span, extended to the end of a
 * lock sequence as above. Returns the number read; zero if none could be. */
template <typename Fetch>
[[nodiscard]] std::size_t fetch_translation_words(
    std::uint64_t pc, std::span<std::uint32_t> words, Fetch &&fetch) {
  std::size_t count = 0;
  while (count < words.size()) {
    if (!fetch(pc + count * sizeof(std::uint32_t), words[count]))
      break;
    const auto terminates =
        obvious_translated_block_terminator(words[count]);
    ++count;
    if (terminates)
      break;
  }
  return count == 0U ? 0U : exclusive_run_span(pc, words, count, fetch);
}

} // namespace uml::cpu::arm64
