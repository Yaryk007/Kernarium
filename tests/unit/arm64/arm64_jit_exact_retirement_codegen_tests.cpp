// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

using uml::emulator::tests::require;

namespace {

namespace jit_test = uml::cpu::arm64::jit::testing;

constexpr std::uint32_t add_x5_one = 0x9100'04a5U;
constexpr std::uint32_t load_pair_q0_q1_x0 = 0xad40'0400U;
constexpr std::uint32_t system_call = 0xd400'0001U;
constexpr std::uint32_t add_packed_result = 0x8b01'0000U;
constexpr std::uint32_t old_merge_packed_result = 0xaa01'0000U;

[[nodiscard]] constexpr std::uint32_t
move_wide(bool keep, unsigned destination, unsigned immediate,
          unsigned half) noexcept {
  return (keep ? 0xf280'0000U : 0xd280'0000U) |
         ((half & 3U) << 21U) | ((immediate & 0xffffU) << 5U) |
         (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
move_not_wide(unsigned destination, unsigned immediate,
              unsigned half) noexcept {
  return 0x9280'0000U | ((half & 3U) << 21U) |
         ((immediate & 0xffffU) << 5U) | (destination & 31U);
}

[[nodiscard]] std::span<const std::uint32_t>
hot(const jit_test::CompiledCode &compiled) {
  require(compiled.hot_words <= compiled.words.size());
  return {compiled.words.data(), compiled.hot_words};
}

[[nodiscard]] std::size_t count(std::span<const std::uint32_t> words,
                                std::uint32_t value) {
  return static_cast<std::size_t>(std::count(words.begin(), words.end(),
                                             value));
}

void constant_materialization_starts_at_the_first_nonzero_half() {
  constexpr unsigned destination = 7;
  const auto zero =
      jit_test::materialize_constant_for_testing(destination, 0);
  constexpr std::array zero_expected{
      move_wide(false, destination, 0, 0),
  };
  require(zero.size() == zero_expected.size() &&
          std::equal(zero_expected.begin(), zero_expected.end(), zero.begin()));

  const auto upper = jit_test::materialize_constant_for_testing(
      destination, std::uint64_t{0x1234} << 32U);
  constexpr std::array upper_expected{
      move_wide(false, destination, 0x1234, 2),
  };
  require(upper.size() == upper_expected.size() &&
          std::equal(upper_expected.begin(), upper_expected.end(),
                     upper.begin()));

  const auto sparse = jit_test::materialize_constant_for_testing(
      destination, 0xabcd'0000'5678'0000ULL);
  constexpr std::array sparse_expected{
      move_wide(false, destination, 0x5678, 1),
      move_wide(true, destination, 0xabcd, 3),
  };
  require(sparse.size() == sparse_expected.size() &&
          std::equal(sparse_expected.begin(), sparse_expected.end(),
                     sparse.begin()));
}

void constant_materialization_uses_inversion_only_when_shorter() {
  constexpr unsigned destination = 7;
  const auto all_ones = jit_test::materialize_constant_for_testing(
      destination, ~std::uint64_t{0});
  constexpr std::array all_ones_expected{
      move_not_wide(destination, 0, 0),
  };
  require(all_ones.size() == all_ones_expected.size() &&
          std::equal(all_ones_expected.begin(), all_ones_expected.end(),
                     all_ones.begin()));

  const auto complement_63 = jit_test::materialize_constant_for_testing(
      destination, ~std::uint64_t{63});
  constexpr std::array complement_63_expected{
      move_not_wide(destination, 63, 0),
  };
  require(complement_63.size() == complement_63_expected.size() &&
          std::equal(complement_63_expected.begin(),
                     complement_63_expected.end(), complement_63.begin()));

  const auto sparse_complement = jit_test::materialize_constant_for_testing(
      destination, 0xffff'1234'ffff'5678ULL);
  constexpr std::array sparse_complement_expected{
      move_not_wide(destination, 0xa987, 0),
      move_wide(true, destination, 0x1234, 2),
  };
  require(
      sparse_complement.size() == sparse_complement_expected.size() &&
      std::equal(sparse_complement_expected.begin(),
                 sparse_complement_expected.end(), sparse_complement.begin()));

  const auto tie = jit_test::materialize_constant_for_testing(
      destination, 0xffff'0000'ffff'0000ULL);
  constexpr std::array tie_expected{
      move_wide(false, destination, 0xffff, 1),
      move_wide(true, destination, 0xffff, 3),
  };
  require(tie.size() == tie_expected.size() &&
          std::equal(tie_expected.begin(), tie_expected.end(), tie.begin()));
}

void continued_result_uses_one_upper_half_move() {
  constexpr std::uint64_t pc = 0x5aff'0000;
  constexpr std::array guest{add_x5_one};
  const auto compiled = jit_test::compile_block(pc, guest);
  require(!compiled.guest_illegal);
  require(compiled.guest_instructions == guest.size());
  require(count(hot(compiled), move_wide(false, 0, 1, 2)) == 1);
  require(count(hot(compiled), move_wide(false, 0, 0, 0)) == 0);
}

void dynamic_result_uses_exact_arithmetic_composition() {
  constexpr std::uint64_t pc = 0x5b00'0000;
  constexpr std::array odd_prefix{
      add_x5_one,
      load_pair_q0_q1_x0,
      system_call,
  };
  const auto compiled = jit_test::compile_block(pc, odd_prefix);
  require(!compiled.guest_illegal);
  require(compiled.guest_instructions == odd_prefix.size());
  require(count(compiled.words, add_packed_result) == 1);
  require(count(compiled.words, old_merge_packed_result) == 0);

  constexpr std::array no_prefix{
      load_pair_q0_q1_x0,
      system_call,
  };
  const auto first = jit_test::compile_block(pc, no_prefix);
  require(!first.guest_illegal);
  require(first.guest_instructions == no_prefix.size());
  require(count(first.words, add_packed_result) == 0);
  require(count(first.words, old_merge_packed_result) == 0);
}

void successful_links_do_not_publish_terminal_metadata() {
  constexpr std::uint64_t direct_pc = 0x5b01'0000;
  constexpr std::array direct{0x1400'0001U}; // b pc + 4
  const auto direct_persistent =
      jit_test::compile_linked_block(direct_pc, direct);

  /*
   * These were STRs of target instruction_count and guest_pc through the
   * x19 chain-state pointer.
   */
  require(count(hot(direct_persistent), 0xf900'0a69U) == 0);
  require(count(hot(direct_persistent), 0xf900'0670U) == 0);

  constexpr std::uint64_t pic_pc = 0x5b02'0000;
  constexpr std::array indirect{0xd61f'00a0U}; // br x5
  const auto pic_persistent =
      jit_test::compile_indirect_linked_block(pic_pc, indirect);
  require(count(hot(pic_persistent), 0xf900'0a69U) == 0);
  require(count(hot(pic_persistent), 0xf900'0670U) == 0);
}

void exclusive_counter_loop_retains_one_translated_address() {
  constexpr std::array guest{
      0xc85f'7e80U, // ldxr x0, [x20]
      0x9100'0400U, // add x0, x0, #1
      0xc801'7e80U, // stxr w1, x0, [x20]
      0x35ff'ffa1U, // cbnz w1, pc - 12
  };
  const auto compiled = jit_test::compile_block(0x210174U, guest);
  const auto code = hot(compiled);
  const auto load = std::ranges::find_if(code, [](std::uint32_t word) {
    return (word & ~31U) == 0xc85f'7ea0U;
  });
  const auto store = std::ranges::find_if(code, [](std::uint32_t word) {
    return (word & ~((31U << 16) | 31U)) == 0xc800'7ea0U;
  });
  require(compiled.guest_instructions == guest.size());
  require(compiled.memory_write_mask == (std::uint64_t{1} << 2U));
  require(compiled.direct_memory_write_mask ==
          (std::uint64_t{1} << 2U));
  require(load != code.end() && store != code.end());
  require(std::distance(load, store) == 2);
}

void profiled_exclusive_integer_shapes_remain_native() {
  constexpr std::uint32_t load = 0xc85f'7c23U;  // ldxr x3, [x1]
  constexpr std::uint32_t store = 0xc805'7c23U; // stxr w5, x3, [x1]
  const auto verify = [&](std::span<const std::uint32_t> guest,
                          std::ptrdiff_t native_distance,
                          std::uint64_t store_mask) {
    const auto compiled = jit_test::compile_block(0x5b03'0000U, guest);
    const auto code = hot(compiled);
    const auto native_load =
        std::ranges::find_if(code, [](std::uint32_t word) {
          return (word & ~31U) == 0xc85f'7ea0U;
        });
    const auto native_store =
        std::ranges::find_if(code, [](std::uint32_t word) {
          return (word & ~((31U << 16) | 31U)) == 0xc800'7ea0U;
        });
    require(compiled.guest_instructions == guest.size());
    require(compiled.memory_write_mask == store_mask);
    require(compiled.direct_memory_write_mask == store_mask);
    require(native_load != code.end() && native_store != code.end());
    require(std::distance(native_load, native_store) == native_distance);
  };

  constexpr std::array adjacent{load, store};
  constexpr std::array add_register{
      load,
      0x8b04'0063U, // add x3, x3, x4
      store,
  };
  constexpr std::array logical_register{
      load,
      0xaa04'0063U, // orr x3, x3, x4
      store,
  };
  verify(adjacent, 1, std::uint64_t{1} << 1U);
  verify(add_register, 2, std::uint64_t{1} << 2U);
  verify(logical_register, 2, std::uint64_t{1} << 2U);

  constexpr std::array flag_setting{
      load,
      0xab04'0063U, // adds x3, x3, x4
      store,
  };
  verify(flag_setting, 2, std::uint64_t{1} << 2U);
}

void compare_retry_region_uses_native_exclusives() {
  constexpr std::array guest{
      0xc85f'7c23U, // ldxr x3, [x1]
      0xeb04'007fU, // cmp x3, x4
      0x5400'0061U, // b.ne pc + 12
      0xc805'7c22U, // stxr w5, x2, [x1]
      0x35ff'ff85U, // cbnz w5, pc - 16
  };
  const auto compiled = jit_test::compile_block(0x5b05'0000U, guest);
  const auto code = hot(compiled);
  require(compiled.guest_instructions == guest.size());
  require(compiled.direct_memory_write_mask == (std::uint64_t{1} << 3U));
  require(std::ranges::any_of(code, [](std::uint32_t word) {
    return (word & ~((31U << 16) | 31U)) == 0xc800'7ea0U;
  }));
}

/* Whether `word` is the host's load-exclusive or store-exclusive on x21, the
 * translated address the run keeps. */
[[nodiscard]] bool native_load_exclusive(std::uint32_t word) {
  return (word & 0x3fe0'0000U) == 0x0840'0000U && ((word >> 5) & 31U) == 21U;
}
[[nodiscard]] bool native_store_exclusive(std::uint32_t word) {
  return (word & 0x3fe0'0000U) == 0x0800'0000U && ((word >> 5) & 31U) == 21U;
}
/* A word that touches no memory: data processing (immediate or register) or
 * a conditional branch, the only words a run's interval may hold. */
[[nodiscard]] bool register_only_or_branch(std::uint32_t word) {
  const auto group = (word >> 25) & 15U;
  const bool data_processing = group == 8U || group == 9U || group == 5U ||
                               group == 13U;
  const bool branch = (word & 0xff00'0010U) == 0x5400'0000U ||
                      (word & 0x7e00'0000U) == 0x3400'0000U ||
                      (word & 0x7e00'0000U) == 0x3600'0000U;
  return data_processing || branch;
}

/* The fused run in `compiled`: exactly one host load-exclusive and one
 * store-exclusive, and between them nothing but register work and branches
 * (to the rollback), whatever the variant. Returns the store's position. */
[[nodiscard]] std::size_t
require_fused_interval(const jit_test::CompiledCode &compiled) {
  const auto code = hot(compiled);
  require(std::ranges::count_if(code, native_load_exclusive) == 1);
  require(std::ranges::count_if(code, native_store_exclusive) == 1);
  const auto load = static_cast<std::size_t>(
      std::ranges::find_if(code, native_load_exclusive) - code.begin());
  const auto store = static_cast<std::size_t>(
      std::ranges::find_if(code, native_store_exclusive) - code.begin());
  require(load < store);
  for (std::size_t index = load + 1; index < store; ++index)
    require(register_only_or_branch(code[index]));
  return store;
}

[[nodiscard]] std::ptrdiff_t sign_extend_19(std::uint32_t word) {
  auto value = static_cast<std::int32_t>((word >> 5) & 0x7ffffU);
  if ((value & 0x40000) != 0)
    value -= 0x80000;
  return value;
}

/* After the store, the tracked variant records the store's range only if the
 * host store succeeded: a cbnz on the store's status register guards the
 * record's first word (a store of x21 through the chain's range table), and
 * nothing of the record comes before the store. */
void require_record_after_a_successful_store(
    const jit_test::CompiledCode &compiled, std::size_t store, bool tracked) {
  const auto code = hot(compiled);
  const auto status = (code[store] >> 16) & 31U;
  require(status != 31U);
  constexpr std::uint32_t record_address = 0xf900'0000U | (3U << 5) | 21U;
  const auto is_record = [](std::uint32_t word) {
    return (word & 0xffc0'03ffU) == record_address;
  };
  require(std::none_of(code.begin(), code.begin() + store + 1, is_record));
  const auto after = std::ranges::find_if(code.begin() + store + 1, code.end(),
                                          is_record);
  if (!tracked) {
    require(after == code.end());
    return;
  }
  require(after != code.end());
  const auto cbnz = code[store + 1];
  require((cbnz & 0xff00'001fU) == (0x3500'0000U | status));
  const auto skipped =
      static_cast<std::ptrdiff_t>(sign_extend_19(cbnz));
  require(static_cast<std::ptrdiff_t>(after - code.begin()) <
          static_cast<std::ptrdiff_t>(store + 1) + skipped);
}

void fused_runs_have_one_interval_in_both_variants() {
  struct Shape {
    std::vector<std::uint32_t> guest;
    std::uint64_t pc;
  };
  const std::array<Shape, 4> shapes{{
      {{0xc85f'7c23U, // ldxr x3, [x1]
        0xc805'7c23U, // stxr w5, x3, [x1]
        0x35ff'ffe5U},
       0x5b08'0000U},
      {{0xc85f'7c23U, // ldxr x3, [x1]
        0x9100'0463U, // add x3, x3, #1
        0xc805'7c23U, // stxr w5, x3, [x1]
        0x35ff'ff85U},
       0x5b09'0000U},
      {{0xc85f'7c23U, // ldxr x3, [x1]
        0xeb04'007fU, // cmp x3, x4
        0x5400'0061U, // b.ne pc + 12
        0xc805'7c22U, // stxr w5, x2, [x1]
        0x35ff'ff85U},
       0x5b0a'0000U},
      /* The status is the zero register: it gets a scratch of its own. */
      {{0xc85f'7c23U, // ldxr x3, [x1]
        0x9100'0463U, // add x3, x3, #1
        0xc81f'7c23U, // stxr wzr, x3, [x1]
        0x35ff'ff9fU},
       0x5b0b'0000U},
  }};
  for (const auto &shape : shapes) {
    for (const bool tracked : {false, true}) {
      const auto compiled = tracked
          ? jit_test::compile_tracked_block(shape.pc, shape.guest)
          : jit_test::compile_block(shape.pc, shape.guest);
      require(compiled.guest_instructions == shape.guest.size());
      const auto store = require_fused_interval(compiled);
      require_record_after_a_successful_store(compiled, store, tracked);
      require(compiled.direct_memory_write_mask ==
              (std::uint64_t{1} << (shape.guest.size() - 2U)));
    }
  }
}

void tracked_compare_retry_region_uses_native_exclusives() {
  constexpr std::array guest{
      0xc85f'7c23U, // ldxr x3, [x1]
      0xeb04'007fU, // cmp x3, x4
      0x5400'0061U, // b.ne pc + 12
      0xc805'7c22U, // stxr w5, x2, [x1]
      0x35ff'ff85U, // cbnz w5, pc - 16
  };
  const auto compiled =
      jit_test::compile_tracked_block(0x5b06'0000U, guest);
  require(compiled.guest_instructions == guest.size());
  require(compiled.direct_memory_write_mask == (std::uint64_t{1} << 3U));
  (void)require_fused_interval(compiled);
}

void compare_retry_with_shared_result_and_status_is_fused() {
  constexpr std::array guest{
      0x885f'fc9bU, // ldxr w27, [x4]
      0x6b02'037fU, // cmp w27, w2
      0x5400'0061U, // b.ne pc + 12
      0x881b'fc83U, // stxr w27, w3, [x4]
      0xb5ff'ff9bU, // cbnz x27, pc - 16
  };
  /* The status is the load's destination: the host store writes it, and the
   * guest's register takes the status after the compare has read the load. */
  for (const bool tracked : {false, true}) {
    const auto compiled = tracked
        ? jit_test::compile_tracked_block(0x5b0c'0000U, guest)
        : jit_test::compile_block(0x5b0c'0000U, guest);
    require(compiled.guest_instructions == guest.size());
    const auto store = require_fused_interval(compiled);
    require_record_after_a_successful_store(compiled, store, tracked);
  }
}

/* What a run may not be: a register read before the run writes it, the base
 * written, the status the data or the base, SP or a reserved encoding named,
 * a side exit into the run, more than four words between the pair, or more
 * registers than the pool. Each declines to one semantic instruction. */
void runs_that_cannot_be_fused_decline() {
  constexpr std::uint32_t load = 0xc85f'7c23U;  // ldxr x3, [x1]
  constexpr std::uint32_t store = 0xc805'7c23U; // stxr w5, x3, [x1]
  constexpr std::uint32_t retry = 0x35ff'ff85U;
  const auto declines = [&](std::initializer_list<std::uint32_t> words,
                            std::uint64_t pc) {
    const std::vector<std::uint32_t> guest{words};
    for (const bool tracked : {false, true}) {
      const auto compiled = tracked
          ? jit_test::compile_tracked_block(pc, guest)
          : jit_test::compile_block(pc, guest);
      require(compiled.guest_instructions == 1U);
      require(std::ranges::none_of(hot(compiled), native_store_exclusive));
    }
  };
  declines({load, 0x9100'0442U /* add x2, x2, #1: live-in written */, store,
            retry}, 0x5b10'0000U);
  declines({load, 0x9100'0421U /* add x1, x1, #1: the base */, store, retry},
           0x5b11'0000U);
  declines({load, 0x9100'03e3U /* add x3, sp, #0 */, store, retry},
           0x5b12'0000U);
  declines({load, 0x9100'0463U, 0xc803'7c23U /* stxr w3, x3, [x1] */, retry},
           0x5b13'0000U);
  declines({load, 0x9100'0463U, 0xc801'7c23U /* stxr w1, x3, [x1] */, retry},
           0x5b14'0000U);
  declines({load, 0x5400'006eU /* b.al */, store, retry}, 0x5b15'0000U);
  declines({load, 0x5400'0041U /* b.ne to the store: pc + 8 */, 0xd280'0027U,
            store, retry}, 0x5b16'0000U);
  declines({load, 0x9100'0463U, 0x9100'0463U, 0x9100'0463U, 0x9100'0463U,
            0x9100'0463U, store, retry}, 0x5b17'0000U);
  declines({load, 0x9341'0063U /* a bitfield: not a run word */, store,
            retry}, 0x5b18'0000U);
  declines({load, 0x1200'7c20U /* and w0, w1, #imms=31: reserved */, store,
            retry}, 0x5b19'0000U);
  declines({load, 0x0bc4'0060U /* add w0, w3, w4, shift type 3: reserved */,
            store, retry}, 0x5b1a'0000U);
}

} // namespace

int main() {
  constant_materialization_starts_at_the_first_nonzero_half();
  constant_materialization_uses_inversion_only_when_shorter();
  continued_result_uses_one_upper_half_move();
  dynamic_result_uses_exact_arithmetic_composition();
  successful_links_do_not_publish_terminal_metadata();
  exclusive_counter_loop_retains_one_translated_address();
  profiled_exclusive_integer_shapes_remain_native();
  compare_retry_region_uses_native_exclusives();
  tracked_compare_retry_region_uses_native_exclusives();
  fused_runs_have_one_interval_in_both_variants();
  compare_retry_with_shared_result_and_status_is_fused();
  runs_that_cannot_be_fused_decline();
  return 0;
}
