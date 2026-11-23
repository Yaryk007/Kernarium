// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_exclusive_run_support.hpp"
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "cpu/arm64/jit/jit_cache_geometry.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <thread>
#include <vector>

using namespace uml::emulator::tests;
namespace arm64 = uml::cpu::arm64;
using ::uml::cpu::EngineEvent;

/*
 * The hot lock sequences of the Android runtime on four executor threads,
 * each in the words ART emits: a J1/J2 state machine used as a lock, a C1
 * compare-and-swap counter in its word and doubleword forms, and the L1/U1
 * lock. Per configuration the workers run all native; one of them runs a
 * forced-semantic copy of the same words; a fifth thread stores to the
 * granule and its line around the lock word, outside the protocol, so that
 * host store-exclusives fail and retry; and another vCPU holds a software
 * reservation, so every block runs in its tracked variant. A lock row
 * increments a counter under its lock with plain stores (a lost update is a
 * failed mutual exclusion); a counter row's final value is exact.
 */

namespace {

using namespace a64;

constexpr auto page = static_cast<std::uint64_t>(DARWIN_UML_GUEST_PAGE_SIZE);
constexpr std::uint64_t base = 0x1a0'0000U;
constexpr std::uint64_t data = base + page;
constexpr std::size_t workers = 4;
/* Each worker does `batch` operations per round and all four start every round
 * together, so each round is a real race for the word however the host
 * schedules the threads, and a slow worker cannot be left behind. */
constexpr unsigned rounds = 1000;
constexpr unsigned batch = 5;
constexpr unsigned iterations = rounds * batch;
constexpr std::uint32_t csel_x6 = 0x9a86'00c6U; // csel x6, x6, x6, eq
constexpr std::uint64_t lock_offset = page;      // the contended word
constexpr std::uint64_t counter_offset = page + 0x200U;
constexpr std::uint64_t holder_code = 0x800U;
constexpr std::uint64_t marker = 0x5c00'0000U;

struct Config final {
  bool semantic_worker;
  bool writer;
  bool holder;
};

/* The shape's words, optionally with a forced-semantic copy: a CSEL after the
 * load (a word no run holds) and the retry one word further back. */
[[nodiscard]] std::vector<std::uint32_t> program(const Shape &shape,
                                                 bool forced) {
  std::vector<std::uint32_t> words(shape.words.begin(),
                                   shape.words.begin() + shape.length);
  if (forced) {
    words.insert(words.begin() + 1, csel_x6);
    words.back() -= 1U << 5U;
  }
  words.resize(16, system_call);
  return words;
}

struct Stats final {
  std::size_t acquire_widest{};
  std::size_t release_widest{};
  std::uint64_t retried{};
  std::uint64_t failed_attempts{};
  std::uint64_t operations{};
};

/* The guest of one row: code at the start of the first page, data on the
 * next, a holder's granule on the fourth. */
class World final {
public:
  World(unsigned long generation, const Config &config)
      : backing_{4}, mapping_{generation, base, backing_, 4},
        generation_{generation}, config_{config} {}

  void code(std::uint64_t offset, std::span<const std::uint32_t> words) {
    for (std::size_t index = 0; index < words.size(); ++index)
      backing_.put32(offset + index * 4U, words[index]);
  }
  [[nodiscard]] JitBacking &backing() { return backing_; }
  [[nodiscard]] unsigned long generation() const { return generation_; }
  [[nodiscard]] const Config &config() const { return config_; }

  /* Another vCPU holds a reservation on a granule of its own. */
  void take_holder() {
    backing_.put32(holder_code, load_exclusive(3, false, 3, 7));
    backing_.put32(holder_code + 4, system_call);
    holder_cpu_.pc = base + holder_code;
    holder_cpu_.registers[7] = base + 3U * page + 0x200U;
    require(holder_.begin_execution(generation_, base));
    std::size_t executed = 0;
    require(holder_.run_block(holder_cpu_, holder_memory_, generation_,
                              executed) == EngineEvent::continued);
    require(executed == 1U && holder_cpu_.exclusive_size == 8U);
    held_ = true;
  }
  void release_holder() {
    if (held_)
      holder_.end_execution();
    held_ = false;
  }

private:
  JitBacking backing_;
  JitMapping mapping_;
  unsigned long generation_;
  Config config_;
  arm64::JitExecutor holder_;
  uml::memory::StaticGuestMemory holder_memory_;
  arm64::CpuState holder_cpu_{};
  bool held_{};
};

/* Runs the guest from cpu.pc until it leaves for something but `continued`:
 * returns the widest block retired, and whether the sequence ran a second
 * pass (a host store-exclusive that failed): in one chained block, which
 * retires two passes, or by a second block entered at the load, as the
 * unlinked tracked variant does. */
struct Run final {
  std::size_t widest{};
  bool passed_again{};
};

Run run_to_call(arm64::JitExecutor &executor,
                uml::memory::StaticGuestMemory &memory,
                arm64::CpuState &cpu, unsigned long generation,
                std::size_t length) {
  Run run;
  const auto entry = cpu.pc;
  unsigned entries = 0;
  auto event = EngineEvent::continued;
  for (unsigned attempt = 0;
       attempt < 10000 && event == EngineEvent::continued; ++attempt) {
    std::size_t executed = 0;
    entries += cpu.pc == entry ? 1U : 0U;
    event = executor.run_block(cpu, memory, generation, executed);
    run.widest = std::max(run.widest, executed);
    if (executed >= 2U * length)
      run.passed_again = true;
  }
  if (entries >= 2U)
    run.passed_again = true;
  require(event == EngineEvent::system_call);
  return run;
}

/* Which worker took each step, indexed by the lock's counter or the CAS
 * counter's value. The workers contended when the stretches of steps they
 * took overlap pairwise: none ran its whole share before another began. A
 * forced-semantic worker is left out of the pairs: it is far slower than a
 * native one and may take its first step only when the others are done, which
 * the architecture permits; its failed attempts show it met them. */
struct OwnerLog final {
  std::array<std::atomic<std::uint8_t>, 1U << 15U> owner{};
  [[nodiscard]] bool interleaved(std::size_t total, unsigned skip) const {
    std::array<std::size_t, workers + 1U> first{};
    std::array<std::size_t, workers + 1U> last{};
    first.fill(total);
    for (std::size_t step = 0; step < total; ++step) {
      const auto tid = owner[step].load();
      require(tid >= 1U && tid <= workers);
      first[tid] = std::min(first[tid], step);
      last[tid] = std::max(last[tid], step);
    }
    for (std::size_t a = 1; a <= workers; ++a)
      for (std::size_t b = a + 1U; b <= workers; ++b)
        if (a != skip && b != skip &&
            (first[a] > last[b] || first[b] > last[a]))
          return false;
    return true;
  }
};

struct LockFamily final {
  const char *name;
  const Shape *acquire;
  const Shape *release;
  /* The registers of the acquire and of the release for worker `tid`. */
  void (*set)(arm64::CpuState &cpu, unsigned tid, std::uint64_t address);
  bool (*acquired)(const arm64::CpuState &cpu);
  bool (*released)(const arm64::CpuState &cpu);
  /* The bits of the lock word that name its state, and their value while
   * worker `tid` holds it. */
  std::uint32_t mask;
  std::uint32_t (*held)(unsigned tid);
};

constexpr std::uint32_t zero_flag = 0x4000'0000U;

const LockFamily j_lock{
    "J1/J2",
    &shape_j1,
    &shape_j2,
    [](arm64::CpuState &cpu, unsigned, std::uint64_t address) {
      cpu.registers[19] = address;
      cpu.nzcv = 0;
    },
    [](const arm64::CpuState &cpu) { return cpu.registers[16] == 0U; },
    [](const arm64::CpuState &cpu) { return (cpu.nzcv & zero_flag) != 0U; },
    0xffff'ffffU,
    [](unsigned) { return static_cast<std::uint32_t>(marker); }};

const LockFamily l_lock{
    "L1/U1",
    &shape_l1,
    &shape_u1,
    [](arm64::CpuState &cpu, unsigned tid, std::uint64_t address) {
      cpu.registers[8] = address;
      cpu.registers[9] = tid;
      cpu.nzcv = 0;
    },
    [](const arm64::CpuState &cpu) { return (cpu.nzcv & zero_flag) != 0U; },
    [](const arm64::CpuState &cpu) { return (cpu.nzcv & zero_flag) != 0U; },
    0xcfff'ffffU,
    [](unsigned tid) { return tid; }};

void lock_worker(World &world, const LockFamily &family, std::size_t index,
                 std::barrier<> &start, OwnerLog &log, Stats &stats) {
  const bool forced = world.config().semantic_worker && index == 0;
  arm64::JitExecutor executor;
  uml::memory::StaticGuestMemory memory;
  arm64::CpuState cpu{};
  const auto generation = world.generation();
  require(executor.begin_execution(generation, base));
  const auto tid = static_cast<unsigned>(index + 1U);
  auto &backing = world.backing();
  /* Translate and link both programs on a word of this worker's own, so that
   * the workers start the contended loop together rather than one finishing
   * before the others have compiled. */
  const auto own = data + 0x100U + 0x40U * index;
  for (const auto entry : {forced ? 0x100U : 0U, forced ? 0x300U : 0x200U}) {
    cpu.pc = base + entry;
    family.set(cpu, tid, own);
    (void)run_to_call(executor, memory, cpu, generation, 1U);
  }
  require((backing.get32(own - base) & family.mask) == 0U);
  for (unsigned round = 0; round < rounds; ++round) {
    start.arrive_and_wait();
    for (unsigned step = 0; step < batch; ++step) {
      unsigned tries = 0;
      for (;;) {
        cpu.pc = base + (forced ? 0x100U : 0U);
        family.set(cpu, tid, data);
        const auto run = run_to_call(executor, memory, cpu, generation,
                                     family.acquire->length);
        stats.acquire_widest = std::max(stats.acquire_widest, run.widest);
        stats.retried += !forced && run.passed_again ? 1U : 0U;
        if (family.acquired(cpu))
          break;
        ++stats.failed_attempts;
        require(++tries < 5'000'000U);
        if ((tries & 63U) == 0U)
          std::this_thread::yield();
      }
      /* The critical section: the lock word shows its owner; the counter is
       * read and written back by plain stores, so a second owner loses one. */
      require((backing.get32(lock_offset) & family.mask) == family.held(tid));
      const auto value = backing.get64(counter_offset);
      require(value < log.owner.size());
      log.owner[value].store(static_cast<std::uint8_t>(tid));
      backing.put64(counter_offset, value + 1U);
      cpu.pc = base + (forced ? 0x300U : 0x200U);
      family.set(cpu, tid, data);
      const auto run = run_to_call(executor, memory, cpu, generation,
                                   family.release->length);
      stats.release_widest = std::max(stats.release_widest, run.widest);
      stats.retried += !forced && run.passed_again ? 1U : 0U;
      require(family.released(cpu));
      ++stats.operations;
    }
  }
  executor.end_execution();
}

/* The CAS counter: C1 in its word or doubleword form. The compare takes the
 * counter's value, the store its successor. */
struct CounterFamily final {
  const char *name;
  const Shape *shape;
  std::uint64_t initial;
};

const CounterFamily c1_word{"C1 (W)", &shape_c1_w, 0};
const CounterFamily c1_double{"C1 (X)", &shape_c1_x, 0x1'0000'0000ULL};

void counter_worker(World &world, const CounterFamily &family,
                    std::size_t index, std::barrier<> &start, OwnerLog &log,
                    Stats &stats) {
  const bool forced = world.config().semantic_worker && index == 0;
  arm64::JitExecutor executor;
  uml::memory::StaticGuestMemory memory;
  arm64::CpuState cpu{};
  const auto generation = world.generation();
  require(executor.begin_execution(generation, base));
  const auto tid = static_cast<std::uint8_t>(index + 1U);
  /* Translate and link on a counter of this worker's own (see lock_worker). */
  const auto own = data + 0x100U + 0x40U * index;
  cpu.pc = base + (forced ? 0x100U : 0U);
  cpu.registers[16] = own;
  cpu.registers[2] = family.initial;
  cpu.registers[0] = family.initial + 1U;
  (void)run_to_call(executor, memory, cpu, generation, 1U);
  require((cpu.nzcv & zero_flag) != 0U);
  std::uint64_t expected = family.initial;
  for (unsigned round = 0; round < rounds; ++round) {
    start.arrive_and_wait();
    for (unsigned successes = 0; successes < batch;) {
      cpu.pc = base + (forced ? 0x100U : 0U);
      cpu.registers[16] = data;
      cpu.registers[2] = expected;
      cpu.registers[0] = expected + 1U;
      cpu.nzcv = 0;
      const auto run = run_to_call(executor, memory, cpu, generation,
                                   family.shape->length);
      stats.acquire_widest = std::max(stats.acquire_widest, run.widest);
      stats.retried += !forced && run.passed_again ? 1U : 0U;
      if ((cpu.nzcv & zero_flag) != 0U) {
        // The values matched and the store went through.
        require(cpu.registers[17] == 0U);
        const auto slot = expected - family.initial;
        require(slot < log.owner.size());
        log.owner[slot].store(tid);
        ++successes;
        ++stats.operations;
        expected += 1U;
      } else {
        // Stale: the load's value is the next expectation.
        require(cpu.registers[17] != expected);
        expected = cpu.registers[17];
        ++stats.failed_attempts;
      }
    }
  }
  executor.end_execution();
}

[[nodiscard]] unsigned long next_generation() {
  static unsigned long generation = 11'000U;
  return generation++;
}

/* The workers, the writer and the holder of one configuration. */
template <typename Worker>
void run_workers(World &world, const Config &config,
                 std::array<Stats, workers> &stats, Worker &&worker) {
  std::barrier start{static_cast<std::ptrdiff_t>(workers)};
  std::atomic<bool> stop{false};
  std::thread writer;
  if (config.holder)
    world.take_holder();
  if (config.writer)
    writer = std::thread{[&] {
      // Plain stores around the contended word: the other word of its
      // 16-byte granule and one on the rest of its cache line.
      std::uint64_t value = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        world.backing().put64(lock_offset + 8U, ++value);
        world.backing().put64(lock_offset + 0x38U, value);
      }
    }};
  std::vector<std::thread> threads;
  for (std::size_t index = 0; index < workers; ++index)
    threads.emplace_back([&, index] { worker(index, start, stats[index]); });
  for (auto &thread : threads)
    thread.join();
  stop.store(true);
  if (writer.joinable())
    writer.join();
  world.release_holder();
}

void check_paths(const Config &config, const std::array<Stats, workers> &stats,
                 const Shape &acquire, const Shape *release,
                 const OwnerLog &log) {
  std::uint64_t retried = 0;
  for (std::size_t index = 0; index < workers; ++index) {
    const bool forced = config.semantic_worker && index == 0;
    if (forced) {
      /* A forced-semantic copy never retires a whole pass in one block. */
      require(stats[index].acquire_widest < acquire.length);
      if (release != nullptr)
        require(stats[index].release_widest < release->length);
    } else {
      require(stats[index].acquire_widest >= acquire.length);
      if (release != nullptr)
        require(stats[index].release_widest >= release->length);
    }
    require(stats[index].operations == iterations);
    retried += stats[index].retried;
  }
  /* The writer's stores must have failed at least one host store-exclusive
   * for the interference to count; a row it did not disturb is invalid. */
  if (config.writer)
    require(retried > 0U);
  /* The workers contended: each took its turns while the others still had
   * theirs to take, and some of their attempts met another worker's state. */
  const unsigned skip = config.semantic_worker ? 1U : 0U;
  require(log.interleaved(workers * iterations, skip));
  std::uint64_t failed = 0;
  for (const auto &own : stats)
    failed += own.failed_attempts;
  require(failed > 0U);
}

void lock_row(const LockFamily &family, const Config &config) {
  const auto generation = next_generation();
  World world{generation, config};
  for (const bool forced : {false, true}) {
    const auto acquire = program(*family.acquire, forced);
    const auto release = program(*family.release, forced);
    world.code(forced ? 0x100U : 0U, acquire);
    world.code(forced ? 0x300U : 0x200U, release);
  }
  world.backing().put64(counter_offset, 0);
  world.backing().put64(lock_offset, 0);
  OwnerLog log;
  std::array<Stats, workers> stats{};
  run_workers(world, config, stats,
              [&](std::size_t index, std::barrier<> &start, Stats &own) {
                lock_worker(world, family, index, start, log, own);
              });
  require(world.backing().get64(counter_offset) == workers * iterations);
  /* Every owner released: the word is free again. */
  require((world.backing().get32(lock_offset) & family.mask) == 0U);
  check_paths(config, stats, *family.acquire, family.release, log);
}

void counter_row(const CounterFamily &family, const Config &config) {
  const auto generation = next_generation();
  World world{generation, config};
  for (const bool forced : {false, true})
    world.code(forced ? 0x100U : 0U, program(*family.shape, forced));
  world.backing().put64(lock_offset, family.initial);
  for (std::size_t index = 0; index < workers; ++index)
    world.backing().put64(lock_offset + 0x100U + 0x40U * index, family.initial);
  OwnerLog log;
  std::array<Stats, workers> stats{};
  run_workers(world, config, stats,
              [&](std::size_t index, std::barrier<> &start, Stats &own) {
                counter_worker(world, family, index, start, log, own);
              });
  const auto mask = family.shape->size == 3U ? ~std::uint64_t{}
                                             : std::uint64_t{0xffff'ffffU};
  require((world.backing().get64(lock_offset) & mask) ==
          ((family.initial + workers * iterations) & mask));
  check_paths(config, stats, *family.shape, nullptr, log);
}

constexpr std::array<Config, 5> configurations{{
    {false, false, false}, // all native
    {true, false, false},  // one worker forced semantic
    {false, true, false},  // a writer around the word
    {false, false, true},  // a holder: the tracked variant
    {true, true, true},    // all of it
}};

} // namespace

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  require(arm64::set_jit_cache_budget(256U * 1024U * 1024U, 10));
  for (const auto &config : configurations) {
    lock_row(j_lock, config);
    lock_row(l_lock, config);
    counter_row(c1_word, config);
    counter_row(c1_double, config);
  }
  return 0;
}
