// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

/*
 * A64 words for the tests of the native exclusive runs, and the hot lock
 * sequences of the Android runtime in the words ART emits them. The offsets
 * of branches are in words from the branch.
 */
namespace uml::emulator::tests::a64 {

constexpr std::uint32_t system_call = 0xd400'0001U;
constexpr std::uint32_t nop = 0xd503'201fU;
constexpr std::uint32_t ret = 0xd65f'03c0U;

[[nodiscard]] constexpr std::uint32_t
load_exclusive(unsigned size, bool acquire, unsigned rt, unsigned rn) noexcept {
  return 0x085f'7c00U | (size << 30U) | (acquire ? 0x8000U : 0U) |
         (rn << 5U) | rt;
}

[[nodiscard]] constexpr std::uint32_t
store_exclusive(unsigned size, bool release, unsigned rs, unsigned rt,
                unsigned rn) noexcept {
  return 0x0800'7c00U | (size << 30U) | (rs << 16U) |
         (release ? 0x8000U : 0U) | (rn << 5U) | rt;
}

[[nodiscard]] constexpr std::uint32_t
move_wide(bool wide, unsigned opc, unsigned rd, unsigned imm16,
          unsigned hw = 0) noexcept {
  return (wide ? 0x8000'0000U : 0U) | (opc << 29U) | 0x1280'0000U |
         (hw << 21U) | (imm16 << 5U) | rd;
}
[[nodiscard]] constexpr std::uint32_t movz(bool wide, unsigned rd,
                                           unsigned imm16,
                                           unsigned hw = 0) noexcept {
  return move_wide(wide, 2, rd, imm16, hw);
}
[[nodiscard]] constexpr std::uint32_t movk(bool wide, unsigned rd,
                                           unsigned imm16,
                                           unsigned hw = 0) noexcept {
  return move_wide(wide, 3, rd, imm16, hw);
}
[[nodiscard]] constexpr std::uint32_t movn(bool wide, unsigned rd,
                                           unsigned imm16,
                                           unsigned hw = 0) noexcept {
  return move_wide(wide, 0, rd, imm16, hw);
}

/* ADD (op = 0) and SUB (op = 1), with S for the flag-setting forms. */
[[nodiscard]] constexpr std::uint32_t
add_sub_immediate(bool wide, bool sub, bool flags, unsigned rd, unsigned rn,
                  unsigned imm12) noexcept {
  return (wide ? 0x8000'0000U : 0U) | (sub ? 0x4000'0000U : 0U) |
         (flags ? 0x2000'0000U : 0U) | 0x1100'0000U | (imm12 << 10U) |
         (rn << 5U) | rd;
}
[[nodiscard]] constexpr std::uint32_t
add_sub_register(bool wide, bool sub, bool flags, unsigned rd, unsigned rn,
                 unsigned rm, unsigned shift = 0, unsigned amount = 0) noexcept {
  return (wide ? 0x8000'0000U : 0U) | (sub ? 0x4000'0000U : 0U) |
         (flags ? 0x2000'0000U : 0U) | 0x0b00'0000U | (shift << 22U) |
         (rm << 16U) | (amount << 10U) | (rn << 5U) | rd;
}
[[nodiscard]] constexpr std::uint32_t cmp(bool wide, unsigned rn,
                                          unsigned rm) noexcept {
  return add_sub_register(wide, true, true, 31, rn, rm);
}

/* opc: 0 AND, 1 ORR, 2 EOR, 3 ANDS; invert selects BIC, ORN, EON, BICS. */
[[nodiscard]] constexpr std::uint32_t
logical_register(bool wide, unsigned opc, bool invert, unsigned rd,
                 unsigned rn, unsigned rm, unsigned shift = 0,
                 unsigned amount = 0) noexcept {
  return (wide ? 0x8000'0000U : 0U) | (opc << 29U) | 0x0a00'0000U |
         (shift << 22U) | (invert ? 0x0020'0000U : 0U) | (rm << 16U) |
         (amount << 10U) | (rn << 5U) | rd;
}
[[nodiscard]] constexpr std::uint32_t
logical_immediate(bool wide, unsigned opc, unsigned rd, unsigned rn,
                  unsigned n, unsigned immr, unsigned imms) noexcept {
  return (wide ? 0x8000'0000U : 0U) | (opc << 29U) | 0x1200'0000U |
         (n << 22U) | (immr << 16U) | (imms << 10U) | (rn << 5U) | rd;
}
/* tst Wn, #0xcfffffff: ANDS WZR with a run of 30 ones rotated right by 2. */
[[nodiscard]] constexpr std::uint32_t tst_w_cfffffff(unsigned rn) noexcept {
  return logical_immediate(false, 3, 31, rn, 0, 2, 29);
}

[[nodiscard]] constexpr std::uint32_t
compare_branch(bool wide, bool nonzero, unsigned rt, int words) noexcept {
  return (wide ? 0x8000'0000U : 0U) | 0x3400'0000U |
         (nonzero ? 0x0100'0000U : 0U) |
         ((static_cast<std::uint32_t>(words) & 0x7ffffU) << 5U) | rt;
}
[[nodiscard]] constexpr std::uint32_t test_branch(bool nonzero, unsigned rt,
                                                  unsigned bit,
                                                  int words) noexcept {
  return ((bit >> 5U) << 31U) | 0x3600'0000U |
         (nonzero ? 0x0100'0000U : 0U) | ((bit & 31U) << 19U) |
         ((static_cast<std::uint32_t>(words) & 0x3fffU) << 5U) | rt;
}
constexpr unsigned eq = 0, ne = 1, cs = 2, cc = 3, mi = 4, pl = 5, vs = 6,
                   vc = 7, hi = 8, ls = 9, ge = 10, lt = 11, gt = 12, le = 13,
                   al = 14;
[[nodiscard]] constexpr std::uint32_t branch_condition(unsigned condition,
                                                       int words) noexcept {
  return 0x5400'0000U |
         ((static_cast<std::uint32_t>(words) & 0x7ffffU) << 5U) | condition;
}
[[nodiscard]] constexpr std::uint32_t branch(int words) noexcept {
  return 0x1400'0000U | (static_cast<std::uint32_t>(words) & 0x03ff'ffffU);
}
/* An unrelated load, a word no run takes. */
[[nodiscard]] constexpr std::uint32_t load_x(unsigned rt,
                                             unsigned rn) noexcept {
  return 0xf940'0000U | (rn << 5U) | rt;
}

/*
 * The hot lock sequences of the Android runtime (ART's JNI transition stubs,
 * its compare-and-swap intrinsic, and the lock and unlock fast paths of
 * libart): the words from the load-exclusive through the retry, as each
 * appears with its branches aimed at `out` words past the load and the retry
 * at the load. `slow` names where the side exits go: after the run.
 */
struct Shape final {
  const char *name;
  std::array<std::uint32_t, 7> words;
  std::size_t length;      // words, load through retry
  std::size_t store_index; // the store-exclusive's word
  unsigned size;           // log2 of the bytes
  unsigned base;           // guest register of the address
};

constexpr int out = 12; // words past the load: after every shape's retry

/* ldxr w16,[x19]; mov w17,#0x5c000000; cbnz w16,slow;
 * stlxr w16,w17,[x19]; cbnz w16,retry */
constexpr Shape shape_j1{"J1",
                   {load_exclusive(2, false, 16, 19),
                    movz(false, 17, 0x5c00, 1),
                    compare_branch(false, true, 16, out - 2),
                    store_exclusive(2, true, 16, 17, 19),
                    compare_branch(false, true, 16, -4), 0, 0},
                   5, 3, 2, 19};
/* ldaxr w16,[x19]; mov w17,#0x5c000000; cmp w16,w17; b.ne slow;
 * stxr w16,wzr,[x19]; cbnz w16,retry */
constexpr Shape shape_j2{"J2",
                   {load_exclusive(2, true, 16, 19),
                    movz(false, 17, 0x5c00, 1), cmp(false, 16, 17),
                    branch_condition(ne, out - 3),
                    store_exclusive(2, false, 16, 31, 19),
                    compare_branch(false, true, 16, -5), 0},
                   6, 4, 2, 19};
/* ldaxr w17,[x16]; cmp w17,w2; b.ne out; stlxr w17,w0,[x16];
 * cbnz w17,retry */
constexpr Shape shape_c1_w{"C1 (W)",
                     {load_exclusive(2, true, 17, 16), cmp(false, 17, 2),
                      branch_condition(ne, out - 2),
                      store_exclusive(2, true, 17, 0, 16),
                      compare_branch(false, true, 17, -4), 0, 0},
                     5, 3, 2, 16};
constexpr Shape shape_c1_x{"C1 (X)",
                     {load_exclusive(3, true, 17, 16), cmp(true, 17, 2),
                      branch_condition(ne, out - 2),
                      store_exclusive(3, true, 17, 0, 16),
                      compare_branch(false, true, 17, -4), 0, 0},
                     5, 3, 3, 16};
/* ldaxr w10,[x8]; eor w11,w10,w9; tst w10,#0xcfffffff; b.ne 2f;
 * stxr w10,w11,[x8]; cbnz w10,1b */
constexpr Shape shape_l1{"L1",
                   {load_exclusive(2, true, 10, 8),
                    logical_register(false, 2, false, 11, 10, 9),
                    tst_w_cfffffff(10), branch_condition(ne, out - 3),
                    store_exclusive(2, false, 10, 11, 8),
                    compare_branch(false, true, 10, -5), 0},
                   6, 4, 2, 8};
/* ldxr w10,[x8]; eor w11,w10,w9; tst w11,#0xcfffffff; b.ne 2f;
 * stlxr w10,w11,[x8]; cbnz w10,1b */
constexpr Shape shape_u1{"U1",
                   {load_exclusive(2, false, 10, 8),
                    logical_register(false, 2, false, 11, 10, 9),
                    tst_w_cfffffff(11), branch_condition(ne, out - 3),
                    store_exclusive(2, true, 10, 11, 8),
                    compare_branch(false, true, 10, -5), 0},
                   6, 4, 2, 8};

constexpr std::array<const Shape *, 6> hot_shapes{
    &shape_j1, &shape_j2, &shape_c1_w, &shape_c1_x, &shape_l1, &shape_u1};

} // namespace uml::emulator::tests::a64
