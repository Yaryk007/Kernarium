// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_exclusive_run_support.hpp"
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "cpu/arm64/jit/jit_cache_geometry.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <span>
#include <utility>
#include <vector>

using namespace uml::emulator::tests;
namespace arm64 = uml::cpu::arm64;
namespace jit_test = uml::cpu::arm64::jit::testing;
using ::uml::cpu::EngineEvent;

/*
 * Native lowering of the lock sequences, through the executor: each hot
 * sequence of the Android runtime runs fused in both variants of its block,
 * leaves by a side exit as if the pair had never started, and yields the
 * state the semantic path yields. A fused run is told by one run_block
 * retiring the whole sequence and leaving no reservation behind; a declined
 * one retires its load alone and leaves one armed.
 */

namespace {

using namespace a64;

constexpr auto page = static_cast<std::uint64_t>(DARWIN_UML_GUEST_PAGE_SIZE);
constexpr std::uint64_t holder_offset = 0x300; // code offset of the holder's load

struct Reg final {
  unsigned index;
  std::uint64_t value;
};

/*
 * A mapped guest: code at the start of its first page, data on the next.
 * With `hold` set, a second vCPU holds a software reservation on a granule
 * of its own for as long as the machine lives, so the block of the executor
 * under test runs in its tracked variant.
 */
class Machine final {
public:
  Machine(unsigned long generation, std::uint64_t base, bool hold,
          std::size_t pages = 4)
      : backing_{pages}, mapping_{generation, base, backing_, pages},
        generation_{generation}, base_{base}, hold_{hold} {
    backing_.put32(holder_offset, load_exclusive(3, false, 3, 7));
    backing_.put32(holder_offset + 4, system_call);
  }

  void code(std::size_t offset, std::span<const std::uint32_t> words) {
    for (std::size_t index = 0; index < words.size(); ++index)
      backing_.put32(offset + index * 4U, words[index]);
  }
  void code(std::size_t offset, std::initializer_list<std::uint32_t> words) {
    code(offset, std::span{words.begin(), words.size()});
  }
  [[nodiscard]] std::uint64_t base() const { return base_; }
  [[nodiscard]] std::uint64_t data() const { return base_ + page; }
  void put(std::uint64_t address, std::uint64_t value) {
    backing_.put64(address - base_, value);
  }
  [[nodiscard]] std::uint64_t get(std::uint64_t address) const {
    return backing_.get64(address - base_);
  }
  [[nodiscard]] std::uint64_t get32(std::uint64_t address) const {
    return static_cast<std::uint32_t>(backing_.get64(address - base_));
  }

  /* Takes the holder's reservation (when asked) and begins the executor. */
  void begin() {
    if (hold_) {
      holder_cpu_.pc = base_ + holder_offset;
      holder_cpu_.registers[7] = base_ + 3U * page + 0x200U;
      require(holder_.begin_execution(generation_, holder_cpu_.pc));
      std::size_t executed = 0;
      require(holder_.run_block(holder_cpu_, memory_, generation_, executed) ==
              EngineEvent::continued);
      require(executed == 1U && holder_cpu_.exclusive_size == 8U);
    }
    require(executor_.begin_execution(generation_, base_));
  }
  void end() {
    executor_.end_execution();
    if (hold_)
      holder_.end_execution();
  }

  /* One run_block from cpu.pc. */
  EngineEvent block(arm64::CpuState &cpu, std::size_t &executed) {
    executed = 0;
    return executor_.run_block(cpu, memory_, generation_, executed);
  }
  /* Blocks until the guest leaves for something but `continued`, adding up
   * what they retire. */
  EngineEvent run(arm64::CpuState &cpu, std::size_t &retired,
                  unsigned limit = 64) {
    retired = 0;
    auto event = EngineEvent::continued;
    for (unsigned attempt = 0;
         attempt < limit && event == EngineEvent::continued; ++attempt) {
      std::size_t executed = 0;
      event = block(cpu, executed);
      retired += executed;
    }
    return event;
  }
  arm64::JitExecutor &executor() { return executor_; }
  [[nodiscard]] unsigned long generation() const { return generation_; }
  [[nodiscard]] JitBacking &backing() { return backing_; }

private:
  JitBacking backing_;
  JitMapping mapping_;
  unsigned long generation_;
  std::uint64_t base_;
  bool hold_;
  arm64::JitExecutor executor_;
  arm64::JitExecutor holder_;
  uml::memory::StaticGuestMemory memory_;
  arm64::CpuState holder_cpu_{};
};

[[nodiscard]] arm64::CpuState
start(std::uint64_t pc, std::initializer_list<Reg> registers) {
  arm64::CpuState cpu{};
  cpu.pc = pc;
  for (const auto &reg : registers)
    cpu.registers[reg.index] = reg.value;
  return cpu;
}

/* The hot sequences with the state each is run on, and what its success and
 * its exit leave behind. */
struct Case final {
  const Shape *shape;
  std::uint64_t memory_ok;
  std::uint64_t memory_exit;
  std::vector<Reg> registers; // besides the address register
  /* A register that differs on the exit path, where the memory alone does not
   * make the branch leave. */
  std::vector<Reg> exit_registers;
  std::uint64_t memory_after;
  std::vector<Reg> after;
  std::uint32_t nzcv_after;
  unsigned rt;
};

constexpr std::uint64_t marker = 0x5c00'0000U;

[[nodiscard]] std::vector<Case> cases() {
  return {
      {&shape_j1, 0, 1, {}, {}, marker, {{16, 0}, {17, marker}}, 0, 16},
      {&shape_j2, marker, 0x1234, {}, {}, 0, {{16, 0}, {17, marker}}, 0x6000'0000U,
       16},
      {&shape_c1_w, 5, 6, {{2, 5}, {0, 9}}, {}, 9, {{17, 0}, {0, 9}}, 0x6000'0000U,
       17},
      {&shape_c1_x, 0x1'0000'0005ULL, 0x2'0000'0005ULL,
       {{2, 0x1'0000'0005ULL}, {0, 9}}, {}, 9, {{17, 0}, {0, 9}},
       0x6000'0000U, 17},
      {&shape_l1, 0, 1, {{9, 7}}, {}, 7, {{10, 0}, {11, 7}}, 0x4000'0000U, 10},
      {&shape_u1, 5, 5, {{9, 5}}, {{9, 0x1000'0000U}}, 0, {{10, 0}, {11, 0}},
       0x4000'0000U, 10},
  };
}

void build(Machine &machine, const Case &c, std::uint64_t memory_value) {
  std::vector<std::uint32_t> words(c.shape->words.begin(),
                                   c.shape->words.begin() + c.shape->length);
  words.resize(16, system_call);
  machine.code(0, words);
  machine.put(machine.data(), memory_value);
}

[[nodiscard]] arm64::CpuState initial(const Machine &machine, const Case &c,
                                      bool exit_path) {
  auto cpu = start(machine.base(), {});
  cpu.registers[c.shape->base] = machine.data();
  for (const auto &reg : c.registers)
    cpu.registers[reg.index] = reg.value;
  if (exit_path)
    for (const auto &reg : c.exit_registers)
      cpu.registers[reg.index] = reg.value;
  return cpu;
}

[[nodiscard]] std::uint64_t mask(unsigned size) {
  return size == 3U ? ~std::uint64_t{} : (std::uint64_t{1} << (8U << size)) - 1U;
}

/* The success path: one block retires the whole sequence, leaves no
 * reservation, and the guest ends with the state the semantic path gives. */
void success_path(Machine &machine, const Case &c) {
  build(machine, c, c.memory_ok);
  auto cpu = initial(machine, c, false);
  std::size_t executed = 0;
  const auto first = machine.block(cpu, executed);
  require(first == EngineEvent::continued || first == EngineEvent::system_call);
  require(executed >= c.shape->length);
  require(cpu.exclusive_size == 0U);
  std::size_t retired = 0;
  if (first == EngineEvent::continued)
    require(machine.run(cpu, retired) == EngineEvent::system_call);
  for (const auto &reg : c.after)
    require(cpu.registers[reg.index] == reg.value);
  require(cpu.nzcv == c.nzcv_after);
  const auto stored = c.shape->size == 3U ? machine.get(machine.data())
                                          : machine.get32(machine.data());
  require(stored == (c.memory_after & mask(c.shape->size)));
  require(cpu.exclusive_size == 0U);
}

/* The exit path on the translation the success path left: the attempt is
 * discarded, the load re-executes alone and arms a reservation, and nothing
 * else of the guest's state differs from the state before the run. */
void exit_path(Machine &machine, const Case &c) {
  machine.put(machine.data(), c.memory_exit);
  const auto before = initial(machine, c, true);
  auto cpu = before;
  std::size_t executed = 0;
  require(machine.block(cpu, executed) == EngineEvent::continued);
  require(executed == 1U && cpu.pc == machine.base() + 4U);
  require(cpu.exclusive_size == (std::size_t{1} << c.shape->size));
  require(cpu.nzcv == before.nzcv);
  for (unsigned index = 0; index < 31U; ++index) {
    if (index == c.rt) {
      require(cpu.registers[index] == (c.memory_exit & mask(c.shape->size)));
    } else {
      require(cpu.registers[index] == before.registers[index]);
    }
  }
  /* The guest takes the branch the loaded value selects and reaches the end
   * without a store. */
  std::size_t retired = 0;
  require(machine.run(cpu, retired) == EngineEvent::system_call);
  const auto stored = c.shape->size == 3U ? machine.get(machine.data())
                                          : machine.get32(machine.data());
  require(stored == (c.memory_exit & mask(c.shape->size)));
}

void hot_shapes_fuse_and_roll_back(bool tracked) {
  unsigned long generation = tracked ? 10'420U : 10'400U;
  for (const auto &c : cases()) {
    const auto base = std::uint64_t{0x260'0000U} + 0x20'0000U * (generation % 7U);
    Machine machine{generation++, base, tracked};
    machine.begin();
    success_path(machine, c);
    exit_path(machine, c);
    /* The translation serves both again: a second success, a second exit. */
    success_path(machine, c);
    exit_path(machine, c);
    machine.end();
  }
}

/*
 * The base is a register of the sequence ART gives, but an interior entry
 * starts an ordinary block: the store-exclusive there fails without a
 * reservation and succeeds with one the guest took apart from it.
 */
void interior_store_entries_are_ordinary_code(bool tracked) {
  unsigned long generation = tracked ? 10'450U : 10'440U;
  for (const auto &c : cases()) {
    const auto base = std::uint64_t{0x2a0'0000U} + 0x20'0000U * (generation % 5U);
    Machine machine{generation++, base, tracked};
    machine.code(0x200, {load_exclusive(c.shape->size, false, 5,
                                        c.shape->base),
                         system_call});
    machine.begin();
    success_path(machine, c); // the translation exists, warm
    const auto store_pc = machine.base() + c.shape->store_index * 4U;
    const auto store_rt = c.shape->words[c.shape->store_index] & 31U;
    const auto status = (c.shape->words[c.shape->store_index] >> 16) & 31U;

    /* No reservation: it fails, memory untouched. */
    machine.put(machine.data(), c.memory_ok);
    auto cpu = initial(machine, c, false);
    cpu.registers[store_rt != 31U ? store_rt : 0] = 0x77U;
    cpu.pc = store_pc;
    std::size_t executed = 0;
    require(machine.block(cpu, executed) == EngineEvent::continued);
    require(status == 31U || cpu.registers[status] == 1U);
    require(machine.get(machine.data()) == c.memory_ok);

    /* A reservation taken by a load of its own: it succeeds. */
    cpu = initial(machine, c, false);
    cpu.registers[store_rt != 31U ? store_rt : 0] = 0x77U;
    cpu.pc = machine.base() + 0x200U;
    require(machine.block(cpu, executed) == EngineEvent::continued);
    require(cpu.exclusive_size == (std::size_t{1} << c.shape->size));
    cpu.pc = store_pc;
    require(machine.block(cpu, executed) == EngineEvent::continued);
    require(status == 31U || cpu.registers[status] == 0U);
    const auto expected = store_rt == 31U ? 0U : 0x77U;
    const auto stored = c.shape->size == 3U ? machine.get(machine.data())
                                            : machine.get32(machine.data());
    require(stored == expected);
    machine.end();
  }
}

/*
 * A run that leaves before its store when the pair is rolled back, then loops
 * through a store-exclusive of its own with no new load: the recursion tail of
 * a thin lock. Clearing the reservation and continuing would fail that store
 * for ever; the rolled-back load re-arms it.
 */
void recursive_lock_makes_progress(bool tracked) {
  Machine machine{tracked ? 10'461U : 10'460U, 0x2e0'0000U, tracked};
  machine.code(0, {load_exclusive(2, true, 10, 8),
                   logical_register(false, 2, false, 11, 10, 9),
                   tst_w_cfffffff(10), branch_condition(ne, 4),
                   store_exclusive(2, false, 10, 11, 8),
                   compare_branch(false, true, 10, -5), system_call,
                   /* 2: the owner re-enters: its count goes up by one */
                   add_sub_immediate(false, false, false, 11, 10, 0x10) |
                       (1U << 22U),
                   store_exclusive(2, false, 12, 11, 8),
                   compare_branch(false, true, 12, -9), system_call});
  machine.put(machine.data(), 1U); // owned by thread 1
  machine.begin();
  auto cpu = start(machine.base(), {{8, machine.data()}, {9, 1U}});
  std::size_t retired = 0;
  require(machine.run(cpu, retired) == EngineEvent::system_call);
  require(machine.get32(machine.data()) == 0x1'0001U);
  require(cpu.registers[12] == 0U && retired < 40U);
  /* The unowned case takes the fast path on the same code. */
  machine.put(machine.data(), 0U);
  cpu = start(machine.base(), {{8, machine.data()}, {9, 1U}});
  require(machine.run(cpu, retired) == EngineEvent::system_call);
  require(machine.get32(machine.data()) == 1U && cpu.registers[10] == 0U);
  machine.end();
}

/*
 * The incoming flags: the run branches on Z before any of its words sets it,
 * and the run touches every register of the pool, so the load of the flags
 * and the registers' copies must not share a host register.
 */
constexpr std::array<std::uint32_t, 7> pressure_words{
    load_exclusive(2, false, 0, 1),
    branch_condition(eq, 6),
    add_sub_register(false, false, false, 0, 3, 4),
    logical_register(false, 2, false, 0, 0, 6),
    logical_register(false, 1, false, 0, 0, 7),
    store_exclusive(2, false, 0, 8, 1),
    compare_branch(false, true, 0, -6),
};

void incoming_flags_under_scratch_pressure(bool tracked) {
  /* The emitted code: x11 is the stored data, loaded after the flags. */
  const std::array<std::uint32_t, 8> guest{
      pressure_words[0], pressure_words[1], pressure_words[2],
      pressure_words[3], pressure_words[4], pressure_words[5],
      pressure_words[6], system_call};
  const auto compiled = tracked ? jit_test::compile_tracked_block(0x2f00'0000U, guest)
                                : jit_test::compile_block(0x2f00'0000U, guest);
  const auto code = std::span{compiled.words.data(), compiled.hot_words};
  constexpr std::uint32_t msr_nzcv_x11 = 0xd51b'420bU;
  const auto flags = std::find(code.begin(), code.end(), msr_nzcv_x11);
  require(flags != code.end());
  const auto data_offset =
      offsetof(arm64::CpuState, registers) + 8U * sizeof(std::uint64_t);
  const std::uint32_t load_x11 =
      0xf940'0000U | (static_cast<std::uint32_t>(data_offset / 8U) << 10U) |
      (15U << 5U) | 11U;
  const auto copy = std::find(flags, code.end(), load_x11);
  require(copy != code.end());
  const auto store = std::find_if(copy, code.end(), [](std::uint32_t word) {
    return (word & 0x3fe0'0000U) == 0x0800'0000U && ((word >> 5) & 31U) == 21U;
  });
  require(store != code.end() && (*store & 31U) == 11U);

  Machine machine{static_cast<unsigned long>(tracked ? 10'471U : 10'470U),
                  0x2f0'0000U, tracked};
  machine.code(0, std::span{guest});
  machine.code(0x40, {system_call});
  machine.backing().put32(4, branch_condition(eq, 15)); // out: the svc at 0x40
  machine.put(machine.data(), 0x1234U);
  machine.begin();
  const auto registers = {Reg{1, machine.data()}, Reg{3, 1}, Reg{4, 2},
                          Reg{6, 4}, Reg{7, 8}, Reg{8, 0xabcdU}};
  /* Z clear: the data register reaches memory, not the flags. */
  auto cpu = start(machine.base(), registers);
  std::size_t executed = 0;
  require(machine.block(cpu, executed) == EngineEvent::continued);
  require(executed >= 6U && cpu.exclusive_size == 0U);
  std::size_t retired = 0;
  require(machine.run(cpu, retired) == EngineEvent::system_call);
  require(machine.get32(machine.data()) == 0xabcdU && cpu.registers[0] == 0U);
  /* Z set: the run is discarded and the branch taken from the load. */
  machine.put(machine.data(), 0x1234U);
  cpu = start(machine.base(), registers);
  cpu.nzcv = 0x4000'0000U;
  require(machine.block(cpu, executed) == EngineEvent::continued);
  require(executed == 1U && cpu.pc == machine.base() + 4U &&
          cpu.exclusive_size == 4U);
  require(machine.run(cpu, retired) == EngineEvent::system_call);
  require(machine.get32(machine.data()) == 0x1234U && cpu.nzcv == 0x4000'0000U);
  machine.end();
}

/* One register more than the pool: the run declines, and completes as the
 * semantic path completes it. */
void a_run_beyond_the_pool_declines(bool tracked) {
  const std::array<std::uint32_t, 8> guest{
      load_exclusive(2, false, 0, 1),
      branch_condition(eq, 6),
      add_sub_register(false, false, false, 0, 3, 4),
      logical_register(false, 2, false, 0, 0, 6),
      logical_register(false, 1, false, 0, 7, 9), // a seventh live-in
      store_exclusive(2, false, 0, 8, 1),
      compare_branch(false, true, 0, -6), system_call};
  Machine machine{static_cast<unsigned long>(tracked ? 10'481U : 10'480U),
                  0x300'0000U, tracked};
  machine.code(0, std::span{guest});
  machine.put(machine.data(), 0x1234U);
  machine.begin();
  const auto registers = {Reg{1, machine.data()}, Reg{3, 1}, Reg{4, 2},
                          Reg{6, 4}, Reg{7, 8}, Reg{9, 16}, Reg{8, 0xabcdU}};
  auto cpu = start(machine.base(), registers);
  std::size_t executed = 0;
  require(machine.block(cpu, executed) == EngineEvent::continued);
  require(executed == 1U && cpu.exclusive_size == 4U);
  std::size_t retired = 0;
  require(machine.run(cpu, retired) == EngineEvent::system_call);
  require(machine.get32(machine.data()) == 0xabcdU && cpu.registers[0] == 0U);
  machine.end();
}

/* Faults: the store of a fused run to a page that is read-only, the exit that
 * skips it, and a base that maps nothing. */
void faults_are_the_semantic_paths(bool tracked) {
  const std::array<std::uint32_t, 6> j1_words{
      shape_j1.words[0], shape_j1.words[1], shape_j1.words[2], shape_j1.words[3], shape_j1.words[4],
      system_call};
  const auto prepare = [&](Machine &machine, std::uint64_t memory_value) {
    machine.code(0, std::span{j1_words});
    machine.backing().put32(8, compare_branch(false, true, 16, 3)); // out
    machine.put(machine.data(), memory_value);
  };
  {
    Machine machine{static_cast<unsigned long>(tracked ? 10'491U : 10'490U),
                    0x340'0000U, tracked};
    prepare(machine, 0U);
    require(darwin_uml_static_vm_protect(
                reinterpret_cast<void *>(machine.data()), page, PROT_READ) == 0);
    machine.begin();
    auto cpu = start(machine.base(), {{19, machine.data()}});
    std::size_t retired = 0;
    require(machine.run(cpu, retired) == EngineEvent::memory_fault);
    /* The load, the move and the branch retire; the store faults at its PC. */
    require(retired == 3U && cpu.pc == machine.base() + 12U);
    require(machine.executor().fault_address() == machine.data());
    require(machine.get32(machine.data()) == 0U);
    machine.end();
  }
  {
    Machine machine{static_cast<unsigned long>(tracked ? 10'493U : 10'492U),
                    0x344'0000U, tracked};
    prepare(machine, 1U);
    require(darwin_uml_static_vm_protect(
                reinterpret_cast<void *>(machine.data()), page, PROT_READ) == 0);
    machine.begin();
    auto cpu = start(machine.base(), {{19, machine.data()}});
    std::size_t retired = 0;
    /* The taken branch skips the store: no fault is delivered. */
    require(machine.run(cpu, retired) == EngineEvent::system_call);
    require(cpu.registers[16] == 1U && machine.get32(machine.data()) == 1U);
    machine.end();
  }
  {
    Machine machine{static_cast<unsigned long>(tracked ? 10'495U : 10'494U),
                    0x348'0000U, tracked};
    prepare(machine, 0U);
    machine.begin();
    auto cpu = start(machine.base(), {{19, machine.base() + 40U * page}});
    std::size_t retired = 0;
    require(machine.run(cpu, retired) == EngineEvent::memory_fault);
    require(retired == 0U && cpu.pc == machine.base());
    require(machine.executor().fault_address() == machine.base() + 40U * page);
    machine.end();
  }
}

/*
 * Every class of word a run may hold, against the semantic path: the same
 * program and state run once through the fused block and once single-stepped,
 * and the registers, the flags and the memory must agree. A side exit is taken
 * in some states and not in others.
 */
struct Middle final {
  std::uint32_t word;
  bool exit{}; // a branch whose target is the end of the program
};

struct Final final {
  std::array<std::uint64_t, 31> registers;
  std::uint32_t nzcv;
  std::uint64_t memory;
  bool operator==(const Final &) const = default;
};

[[nodiscard]] Final run_program(const std::vector<Middle> &middles,
                                std::uint64_t memory_value,
                                std::uint32_t nzcv, std::uint64_t x4,
                                unsigned long generation, bool single_step,
                                bool tracked, bool *fused) {
  Machine machine{generation, 0x380'0000U, tracked};
  std::vector<std::uint32_t> words{load_exclusive(3, false, 3, 1)};
  const auto end = middles.size() + 3U;
  for (const auto &middle : middles) {
    auto word = middle.word;
    if (middle.exit) {
      const auto offset =
          static_cast<std::uint32_t>(end - words.size()) & 0x7ffffU;
      if ((word & 0xff00'0010U) == 0x5400'0000U ||
          (word & 0x7e00'0000U) == 0x3400'0000U)
        word = (word & ~(0x7ffffU << 5U)) | (offset << 5U);
      else
        word = (word & ~(0x3fffU << 5U)) | ((offset & 0x3fffU) << 5U);
    }
    words.push_back(word);
  }
  words.push_back(store_exclusive(3, false, 5, 6, 1));
  words.push_back(compare_branch(false, true, 5,
                                 -static_cast<int>(words.size())));
  words.push_back(system_call);
  machine.code(0, words);
  machine.put(machine.data(), memory_value);
  machine.executor().set_single_step(single_step);
  machine.begin();
  auto cpu = start(machine.base(), {{1, machine.data()}, {4, x4},
                                    {6, 0x1111'2222'3333'4444ULL}, {7, 5},
                                    {8, 0x8000'0000'0000'0001ULL},
                                    {9, 0xffff'ffffULL}, {20, 0x2020}});
  cpu.nzcv = nzcv;
  std::size_t retired = 0;
  std::size_t first = 0;
  auto event = machine.block(cpu, first);
  if (fused != nullptr)
    *fused = !single_step && first >= middles.size() + 3U;
  if (event == EngineEvent::continued)
    event = machine.run(cpu, retired, 400U);
  require(event == EngineEvent::system_call);
  Final result{};
  for (unsigned index = 0; index < 31U; ++index)
    result.registers[index] = cpu.registers[index];
  result.nzcv = cpu.nzcv;
  result.memory = machine.get(machine.data());
  machine.end();
  return result;
}

void every_word_class_matches_the_semantic_path(bool tracked) {
  using M = Middle;
  const std::vector<std::vector<M>> programs{
      {{movz(true, 6, 0x1234, 1)}},
      {{movn(false, 6, 0x55, 0)}},
      {{movz(true, 6, 1)}, {movk(true, 6, 0xbeef, 2)}},
      {{add_sub_immediate(true, false, false, 6, 3, 7)}},
      {{add_sub_immediate(false, true, false, 6, 3, 1)}},
      {{add_sub_immediate(true, false, true, 6, 3, 1)}},
      {{add_sub_immediate(false, true, true, 6, 3, 5)}},
      {{add_sub_immediate(true, true, true, 31, 3, 5)}, {0x5400'0001U, true}},
      {{add_sub_register(true, false, false, 6, 3, 4, 0, 3)}},
      {{add_sub_register(true, true, false, 6, 3, 4, 2, 2)}},
      {{add_sub_register(true, false, true, 6, 3, 4)}},
      {{add_sub_register(false, true, true, 6, 3, 4, 1, 1)}},
      {{logical_immediate(true, 0, 6, 3, 0, 56, 7)}},
      {{logical_immediate(false, 1, 6, 3, 0, 28, 3)}},
      {{logical_immediate(true, 2, 6, 3, 0, 1, 0)}},
      {{logical_immediate(true, 3, 6, 3, 1, 0, 15)}},
      {{logical_register(true, 0, false, 6, 3, 4)}},
      {{logical_register(true, 1, false, 6, 3, 4, 3, 7)}},
      {{logical_register(false, 2, false, 6, 3, 4, 0, 3)}},
      {{logical_register(true, 0, true, 6, 3, 4)}},
      {{logical_register(false, 1, true, 6, 3, 4)}},
      {{logical_register(true, 2, true, 6, 3, 4, 1, 4)}},
      {{logical_register(true, 3, false, 6, 3, 4)}},
      {{logical_register(false, 3, true, 6, 3, 4)}},
      /* The zero register as a source, a destination and a status. */
      {{logical_register(true, 1, false, 6, 31, 3)}},
      {{add_sub_register(true, false, false, 6, 3, 31)}},
      {{logical_register(true, 3, false, 31, 3, 4)}, {0x5400'0001U, true}},
      /* Side exits, taken and not. */
      {{compare_branch(true, false, 3, 0), true}, {add_sub_immediate(true, false, false, 6, 3, 1)}},
      {{compare_branch(false, true, 3, 0), true}, {add_sub_immediate(true, false, false, 6, 3, 1)}},
      {{test_branch(false, 3, 40, 0), true}, {movz(true, 6, 9)}},
      {{test_branch(true, 3, 3, 0), true}, {movz(true, 6, 9)}},
      {{add_sub_register(true, true, true, 31, 3, 4)}, {branch_condition(lt, 0), true},
       {add_sub_immediate(true, false, false, 6, 3, 1)}},
      /* A branch on the incoming flags. */
      {{branch_condition(eq, 0), true}, {add_sub_immediate(true, false, false, 6, 3, 2)}},
      {{branch_condition(mi, 0), true}, {add_sub_immediate(true, false, false, 6, 3, 2)}},
      {{compare_branch(true, false, 4, 0), true}, {test_branch(false, 4, 0, 0), true},
       {movz(true, 6, 2)}, {movk(true, 6, 3, 1)}},
  };
  const std::array<std::uint64_t, 4> memories{
      0x0123'4567'89ab'cdefULL, 0, 0x8000'0000'0000'0000ULL, 0xffff'ffff'ffff'ffffULL};
  const std::array<std::uint32_t, 3> flags{0x0000'0000U, 0x4000'0000U, 0x8000'0000U};
  unsigned long generation = tracked ? 10'700U : 10'600U;
  unsigned fused_runs = 0;
  for (const auto &program : programs) {
    for (const auto memory : memories) {
      for (const auto nzcv : flags) {
        for (const auto x4 : {std::uint64_t{0x1111}, memory, std::uint64_t{0}}) {
          bool fused = false;
          const auto native = run_program(program, memory, nzcv, x4,
                                          generation++, false, tracked, &fused);
          const auto control = run_program(program, memory, nzcv, x4,
                                           generation++, true, tracked, nullptr);
          if (!(native == control)) {
            std::fprintf(stderr, "program %zu memory %llx nzcv %x x4 %llx fused %d\n",
                         static_cast<std::size_t>(&program - programs.data()),
                         static_cast<unsigned long long>(memory), nzcv,
                         static_cast<unsigned long long>(x4), fused);
            for (unsigned r = 0; r < 31; ++r)
              if (native.registers[r] != control.registers[r])
                std::fprintf(stderr, "  x%u native %llx control %llx\n", r,
                             static_cast<unsigned long long>(native.registers[r]),
                             static_cast<unsigned long long>(control.registers[r]));
            std::fprintf(stderr, "  nzcv native %x control %x memory %llx %llx\n",
                         native.nzcv, control.nzcv,
                         static_cast<unsigned long long>(native.memory),
                         static_cast<unsigned long long>(control.memory));
          }
          require(native == control);
          fused_runs += fused ? 1U : 0U;
        }
      }
    }
  }
  /* The comparison would pass if nothing were fused. */
  require(fused_runs > programs.size());
}

} // namespace

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  require(arm64::set_jit_cache_budget(256U * 1024U * 1024U, 10));
  for (const bool tracked : {false, true}) {
    hot_shapes_fuse_and_roll_back(tracked);
    interior_store_entries_are_ordinary_code(tracked);
    recursive_lock_makes_progress(tracked);
    incoming_flags_under_scratch_pressure(tracked);
    a_run_beyond_the_pool_declines(tracked);
    faults_are_the_semantic_paths(tracked);
    every_word_class_matches_the_semantic_path(tracked);
  }
  return 0;
}
