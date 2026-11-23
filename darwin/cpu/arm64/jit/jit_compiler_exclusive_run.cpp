// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_compiler.hpp"
#include "jit_exclusive_run.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <optional>

namespace uml::cpu::arm64 {

namespace {

/*
 * The host registers a native exclusive run keeps its guest registers in.
 * x8 holds the base address until the translation ends, x21 the translated
 * one, x3 and x4 belong to the tracked store record, and x5-x7, x12, x13 and
 * x17 are the translation's resident bindings; the rest of the registers the
 * run may clobber is this pool, in the order a run takes them.
 */
constexpr std::array<unsigned, 7> shadow_pool{0U, 1U, 2U, 9U, 10U, 11U, 16U};

enum class RunClass : unsigned char {
  move_wide,
  add_sub_immediate,
  add_sub_shifted,
  logical_shifted,
  logical_immediate,
  compare_branch,
  test_branch,
  branch_condition,
};

struct RunOperands final {
  RunClass kind{};
  unsigned rd{31U};
  unsigned rn{31U};
  unsigned rm{31U};
  bool reads_rd{};
  bool reads_rn{};
  bool reads_rm{};
  bool writes_rd{};
  bool reads_flags{};
  bool writes_flags{};
};

/* The registers and flags a middle or side exit of ExclusiveRunWord reads
 * and writes. Register 31 is the zero register in every field a run names. */
[[nodiscard]] RunOperands run_operands(std::uint32_t word) noexcept {
  RunOperands operands;
  operands.rd = word & 31U;
  operands.rn = (word >> 5U) & 31U;
  operands.rm = (word >> 16U) & 31U;
  const unsigned opc = (word >> 29U) & 3U;
  if ((word & 0x1f80'0000U) == 0x1280'0000U) {
    operands.kind = RunClass::move_wide;
    operands.reads_rd = opc == 3U;
    operands.writes_rd = true;
  } else if ((word & 0x1f80'0000U) == 0x1100'0000U) {
    operands.kind = RunClass::add_sub_immediate;
    operands.reads_rn = true;
    operands.writes_rd = true;
    operands.writes_flags = (word & 0x2000'0000U) != 0U;
  } else if ((word & 0x1f20'0000U) == 0x0b00'0000U) {
    operands.kind = RunClass::add_sub_shifted;
    operands.reads_rn = operands.reads_rm = true;
    operands.writes_rd = true;
    operands.writes_flags = (word & 0x2000'0000U) != 0U;
  } else if ((word & 0x1f00'0000U) == 0x0a00'0000U) {
    operands.kind = RunClass::logical_shifted;
    operands.reads_rn = operands.reads_rm = true;
    operands.writes_rd = true;
    operands.writes_flags = opc == 3U;
  } else if ((word & 0x1f80'0000U) == 0x1200'0000U) {
    operands.kind = RunClass::logical_immediate;
    operands.reads_rn = true;
    operands.writes_rd = true;
    operands.writes_flags = opc == 3U;
  } else if ((word & 0x7e00'0000U) == 0x3400'0000U) {
    operands.kind = RunClass::compare_branch;
    operands.reads_rd = true;
  } else if ((word & 0x7e00'0000U) == 0x3600'0000U) {
    operands.kind = RunClass::test_branch;
    operands.reads_rd = true;
  } else {
    operands.kind = RunClass::branch_condition;
    operands.reads_flags = true;
  }
  /* A write to the zero register is no write; a flag-setting form with it as
   * the result (CMP, CMN, TST) sets the flags alone. */
  operands.writes_rd = operands.writes_rd && operands.rd != 31U;
  return operands;
}

/* The word offset a side exit's branch encodes. */
[[nodiscard]] std::int64_t side_exit_words(std::uint32_t word,
                                           RunClass kind) noexcept {
  return kind == RunClass::test_branch
             ? detail::sign_extend((word >> 5U) & 0x3fffU, 14)
             : detail::sign_extend((word >> 5U) & 0x7'ffffU, 19);
}

/* One native exclusive run: the words [0, store_index] of a block, and which
 * host register stands for each guest register it touches. */
struct NativeExclusiveRun final {
  std::size_t store_index{};
  unsigned size{};
  unsigned base{};
  unsigned load_destination{};
  unsigned store_source{};
  unsigned status{};
  /* The guest registers in order of first touch, and their shadows. */
  std::array<unsigned, 32> order{};
  std::size_t touched{};
  std::array<unsigned, 32> host{};
  std::array<bool, 32> live_in{};
  std::array<bool, 32> written{};
  /* The status of a store-exclusive to the zero register is written to a
   * scratch shadow of its own. */
  unsigned zero_status_host{31U};
  bool base_read{};
  bool incoming_flags{};
  bool writes_flags{};
  bool side_exits{};

  [[nodiscard]] unsigned host_of(unsigned guest) const noexcept {
    return guest == 31U ? 31U : host[guest];
  }
};

/*
 * Decides whether words [0, store_index] of `instructions` are a run that can
 * execute between one host load-exclusive and store-exclusive, and says which
 * host register holds each guest register. Everything but the pool is judged
 * on the words alone, so the tracked variant, which compiles the same words,
 * reaches the same decision.
 */
[[nodiscard]] std::optional<NativeExclusiveRun>
analyze_exclusive_run(std::span<const std::uint32_t> instructions,
                      std::size_t store_index, std::uint64_t pc) {
  if (store_index < 1U || store_index > maximum_exclusive_run_middles + 1U ||
      instructions.size() <= store_index)
    return std::nullopt;
  const auto load = instructions[0];
  const auto store = instructions[store_index];
  if (exclusive_run_word_kind(load) != ExclusiveRunWord::load_exclusive ||
      exclusive_run_word_kind(store) != ExclusiveRunWord::store_exclusive)
    return std::nullopt;

  NativeExclusiveRun run;
  run.store_index = store_index;
  run.size = load >> 30U;
  run.base = (load >> 5U) & 31U;
  run.load_destination = load & 31U;
  run.store_source = store & 31U;
  run.status = (store >> 16U) & 31U;
  /* The pair is one access: one size, one base, no offset fields. */
  if ((store >> 30U) != run.size || ((store >> 5U) & 31U) != run.base ||
      ((load >> 16U) & 31U) != 31U || ((load >> 10U) & 31U) != 31U ||
      ((store >> 10U) & 31U) != 31U)
    return std::nullopt;
  /* Rt = Xn and Ws = Rt2/Xn are CONSTRAINED UNPREDICTABLE: the semantic path
   * keeps its behaviour. */
  if (run.load_destination == run.base || run.status == run.store_source ||
      run.status == run.base)
    return std::nullopt;

  std::array<bool, 32> seen{};
  const auto touch = [&](unsigned guest) {
    if (!seen[guest]) {
      seen[guest] = true;
      run.order[run.touched++] = guest;
    }
  };
  /* A register the run reads before it writes keeps its value for the
   * rollback only if the run never writes it before the store. */
  const auto read = [&](unsigned guest) {
    if (guest == 31U)
      return;
    if (!seen[guest])
      run.live_in[guest] = true;
    touch(guest);
    if (guest == run.base)
      run.base_read = true;
  };
  const auto write = [&](unsigned guest) {
    if (guest == 31U)
      return true;
    if (run.live_in[guest])
      return false;
    touch(guest);
    run.written[guest] = true;
    return true;
  };
  /* The base is the first shadow. SP is the base of no run that names it. */
  if (run.base != 31U) {
    run.live_in[run.base] = true;
    touch(run.base);
  }
  if (!write(run.load_destination))
    return std::nullopt;

  const std::uint64_t last = pc + store_index * sizeof(std::uint32_t);
  bool flags_written = false;
  for (std::size_t index = 1; index < store_index; ++index) {
    const auto word = instructions[index];
    const auto kind = exclusive_run_word_kind(word);
    if (kind != ExclusiveRunWord::middle &&
        kind != ExclusiveRunWord::side_exit)
      return std::nullopt;
    const auto operands = run_operands(word);
    if (operands.reads_flags && !flags_written)
      run.incoming_flags = true;
    if (kind == ExclusiveRunWord::side_exit) {
      const auto here = pc + index * sizeof(std::uint32_t);
      const auto target =
          here + static_cast<std::uint64_t>(
                     side_exit_words(word, operands.kind) * 4);
      if (target >= pc && target <= last)
        return std::nullopt;
      run.side_exits = true;
    }
    if (operands.reads_rd)
      read(operands.rd);
    if (operands.reads_rn)
      read(operands.rn);
    if (operands.reads_rm)
      read(operands.rm);
    if (operands.writes_rd && !write(operands.rd))
      return std::nullopt;
    if (operands.writes_flags) {
      flags_written = true;
      run.writes_flags = true;
    }
  }
  read(run.store_source);
  /* The status is written by the store: a register the run read is allowed. */
  const bool zero_status = run.status == 31U;
  if (!zero_status) {
    touch(run.status);
    run.written[run.status] = true;
  }

  const std::size_t needed = run.touched + (zero_status ? 1U : 0U);
  if (needed > shadow_pool.size())
    return std::nullopt;
  /* The base takes the first shadow, then the live-ins in order of first
   * touch, then the rest in that order, and a zero-register status the last. */
  std::size_t next = 0;
  const auto assign = [&](unsigned guest) {
    run.host[guest] = shadow_pool[next++];
  };
  if (run.base != 31U)
    assign(run.base);
  for (std::size_t index = 0; index < run.touched; ++index)
    if (run.live_in[run.order[index]] && run.order[index] != run.base)
      assign(run.order[index]);
  for (std::size_t index = 0; index < run.touched; ++index)
    if (!run.live_in[run.order[index]])
      assign(run.order[index]);
  if (zero_status)
    run.zero_status_host = shadow_pool[next];
  return run;
}

/* The index of the store-exclusive that ends the run at the front of
 * `instructions`: the first word that is one, with only middles and side
 * exits before it and no more than four of them. Zero if there is none. */
[[nodiscard]] std::size_t
find_exclusive_run_store(std::span<const std::uint32_t> instructions) {
  if (instructions.size() < 2U ||
      exclusive_run_word_kind(instructions[0]) !=
          ExclusiveRunWord::load_exclusive)
    return 0;
  const auto limit = std::min(instructions.size(),
                              maximum_exclusive_run_middles + 2U);
  for (std::size_t index = 1; index < limit; ++index) {
    const auto kind = exclusive_run_word_kind(instructions[index]);
    if (kind == ExclusiveRunWord::store_exclusive)
      return index;
    if (kind != ExclusiveRunWord::middle &&
        kind != ExclusiveRunWord::side_exit)
      return 0;
  }
  return 0;
}

/* The word with its register fields replaced by the run's host registers. */
[[nodiscard]] std::uint32_t
shadow_encoding(std::uint32_t word, const RunOperands &operands,
                const NativeExclusiveRun &run) noexcept {
  const auto field = [&](unsigned shift, unsigned guest) {
    word = (word & ~(31U << shift)) | (run.host_of(guest) << shift);
  };
  field(0U, operands.rd);
  if (operands.kind == RunClass::move_wide ||
      operands.kind == RunClass::compare_branch ||
      operands.kind == RunClass::test_branch ||
      operands.kind == RunClass::branch_condition)
    return word;
  field(5U, operands.rn);
  if (operands.kind == RunClass::add_sub_shifted ||
      operands.kind == RunClass::logical_shifted)
    field(16U, operands.rm);
  return word;
}

} // namespace

/*
 * A load-exclusive at a block's first word, up to four register-only words,
 * and the store-exclusive that follows, run on the host's own monitor:
 *
 *   ldxr Rt, [Xn]; M1 .. Mk; stxr Ws, Rd, [Xn]
 *
 * with the conditional branches among the M's leaving the run. Between the
 * host load and store the code holds only the run's own re-encoded words and
 * branches to its rollback; every register is a scratch copy of the guest
 * one, loaded before the load-exclusive and written back after the store, so
 * the interval touches no memory. A side exit discards the attempt and has
 * the load-exclusive re-executed semantically; see the exclusive_rollback arm.
 *
 * Both variants compile the same words to the same shape, differing only in
 * the tracked one's record of the store, taken after a store that succeeded.
 */
[[nodiscard]] std::size_t JitCompiler::try_emit_native_exclusive_run(
    std::span<const std::uint32_t> instructions, std::uint64_t pc) {
  if (count_ != 0U || !no_gprs_bound() || instructions.size() < 2U)
    return 0;
  const auto store_index = find_exclusive_run_store(instructions);
  if (store_index == 0U)
    return 0;
  const auto planned = analyze_exclusive_run(instructions, store_index, pc);
  if (!planned)
    return 0;
  const auto &run = *planned;
  const auto load_instruction = instructions[0];
  const auto store_instruction = instructions[store_index];
  const std::size_t bytes = std::size_t{1} << run.size;

  /* Every exit is made here, at the run's first word, before any binding or
   * flag change: its arm publishes the state at the load, so a discarded
   * attempt retires nothing and leaves no trace. */
  const auto slow = cold_exit(load_instruction, pc,
                              ColdPath::Action::deferred_exclusive_slow);
  const auto fault =
      cold_exit(load_instruction, pc, ColdPath::Action::memory_fault);
  const auto rollback = run.side_exits
      ? cold_exit(load_instruction, pc, ColdPath::Action::exclusive_rollback)
      : slow;

  load_gpr(8, run.base, true);
  if (run.base == 31U)
    emit_misalignment_check(8, 16, slow);
  emit_untag_data_address(8);
  emit_misalignment_check(8, bytes, slow);

  /*
   * Resolve both permissions before establishing the host monitor. x21 is
   * host-owned chain state, so it keeps the translated write address across
   * the run; the selector is restored after the store, before the
   * terminating branch can use it.
   */
  emit_translate(8, Access::write, slow, slow, bytes);
  emitter_.move_x(21, 8);
  load_gpr(8, run.base, true);
  emit_untag_data_address(8);
  emit_translate(8, Access::read, slow, fault, bytes);
  emitter_.compare_register(8, 21);
  emitter_.branch_condition(slow, 1); // NE

  /* The translation's compare clobbered the host flags. Load the guest's
   * before any shadow takes x11, which loading them uses. */
  if (run.incoming_flags)
    load_flags();
  for (std::size_t index = 0; index < run.touched; ++index) {
    const auto guest = run.order[index];
    if (run.live_in[guest] && (guest != run.base || run.base_read))
      emitter_.load_x(run.host[guest], 15, register_offset(guest, false));
  }

  /* The interval. */
  auto native_load = load_instruction &
      ~((31U << 16) | (31U << 10) | (31U << 5) | 31U);
  native_load |= (31U << 16) | (31U << 10) | (21U << 5) |
                 run.host_of(run.load_destination);
  emitter_.emit(native_load);
  for (std::size_t index = 1; index < store_index; ++index) {
    const auto word = instructions[index];
    const auto operands = run_operands(word);
    const auto encoded = shadow_encoding(word, operands, run);
    switch (operands.kind) {
    case RunClass::compare_branch:
      emitter_.compare_zero(rollback, run.host_of(operands.rd),
                            (word >> 31U) != 0U, (word & 0x0100'0000U) != 0U);
      break;
    case RunClass::test_branch:
      emitter_.test_zero(rollback, run.host_of(operands.rd),
                         ((word >> 26U) & 32U) | ((word >> 19U) & 31U),
                         (word & 0x0100'0000U) != 0U);
      break;
    case RunClass::branch_condition:
      emitter_.branch_condition(rollback, word & 15U);
      break;
    default:
      emitter_.emit(encoded);
      break;
    }
  }
  const unsigned status_host = run.status == 31U
                                   ? run.zero_status_host
                                   : run.host_of(run.status);
  auto native_store = store_instruction &
      ~((31U << 16) | (31U << 10) | (31U << 5) | 31U);
  native_store |= (status_host << 16) | (31U << 10) | (21U << 5) |
                  run.host_of(run.store_source);
  count_ += store_index;
  mark_direct_memory_write();
  emitter_.emit(native_store);

  /* After the store: record it only where it succeeded, publish the shadows
   * as the guest's registers, and end the software monitor. */
  if (track_direct_stores_) {
    const auto stored = emitter_.make_label();
    emitter_.compare_zero(stored, status_host, false, true);
    track_direct_store(21, bytes, store_index);
    emitter_.bind(stored);
  }
  for (std::size_t index = 0; index < run.touched; ++index) {
    const auto guest = run.order[index];
    if (!run.written[guest])
      continue;
    const auto destination =
        write_native_gpr(guest, false, false, run.host[guest]);
    if (destination.host != run.host[guest])
      emitter_.move_x(destination.host, run.host[guest]);
    commit_native_gpr(destination, guest, false);
  }
  if (run.writes_flags)
    store_flags();
  clear_local_exclusive_monitor();
  emitter_.load_b(21, 19, offsetof(JitChainState, link_selector));

  ++count_;
  return store_index + 1U;
}

} // namespace uml::cpu::arm64
