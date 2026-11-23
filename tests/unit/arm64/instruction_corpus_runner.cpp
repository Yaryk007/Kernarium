// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Runs the generated A64 instruction corpus through the production gadget
 * and JIT engines in one process. tests/unit/arm64/corpus/README.md owns the
 * pack format and the named validator relations; tools/arm64-tests/
 * build-unit-corpus.py defines the state fields, option bits and fills.
 */
#include "a64_register_operands.hpp"
#include "architecture_difference.hpp"
#include "arm64_exclusive_retry.hpp"
#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/detail/sme_decode.hpp"
#include "cpu/arm64/engine_execution.hpp"
#include "cpu/arm64/floating_controls.hpp"
#include "cpu/arm64/gadgets/executor.hpp"
#include "cpu/arm64/gadgets/sme_support.hpp"
#include "cpu/arm64/jit/jit_sme_support.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "cpu/engine.hpp"
#include "cpu/engine_factory.hpp"
#include "cpu/host_capabilities.hpp"
#include "memory/static_guest_memory.hpp"
#include "platform/apple/host_features.hpp"

#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"
#include "static_mmu_test_support.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

namespace arm64 = ::uml::cpu::arm64;
using ::uml::emulator::tests::exclusive_attempts;
using ::uml::emulator::tests::first_architecture_difference;
using ::uml::emulator::tests::first_sme_register_difference;
using ::uml::cpu::ArmFeature;
using ::uml::cpu::ArmFeatureSet;
using ::uml::cpu::EngineKind;
using ::uml::cpu::ExitReason;
using ::uml::memory::MemoryAccess;
using ::uml::memory::MemoryFault;
using ::uml::memory::StaticGuestMemory;

constexpr std::uint64_t code_base = 0x4f00'0000;
constexpr std::size_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr std::size_t execution_limit = 64;
constexpr unsigned long generation = 0x10000;
constexpr std::array<char, 8> pack_magic{'U', 'M', 'L', 'A',
                                         '6', '4', 'C', '\0'};
constexpr std::uint32_t pack_version = 2;
/* A record's profiles byte: bit 1 is the 4 KiB profile. Bit 0 was the retired
 * 16 KiB profile, which a pack no longer carries. */
constexpr std::uint8_t record_profiles = 2U;
constexpr std::uint32_t unbounded = 0xffff'ffffU;
constexpr std::uint16_t no_validator = 0xffffU;
/* Option bits 0 and 1 enabled the retired FEAT_PAuth and FEAT_GCS, and bit 12
 * asked for a virtual CPU without FEAT_SVE, which no virtual CPU has. */
constexpr std::uint16_t retired_options = 0x1003U;
/* Option bit 10 runs a record on a virtual CPU without FEAT_AFP on any host:
 * its cases pin FPCR.FIZ, AH and NEP as RES0. */
constexpr std::uint16_t without_afp_option = 0x400U;
/* Option bit 11 does the same for FEAT_SME: its cases pin SVE and SME
 * encodings UNDEFINED. */
constexpr std::uint16_t without_sme_option = 0x800U;
constexpr std::uint16_t known_options = 0x1fffU;
/* How long --interrupt may go on looking for a lost reservation. */
constexpr std::chrono::seconds interrupt_deadline{60};
/* SME state: Z0-Z31, P0-P15 and the ZA rows are sized by a record's
 * streaming vector length (SVL), ZT0 is 64 bytes. */
constexpr std::size_t zt0_bytes = arm64::sme_zt0_bytes;

[[nodiscard]] constexpr bool valid_svl(unsigned bytes) noexcept
{
  return arm64::valid_streaming_vector_length(bytes);
}

class CorpusError final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

[[nodiscard]] std::string hex(std::uint64_t value) {
  char text[24];
  std::snprintf(text, sizeof(text), "%#" PRIx64, value);
  return text;
}

/* ------------------------------------------------------------------------ */
/* Pack decoding                                                              */

class Reader final {
public:
  explicit Reader(std::span<const std::byte> data) noexcept : data_{data} {}

  [[nodiscard]] std::span<const std::byte> bytes(std::size_t size) {
    if (size > data_.size() - offset_)
      throw CorpusError{"truncated corpus pack"};
    const auto result = data_.subspan(offset_, size);
    offset_ += size;
    return result;
  }

  [[nodiscard]] std::uint64_t little(std::size_t size) {
    const auto data = bytes(size);
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < size; ++index)
      value |= std::to_integer<std::uint64_t>(data[index]) << (index * 8U);
    return value;
  }

  [[nodiscard]] std::uint8_t u8() {
    return static_cast<std::uint8_t>(little(1));
  }
  [[nodiscard]] std::uint16_t u16() {
    return static_cast<std::uint16_t>(little(2));
  }
  [[nodiscard]] std::uint32_t u32() {
    return static_cast<std::uint32_t>(little(4));
  }
  [[nodiscard]] std::uint64_t u64() { return little(8); }

  [[nodiscard]] std::string_view string(std::size_t size) {
    const auto data = bytes(size);
    return {reinterpret_cast<const char *>(data.data()), data.size()};
  }
  [[nodiscard]] std::string_view string16() { return string(u16()); }
  [[nodiscard]] std::string_view string32() { return string(u32()); }

  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] bool done() const noexcept { return offset_ == data_.size(); }

private:
  std::span<const std::byte> data_;
  std::size_t offset_{};
};

/* Terminal events, in pack order. Code 2 was the retired FEAT_GCS fault. */
enum class Event : std::uint8_t {
  system_call,
  memory_fault,
  illegal_instruction = 3,
  breakpoint,
  floating_point_trap,
  pc_alignment,
  instruction_budget,
  fatal,
};
constexpr std::uint8_t retired_event = 2U;
constexpr std::array<std::string_view, 9> event_names{
    "system-call",        "memory-fault", "retired",
    "illegal-instruction", "breakpoint",  "floating-point-trap",
    "pc-alignment",        "instruction-budget", "fatal"};

[[nodiscard]] std::string_view event_name(Event event) noexcept {
  return event_names[static_cast<std::size_t>(event)];
}

[[nodiscard]] constexpr Event event_for(ExitReason reason) noexcept {
  switch (reason) {
  case ExitReason::system_call:
    return Event::system_call;
  case ExitReason::memory_fault:
    return Event::memory_fault;
  case ExitReason::illegal_instruction:
    return Event::illegal_instruction;
  case ExitReason::breakpoint:
    return Event::breakpoint;
  case ExitReason::floating_point_trap:
    return Event::floating_point_trap;
  case ExitReason::pc_alignment:
    return Event::pc_alignment;
  case ExitReason::instruction_budget:
  case ExitReason::interrupt:
    return Event::instruction_budget;
  case ExitReason::single_step:
  case ExitReason::sme_access:
  case ExitReason::fatal:
    return Event::fatal;
  }
  return Event::fatal;
}

constexpr std::array<std::string_view, 3> access_names{"read", "write",
                                                       "execute"};

struct Retirement final {
  enum class Kind : std::uint8_t { exact, pattern, unchecked };
  Kind kind{Kind::exact};
  std::uint32_t minimum{};
  std::uint32_t step{1};
  std::uint32_t maximum{unbounded};
};

struct RegionSpec final {
  std::uint64_t address{};
  std::uint32_t size{};
  std::uint8_t flags{};
  std::uint32_t blob{};
  /* Encoded (offset, length, bytes) runs that differ after execution. */
  std::span<const std::byte> runs;
  std::uint16_t run_count{};

  [[nodiscard]] bool readable() const noexcept { return (flags & 1U) != 0U; }
  [[nodiscard]] bool writable() const noexcept { return (flags & 2U) != 0U; }
  [[nodiscard]] bool executable() const noexcept { return (flags & 4U) != 0U; }
  [[nodiscard]] bool guest_code() const noexcept { return (flags & 8U) != 0U; }
  [[nodiscard]] bool guest_writable() const noexcept {
    return (flags & 16U) != 0U;
  }
};

struct CorpusCase final {
  std::uint32_t program{};
  std::string_view name;
  std::uint32_t ordinal_16k{};
  std::uint32_t ordinal_4k{};
  Event event{};
  bool invalid_encoding{};
  bool compare_fault{};
  Retirement retired;
  std::uint64_t fault_address{};
  MemoryAccess fault_access{MemoryAccess::read};
  std::uint32_t syndrome{};
  std::uint16_t native{};
  std::uint16_t validator{no_validator};
  std::span<const std::byte> initial;
  std::span<const std::byte> expected;
  std::vector<RegionSpec> memory;
};

struct ProgramWord final {
  std::uint16_t offset{};
  std::uint32_t word{};
};

struct Program final {
  std::uint16_t entry{};
  std::uint32_t entry_word{};
  std::vector<ProgramWord> words;
};

struct Record final {
  std::string_view name;
  std::string_view producer;
  std::string_view site;
  std::uint8_t profiles{};
  std::uint16_t options{};
  /* The SVL the cases' SME state is sized for and their expectations hold
   * at, or 0 for a record without SME state. */
  std::uint16_t svl{};
  std::vector<Program> programs;
  std::vector<CorpusCase> cases;
};

/* Every encoded non-register state field; see the README field table. Ids
 * 7, 9-16, 20, and 32-37 held the retired FEAT_MTE, FEAT_CPA, FEAT_GCS, and
 * FEAT_PAuth state, 17 the retired FEAT_LS64 ACCDATA, 5 the retired FEAT_FP8
 * FPMR, 18 the retired PSTATE.SSBS, and 21 and 22 the producer's one-byte
 * PSTATE.SM and ZA, and 40 the retired non-streaming SVE liveness; all stay
 * reserved. SVCR and TPIDR2_EL0 appear only in a record with an SVL. */
enum class Field : std::uint8_t {
  pc = 1,
  nzcv,
  fpcr,
  fpsr,
  tls = 6,
  tbi_enabled = 8,
  pstate_dit = 19,
  pstate_daif_access = 23,
  pstate_daif,
  exclusive_address,
  exclusive_key,
  exclusive_value,
  exclusive_value2,
  exclusive_version,
  exclusive_size,
  fp_dirty,
  svcr = 38,
  tpidr2,
};

[[nodiscard]] std::size_t field_size(std::uint8_t identifier) {
  switch (static_cast<Field>(identifier)) {
  case Field::pc:
  case Field::tls:
  case Field::exclusive_address:
  case Field::exclusive_key:
  case Field::exclusive_value:
  case Field::exclusive_value2:
  case Field::exclusive_version:
  case Field::tpidr2:
    return 8;
  case Field::nzcv:
  case Field::fpcr:
  case Field::fpsr:
    return 4;
  case Field::tbi_enabled:
  case Field::pstate_dit:
  case Field::pstate_daif_access:
  case Field::pstate_daif:
  case Field::exclusive_size:
  case Field::fp_dirty:
  case Field::svcr:
    return 1;
  }
  throw CorpusError{"unknown state field " + std::to_string(identifier)};
}

/* Skips one sparse state's SME sections: Z, P, ZA rows, ZT0. */
void skip_sme_sections(Reader &reader, unsigned svl) {
  const auto z = reader.u32();
  (void)reader.bytes(svl * static_cast<std::size_t>(std::popcount(z)));
  const auto p = reader.u16();
  (void)reader.bytes(svl / 8U * static_cast<std::size_t>(std::popcount(p)));
  std::size_t rows = 0;
  for (const auto byte : reader.bytes(svl / 8U))
    rows += static_cast<std::size_t>(
        std::popcount(std::to_integer<unsigned>(byte)));
  (void)reader.bytes(svl * rows);
  const auto zt0 = reader.u8();
  if (zt0 > 1U)
    throw CorpusError{"invalid ZT0 flag"};
  (void)reader.bytes(zt0 * zt0_bytes);
}

/* Returns the encoded span of one sparse state and validates its shape. */
[[nodiscard]] std::span<const std::byte>
sparse_state(Reader &reader, std::span<const std::byte> body, unsigned svl) {
  const auto begin = reader.offset();
  const auto registers = reader.u32();
  (void)reader.bytes(8U * static_cast<std::size_t>(std::popcount(registers)));
  const auto vectors = reader.u32();
  (void)reader.bytes(16U * static_cast<std::size_t>(std::popcount(vectors)));
  if (svl != 0U)
    skip_sme_sections(reader, svl);
  const auto fields = reader.u8();
  for (unsigned index = 0; index < fields; ++index)
    (void)reader.bytes(field_size(reader.u8()));
  return body.subspan(begin, reader.offset() - begin);
}

[[nodiscard]] std::uint64_t fill_register(unsigned index) noexcept {
  return 0x5eed'0000'0000'0000ULL |
         (static_cast<std::uint64_t>(index & 31U) << 40U) |
         (0x0101'0101ULL * (static_cast<std::uint64_t>(index & 31U) + 1U));
}

[[nodiscard]] std::byte fill_vector_byte(unsigned index,
                                         unsigned byte) noexcept {
  return static_cast<std::byte>(((index & 31U) * 16U + (byte & 15U)) * 0x9dU +
                                0x3bU);
}

/* The base every initial state is encoded against: canonical register fill,
 * the entry PC, and the pack's default for every other exported field. */
[[nodiscard]] arm64::CpuState base_state(std::uint16_t entry) {
  arm64::CpuState state{};
  for (unsigned index = 0; index < state.registers.size(); ++index)
    state.registers[index] = fill_register(index);
  state.sp = fill_register(31);
  for (unsigned index = 0; index < state.vectors.size(); ++index)
    for (unsigned byte = 0; byte < 16U; ++byte)
      state.vectors[index].bytes[byte] = fill_vector_byte(index, byte);
  state.pc = code_base + entry;
  state.nzcv = 0;
  state.fpcr = 0;
  state.fpsr = 0;
  state.tls = 0;
  state.tbi_enabled = false;
  state.pstate_dit = false;
  state.pstate_daif_access = false;
  state.pstate_daif = 0;
  state.exclusive_address = ~std::uint64_t{0};
  state.exclusive_key = 0;
  state.exclusive_value = 0;
  state.exclusive_value2 = 0;
  state.exclusive_version = 0;
  state.exclusive_size = 0;
  state.fp_dirty = false;
  return state;
}

/* One byte of the SME base fill; see the README's SME state. */
[[nodiscard]] std::byte sme_fill(std::uint32_t index) noexcept {
  return static_cast<std::byte>((index * 0x9e37'79b1U) >> 24U);
}

/* The SME state every initial state is encoded against, in the register
 * file's layout (Z, P, ZA rows, ZT0). */
[[nodiscard]] std::vector<std::byte> sme_base_image(unsigned svl) {
  std::vector<std::byte> image(arm64::sme_storage_size(svl));
  for (unsigned n = 0; n < arm64::sme_z_registers; ++n)
    for (unsigned byte = 0; byte < svl; ++byte)
      image[arm64::sme_z_offset(svl, n) + byte] = sme_fill(n << 8U | byte);
  for (unsigned n = 0; n < arm64::sme_p_registers; ++n)
    for (unsigned byte = 0; byte < svl / 8U; ++byte)
      image[arm64::sme_p_offset(svl, n) + byte] =
          sme_fill(0x2000U | n << 5U | byte);
  for (unsigned row = 0; row < svl; ++row)
    for (unsigned byte = 0; byte < svl; ++byte)
      image[arm64::sme_za_offset(svl, row) + byte] =
          sme_fill(0x1'0000U | row << 8U | byte);
  for (unsigned byte = 0; byte < zt0_bytes; ++byte)
    image[arm64::sme_zt0_offset(svl) + byte] = sme_fill(0x3'0000U | byte);
  return image;
}

/* Applies one sparse state's SME sections to an SME image. */
void apply_sme_sections(Reader &reader, std::span<std::byte> image,
                        unsigned svl) {
  const auto copy = [&](std::size_t offset, std::size_t size) {
    const auto data = reader.bytes(size);
    std::copy(data.begin(), data.end(), image.begin() + offset);
  };
  const auto z = reader.u32();
  for (unsigned n = 0; n < arm64::sme_z_registers; ++n)
    if ((z >> n) & 1U)
      copy(arm64::sme_z_offset(svl, n), svl);
  const auto p = reader.u16();
  for (unsigned n = 0; n < arm64::sme_p_registers; ++n)
    if ((p >> n) & 1U)
      copy(arm64::sme_p_offset(svl, n), svl / 8U);
  const auto rows = reader.bytes(svl / 8U);
  for (unsigned row = 0; row < svl; ++row)
    if ((std::to_integer<unsigned>(rows[row / 8U]) >> (row % 8U)) & 1U)
      copy(arm64::sme_za_offset(svl, row), svl);
  if (reader.u8() != 0U)
    copy(arm64::sme_zt0_offset(svl), zt0_bytes);
}

/* Applies a sparse state; `image` receives its SME sections when the
 * record has an SVL. */
void apply_sparse(arm64::CpuState &state, std::span<const std::byte> encoded,
                  unsigned svl = 0, std::span<std::byte> image = {}) {
  Reader reader{encoded};
  const auto registers = reader.u32();
  for (unsigned index = 0; index < 32U; ++index)
    if ((registers >> index) & 1U) {
      const auto value = reader.u64();
      if (index == 31U)
        state.sp = value;
      else
        state.registers[index] = value;
    }
  const auto vectors = reader.u32();
  for (unsigned index = 0; index < 32U; ++index)
    if ((vectors >> index) & 1U) {
      const auto data = reader.bytes(16);
      std::copy(data.begin(), data.end(), state.vectors[index].bytes.begin());
    }
  if (svl != 0U)
    apply_sme_sections(reader, image, svl);
  const auto fields = reader.u8();
  for (unsigned field = 0; field < fields; ++field) {
    const auto identifier = reader.u8();
    const auto flag = [&] { return reader.u8() != 0U; };
    switch (static_cast<Field>(identifier)) {
    case Field::pc:
      state.pc = reader.u64();
      break;
    case Field::nzcv:
      state.nzcv = reader.u32();
      break;
    case Field::fpcr:
      state.fpcr = reader.u32();
      break;
    case Field::fpsr:
      state.fpsr = reader.u32();
      break;
    case Field::tls:
      state.tls = reader.u64();
      break;
    case Field::tbi_enabled:
      state.tbi_enabled = flag();
      break;
    case Field::pstate_dit:
      state.pstate_dit = flag();
      break;
    case Field::pstate_daif_access:
      state.pstate_daif_access = flag();
      break;
    case Field::pstate_daif:
      state.pstate_daif = reader.u8();
      break;
    case Field::exclusive_address:
      state.exclusive_address = reader.u64();
      break;
    case Field::exclusive_key:
      state.exclusive_key = static_cast<std::uintptr_t>(reader.u64());
      break;
    case Field::exclusive_value:
      state.exclusive_value = reader.u64();
      break;
    case Field::exclusive_value2:
      state.exclusive_value2 = reader.u64();
      break;
    case Field::exclusive_version:
      state.exclusive_version = reader.u64();
      break;
    case Field::exclusive_size:
      state.exclusive_size = reader.u8();
      break;
    case Field::fp_dirty:
      state.fp_dirty = flag();
      break;
    case Field::svcr:
      state.svcr = reader.u8();
      break;
    case Field::tpidr2:
      state.tpidr2 = reader.u64();
      break;
    default:
      throw CorpusError{"unknown state field " + std::to_string(identifier)};
    }
  }
  if (!reader.done())
    throw CorpusError{"trailing sparse state data"};
}

/* While a state's PSTATE.SM is set its V registers are its Z registers' low
 * bytes. */
void streaming_view(arm64::CpuState &state, std::span<const std::byte> image,
                    unsigned svl) {
  if (svl == 0U || (state.svcr & arm64::svcr_sm) == 0U)
    return;
  for (unsigned n = 0; n < arm64::sme_z_registers; ++n)
    std::copy_n(image.begin() + static_cast<std::ptrdiff_t>(
                                    arm64::sme_z_offset(svl, n)),
                16, state.vectors[n].bytes.begin());
}

/* ------------------------------------------------------------------------ */
/* Architectural comparison and validator relations                         */

struct RelationInput final {
  /* Initial and expected states are normalized exactly like the actual one. */
  const arm64::CpuState &initial;
  const arm64::CpuState &expected;
  const arm64::CpuState &actual;
  const Program &program;
};

using Relation = std::optional<std::string> (*)(const RelationInput &);

/* A failed store-exclusive clears the monitor; the retained monitor payload
 * is opaque, so only its identity and size are compared. */
std::optional<std::string> exclusive_store_failure(const RelationInput &in) {
  auto normalized = in.actual;
  normalized.exclusive_value = in.expected.exclusive_value;
  normalized.exclusive_value2 = in.expected.exclusive_value2;
  normalized.exclusive_version = in.expected.exclusive_version;
  return first_architecture_difference(in.expected, normalized);
}

/* A system-register read of the host counter is excluded from equality. */
std::optional<std::string> ignore_counter_register(const RelationInput &in) {
  auto expected = in.expected;
  for (const auto word : in.program.words) {
    const unsigned destination = word.word & 31U;
    if ((word.word & 0xfff0'0000U) == 0xd530'0000U && destination != 31U)
      expected.registers[destination] = in.actual.registers[destination];
  }
  return first_architecture_difference(expected, in.actual);
}

/* MRS CNTFRQ_EL0 reads the frequency of the host that runs the case, 24 MHz
 * on an M1 and 1 GHz on an M4. The runner reads it with the host's own
 * instruction, not through an engine, and expects it in the destination. */
std::optional<std::string> host_counter_frequency(const RelationInput &in) {
  std::uint64_t frequency{};
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
  auto expected = in.expected;
  for (const auto word : in.program.words) {
    const unsigned destination = word.word & 31U;
    if ((word.word & ~31U) == 0xd53b'e000U && destination != 31U)
      expected.registers[destination] = frequency;
  }
  return first_architecture_difference(expected, in.actual);
}

[[nodiscard]] Relation relation_for(std::string_view name) {
  static const std::map<std::string_view, Relation> relations{
      {"host-counter-frequency/v1", host_counter_frequency},
      {"ignore-host-counter-register/v1", ignore_counter_register},
      {"permitted-exclusive-store-failure/v1", exclusive_store_failure},
  };
  if (const auto found = relations.find(name); found != relations.end())
    return found->second;
  throw CorpusError{"the runner does not implement validator relation " +
                    std::string{name}};
}

/* ------------------------------------------------------------------------ */
/* Corpus                                                                     */

struct Corpus final {
  std::vector<std::byte> body;
  std::string meta;
  std::vector<ArmFeatureSet> native_sets;
  std::vector<std::string_view> tokens;
  std::vector<std::string_view> validator_names;
  std::vector<Relation> relations;
  std::vector<std::span<const std::byte>> blobs;
  std::vector<Record> records;
};

[[nodiscard]] std::vector<std::byte> read_file(const char *path) {
  std::ifstream input{path, std::ios::binary};
  if (!input)
    throw CorpusError{std::string{"cannot open "} + path};
  input.seekg(0, std::ios::end);
  const auto size = static_cast<std::size_t>(input.tellg());
  input.seekg(0, std::ios::beg);
  std::vector<std::byte> data(size);
  if (!input.read(reinterpret_cast<char *>(data.data()),
                  static_cast<std::streamsize>(size)))
    throw CorpusError{std::string{"cannot read "} + path};
  return data;
}

[[nodiscard]] std::vector<std::byte> inflate_pack(const char *path) {
  const auto file = read_file(path);
  Reader header{file};
  const auto magic = header.bytes(pack_magic.size());
  if (!std::equal(magic.begin(), magic.end(), pack_magic.begin(),
                  [](std::byte left, char right) {
                    return std::to_integer<char>(left) == right;
                  }))
    throw CorpusError{"not an A64 instruction corpus pack"};
  if (header.u32() != pack_version || header.u32() != 0U)
    throw CorpusError{"unsupported corpus pack version"};
  const auto size = header.u64();
  std::vector<std::byte> body(static_cast<std::size_t>(size));
  auto length = static_cast<uLongf>(size);
  const auto compressed = std::span{file}.subspan(header.offset());
  if (uncompress(reinterpret_cast<Bytef *>(body.data()), &length,
                 reinterpret_cast<const Bytef *>(compressed.data()),
                 static_cast<uLong>(compressed.size())) != Z_OK ||
      length != size)
    throw CorpusError{"corrupt corpus pack body"};
  return body;
}

[[nodiscard]] ArmFeatureSet
feature_set(std::uint16_t index, const std::vector<std::string_view> &tokens) {
  if (index >= tokens.size())
    throw CorpusError{"native feature index out of range"};
  const auto &known = ::uml::platform::arm_feature_tokens;
  const auto found = std::find(known.begin(), known.end(), tokens[index]);
  if (found == known.end())
    throw CorpusError{"the runtime does not define feature " +
                      std::string{tokens[index]}};
  ArmFeatureSet result;
  result.set(static_cast<std::size_t>(found - known.begin()));
  return result;
}

void validate_case(const Record &record, const CorpusCase &test_case,
                   const Corpus &corpus) {
  const auto fail = [&](std::string_view message) {
    throw CorpusError{std::string{record.name} + ": " +
                      std::string{test_case.name} + ": " +
                      std::string{message}};
  };
  if (test_case.program >= record.programs.size())
    fail("program index out of range");
  for (const auto word : record.programs[test_case.program].words)
    if (word.offset > page_size - 4U)
      fail("program word lies outside the code page");
  if (test_case.native >= corpus.native_sets.size())
    fail("native set index out of range");
  if (test_case.validator != no_validator &&
      test_case.validator >= corpus.relations.size())
    fail("validator index out of range");
  if (test_case.retired.kind == Retirement::Kind::pattern &&
      (test_case.retired.step == 0U ||
       test_case.retired.maximum < test_case.retired.minimum))
    fail("invalid retirement pattern");
  for (std::size_t index = 0; index < test_case.memory.size(); ++index) {
    const auto &region = test_case.memory[index];
    if (region.blob >= corpus.blobs.size() ||
        corpus.blobs[region.blob].size() != region.size)
      fail("memory blob does not match its region");
    if (region.size == 0U || region.size % page_size != 0U ||
        region.address % page_size != 0U ||
        region.address > ~std::uint64_t{0} - region.size)
      fail("invalid memory-region shape");
    if (region.address < code_base + page_size &&
        code_base < region.address + region.size)
      fail("memory region overlaps code");
    for (std::size_t prior = 0; prior < index; ++prior) {
      const auto &other = test_case.memory[prior];
      if (region.address < other.address + other.size &&
          other.address < region.address + region.size)
        fail("memory regions overlap");
    }
  }
}

[[nodiscard]] Corpus load_corpus(const char *path) {
  Corpus corpus;
  corpus.body = inflate_pack(path);
  const std::span<const std::byte> body{corpus.body};
  Reader reader{body};
  corpus.meta = std::string{reader.string32()};
  const auto token_count = reader.u16();
  for (unsigned index = 0; index < token_count; ++index)
    corpus.tokens.push_back(reader.string16());
  const auto native_count = reader.u16();
  for (unsigned index = 0; index < native_count; ++index) {
    ArmFeatureSet set;
    const auto features = reader.u8();
    for (unsigned feature = 0; feature < features; ++feature)
      set |= feature_set(reader.u16(), corpus.tokens);
    corpus.native_sets.push_back(set);
  }
  const auto validator_count = reader.u16();
  for (unsigned index = 0; index < validator_count; ++index) {
    corpus.validator_names.push_back(reader.string16());
    corpus.relations.push_back(relation_for(corpus.validator_names.back()));
  }
  const auto blob_count = reader.u32();
  for (std::uint32_t index = 0; index < blob_count; ++index)
    corpus.blobs.push_back(reader.bytes(reader.u32()));
  const auto record_count = reader.u32();
  corpus.records.reserve(record_count);
  for (std::uint32_t index = 0; index < record_count; ++index) {
    auto &record = corpus.records.emplace_back();
    record.name = reader.string16();
    record.producer = reader.string16();
    record.site = reader.string16();
    record.profiles = reader.u8();
    if (record.profiles != record_profiles)
      throw CorpusError{std::string{record.name} +
                        ": not a 4 KiB profile record"};
    record.options = reader.u16();
    record.svl = reader.u16();
    if ((record.options & retired_options) != 0U)
      throw CorpusError{std::string{record.name} + ": retired harness option"};
    if ((record.options & ~known_options) != 0U)
      throw CorpusError{std::string{record.name} + ": unknown harness option"};
    if (record.svl != 0U && !valid_svl(record.svl))
      throw CorpusError{std::string{record.name} + ": invalid SVL"};
    const auto program_count = reader.u32();
    record.programs.reserve(program_count);
    for (std::uint32_t program = 0; program < program_count; ++program) {
      auto &entry = record.programs.emplace_back();
      entry.entry = reader.u16();
      const auto words = reader.u16();
      bool found_entry = false;
      for (unsigned word = 0; word < words; ++word) {
        const ProgramWord item{.offset = reader.u16(), .word = reader.u32()};
        if (item.offset % 4U != 0U)
          throw CorpusError{std::string{record.name} + ": invalid program"};
        for (const auto prior : entry.words)
          if (prior.offset == item.offset)
            throw CorpusError{std::string{record.name} +
                              ": duplicate program word"};
        if (item.offset == entry.entry) {
          entry.entry_word = item.word;
          found_entry = true;
        }
        entry.words.push_back(item);
      }
      if (!found_entry)
        throw CorpusError{std::string{record.name} +
                          ": program entry word is absent"};
    }
    const auto case_count = reader.u32();
    record.cases.reserve(case_count);
    for (std::uint32_t item = 0; item < case_count; ++item) {
      auto &test_case = record.cases.emplace_back();
      test_case.program = reader.u32();
      test_case.name = reader.string16();
      test_case.ordinal_16k = reader.u32();
      test_case.ordinal_4k = reader.u32();
      const auto event = reader.u8();
      if (event >= event_names.size() || event == retired_event)
        throw CorpusError{std::string{record.name} + ": unknown event"};
      test_case.event = static_cast<Event>(event);
      const auto flags = reader.u8();
      test_case.invalid_encoding = (flags & 1U) != 0U;
      test_case.compare_fault = (flags & 2U) != 0U;
      switch (reader.u8()) {
      case 0:
        test_case.retired = {.kind = Retirement::Kind::exact,
                             .minimum = reader.u32()};
        break;
      case 1: {
        const auto minimum = reader.u32();
        const auto step = reader.u32();
        const auto maximum = reader.u32();
        test_case.retired = {.kind = Retirement::Kind::pattern,
                             .minimum = minimum,
                             .step = step,
                             .maximum = maximum};
        break;
      }
      case 2:
        test_case.retired = {.kind = Retirement::Kind::unchecked};
        break;
      default:
        throw CorpusError{std::string{record.name} +
                          ": unknown retirement kind"};
      }
      if (test_case.compare_fault) {
        test_case.fault_address = reader.u64();
        const auto access = reader.u8();
        if (access >= access_names.size())
          throw CorpusError{std::string{record.name} + ": unknown access"};
        test_case.fault_access = static_cast<MemoryAccess>(access);
      }
      if (test_case.event == Event::floating_point_trap)
        test_case.syndrome = reader.u32();
      test_case.native = reader.u16();
      test_case.validator = reader.u16();
      test_case.initial = sparse_state(reader, body, record.svl);
      test_case.expected = sparse_state(reader, body, record.svl);
      const auto regions = reader.u8();
      for (unsigned region_index = 0; region_index < regions; ++region_index) {
        auto &region = test_case.memory.emplace_back();
        region.address = reader.u64();
        region.size = reader.u32();
        region.flags = reader.u8();
        region.blob = reader.u32();
        region.run_count = reader.u16();
        const auto runs_begin = reader.offset();
        for (unsigned run = 0; run < region.run_count; ++run) {
          const auto offset = reader.u32();
          const auto length = reader.u16();
          if (offset > region.size || length > region.size - offset)
            throw CorpusError{std::string{record.name} +
                              ": memory run out of range"};
          (void)reader.bytes(length);
        }
        region.runs = body.subspan(runs_begin, reader.offset() - runs_begin);
        /* The two reserved allocation-tag list counts. */
        if (reader.u16() != 0U || reader.u16() != 0U)
          throw CorpusError{std::string{record.name} +
                            ": retired allocation tags"};
      }
      validate_case(record, test_case, corpus);
    }
  }
  if (!reader.done())
    throw CorpusError{"trailing corpus pack data"};
  return corpus;
}

/* ------------------------------------------------------------------------ */
/* Guest mapping                                                              */

[[noreturn]] void host_failure(std::string_view what) {
  throw std::runtime_error{std::string{what} + ": " + std::strerror(errno)};
}

/* An unnamed POSIX shared-memory object with a host view. */
class SharedBacking final {
public:
  explicit SharedBacking(std::size_t size) : size_{size} {
    static std::uint64_t serial = 0;
    char name[32];
    std::snprintf(name, sizeof(name), "/a64c.%d.%" PRIu64,
                  static_cast<int>(::getpid()), ++serial);
    descriptor_ = ::shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (descriptor_ < 0)
      host_failure("shm_open");
    (void)::shm_unlink(name);
    if (::ftruncate(descriptor_, static_cast<off_t>(size)) != 0)
      host_failure("ftruncate");
    void *view = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                        descriptor_, 0);
    if (view == MAP_FAILED)
      host_failure("mmap");
    view_ = static_cast<std::byte *>(view);
  }

  ~SharedBacking() {
    if (view_ != nullptr)
      (void)::munmap(view_, size_);
    if (descriptor_ >= 0)
      (void)::close(descriptor_);
  }

  SharedBacking(const SharedBacking &) = delete;
  SharedBacking &operator=(const SharedBacking &) = delete;

  [[nodiscard]] int descriptor() const noexcept { return descriptor_; }
  [[nodiscard]] std::span<std::byte> view() const noexcept {
    return {view_, size_};
  }

private:
  std::size_t size_;
  int descriptor_{-1};
  std::byte *view_{};
};

class AddressSpace final {
public:
  AddressSpace() {
    if (darwin_uml_static_vm_activate(generation) != 0)
      throw std::runtime_error{"cannot activate the guest address space"};
  }
  ~AddressSpace() { (void)darwin_uml_static_vm_destroy(generation); }

  AddressSpace(const AddressSpace &) = delete;
  AddressSpace &operator=(const AddressSpace &) = delete;
};

class GuestMapping final {
public:
  GuestMapping(std::uint64_t address, const SharedBacking &backing,
               std::size_t size, int protection, bool guest_code,
               bool guest_writable)
      : address_{address}, size_{size} {
    if (darwin_uml_static_vm_map_generation(
            generation, reinterpret_cast<void *>(address), backing.descriptor(),
            0, size, protection, guest_code ? 1 : 0,
            guest_writable ? 1 : 0) != 0)
      throw std::runtime_error{"cannot map guest region " + hex(address)};
  }

  ~GuestMapping() {
    (void)darwin_uml_static_vm_unmap_generation(
        generation, reinterpret_cast<void *>(address_), size_);
  }

  GuestMapping(const GuestMapping &) = delete;
  GuestMapping &operator=(const GuestMapping &) = delete;

private:
  std::uint64_t address_;
  std::size_t size_;
};

/* Bytes of guest memory: where a store-exclusive writes. */
struct Footprint final {
  std::uint64_t address{};
  std::size_t size{};
};

/* One declared data region, mapped for the lifetime of one case. */
class CaseRegion final {
public:
  CaseRegion(const RegionSpec &spec, std::span<const std::byte> initial)
      : spec_{spec}, initial_{initial}, backing_{spec.size},
        mapping_{spec.address,
                 backing_,
                 spec.size,
                 (spec.readable() ? PROT_READ : 0) |
                     (spec.writable() ? PROT_WRITE : 0) |
                     (spec.executable() ? PROT_EXEC : 0),
                 spec.guest_code(),
                 spec.guest_writable()} {
    if (spec.run_count != 0U) {
      expected_.assign(initial.begin(), initial.end());
      Reader runs{spec.runs};
      for (unsigned run = 0; run < spec.run_count; ++run) {
        const auto offset = runs.u32();
        const auto data = runs.bytes(runs.u16());
        std::copy(data.begin(), data.end(), expected_.begin() + offset);
      }
    }
  }

  CaseRegion(const CaseRegion &) = delete;
  CaseRegion &operator=(const CaseRegion &) = delete;

  void restore() const {
    std::copy(initial_.begin(), initial_.end(), backing_.view().begin());
  }

  /* The region's bytes as the case started, as the case expects them to end,
   * and as they are now. */
  [[nodiscard]] std::span<const std::byte> initial() const noexcept {
    return initial_;
  }
  [[nodiscard]] std::span<const std::byte> expected() const noexcept {
    return expected_.empty() ? initial_
                             : std::span<const std::byte>{expected_};
  }
  [[nodiscard]] std::span<std::byte> live() const noexcept {
    return backing_.view();
  }

  [[nodiscard]] std::uint64_t address() const noexcept {
    return spec_.address;
  }

  /* Whether `size` bytes at `address` lie in the region. */
  [[nodiscard]] bool contains(std::uint64_t address,
                              std::size_t size) const noexcept {
    return address >= spec_.address && address - spec_.address <= spec_.size &&
           size <= spec_.size - (address - spec_.address);
  }

  /* The bytes that differ from what the case expects, naming the first.
   * `unwritten`, for a lost store-exclusive, expects its bytes as the case
   * started with them. */
  [[nodiscard]] std::optional<std::string>
  check(const Footprint *unwritten = nullptr) const {
    const auto actual = backing_.view();
    std::vector<std::byte> lost;
    std::span<const std::byte> expected = this->expected();
    if (unwritten != nullptr && contains(unwritten->address, unwritten->size)) {
      lost.assign(expected.begin(), expected.end());
      const auto offset = unwritten->address - spec_.address;
      std::copy_n(initial_.begin() + static_cast<std::ptrdiff_t>(offset),
                  unwritten->size,
                  lost.begin() + static_cast<std::ptrdiff_t>(offset));
      expected = lost;
    }
    const auto mismatch =
        std::mismatch(actual.begin(), actual.end(), expected.begin());
    if (mismatch.first != actual.end()) {
      const auto offset =
          static_cast<std::uint64_t>(mismatch.first - actual.begin());
      return "memory at " + hex(spec_.address + offset) + " expected " +
             hex(std::to_integer<unsigned>(*mismatch.second)) + " actual " +
             hex(std::to_integer<unsigned>(*mismatch.first));
    }
    return std::nullopt;
  }

private:
  const RegionSpec &spec_;
  std::span<const std::byte> initial_;
  SharedBacking backing_;
  GuestMapping mapping_;
  std::vector<std::byte> expected_;
};

/* ------------------------------------------------------------------------ */
/* Engines                                                                    */

/* The production engines. */
enum class Lane : std::uint8_t { gadgets, jit };

[[nodiscard]] std::string_view lane_name(Lane lane) noexcept {
  switch (lane) {
  case Lane::gadgets:
    return "gadgets";
  case Lane::jit:
    return "jit";
  }
  return "?";
}

/* How much of the SME profile a lane's engine implements. */
[[nodiscard]] constexpr arm64::SmeSupport lane_sme_support(Lane lane) noexcept {
  switch (lane) {
  case Lane::gadgets:
    return arm64::gadgets::sme_support;
  case Lane::jit:
    return arm64::jit::sme_support;
  }
  return arm64::SmeSupport::none;
}

/* The SME profile's tokens, which without_sme removes. */
constexpr std::array<ArmFeature, 4> sme_features{
    ArmFeature::sme, ArmFeature::sme2, ArmFeature::sme_f64f64,
    ArmFeature::sme_i16i64};

struct Outcome final {
  Event event{Event::fatal};
  arm64::CpuState cpu;
  std::size_t instructions{};
  MemoryFault fault{};
  std::uint32_t syndrome{};
};

/* The production engines for one harness option set. */
class Engines final {
public:
  Engines(std::uint16_t options, ::uml::platform::HostCapabilities host)
      : host_{std::move(host)} {
    if ((options & without_afp_option) != 0U)
      host_.supported.reset(static_cast<std::size_t>(ArmFeature::afp));
    if ((options & without_sme_option) != 0U) {
      for (const auto feature : sme_features)
        host_.supported.reset(static_cast<std::size_t>(feature));
      host_.sme_svl_bytes = 0;
    }
    /* No virtual CPU has SVE, whatever the host reports. */
    host_ = ::uml::platform::without_sve(std::move(host_));
    gadgets_ = Gadgets{options, host_};
    if (::uml::cpu::cpu_engine_available(EngineKind::jit))
      jit_ = ::uml::cpu::make_cpu_engine(
          EngineKind::jit, ::uml::cpu::EngineConfiguration{
                               .host_capabilities = host_,
                           });
  }

  [[nodiscard]] bool jit_available() const noexcept { return jit_ != nullptr; }

  /* The host the engines translate for. */
  [[nodiscard]] const ::uml::platform::HostCapabilities &host() const noexcept {
    return host_;
  }

  [[nodiscard]] Outcome run(Lane lane, const arm64::CpuState &initial,
                            StaticGuestMemory &memory) {
    Outcome outcome;
    outcome.cpu = initial;
    /* Enter as the runtime does (load_cpu): the task's vectors deferred and
     * the register file stale, so a program that touches the vectors without
     * being marked as doing so loses its writes, and read them back as the
     * context switch saves them. */
    auto saved = initial.vectors;
    outcome.cpu.defer_fp_load(saved.data(), initial.fpcr, initial.fpsr,
                              ~std::uint32_t{0});
    outcome.cpu.vectors = {};
    const auto result = run_engine(lane, outcome.cpu, memory, execution_limit);
    if (outcome.cpu.fp_dirty)
      saved = outcome.cpu.vectors;
    outcome.cpu.fpcr = outcome.cpu.effective_fpcr();
    outcome.cpu.fpsr =
        (outcome.cpu.effective_fpsr() & arm64::fpsr_writable_mask) |
        (initial.fpsr & ~arm64::fpsr_writable_mask);
    outcome.cpu.vectors = saved;
    outcome.cpu.fp_loaded = true;
    outcome.event = event_for(result.reason);
    outcome.instructions = result.retired_instructions;
    outcome.fault = {result.fault.address, result.fault.access};
    outcome.syndrome = result.syndrome;
    return outcome;
  }

private:
  [[nodiscard]] ::uml::cpu::ExecutionResult
  run_engine(Lane lane, arm64::CpuState &cpu, StaticGuestMemory &memory,
             std::size_t budget) {
    const auto sequences = darwin_uml_static_vm_sequences(generation);
    memory.prepare(generation, sequences.mapping_changes);
    ::uml::cpu::ExecutionRequest request{
        .cpu = cpu,
        .memory = memory,
        .translated_memory = &memory,
        .generation = generation,
        .instruction_budget = budget,
    };
    if (lane == Lane::jit)
      return jit_->run(request);
    return gadgets_->run(request);
  }

  /* A gadget engine for one virtual CPU. Options that force a portable
   * gadget body select the gadget executor directly, as the harness that
   * produced those cases did. */
  class Gadgets final {
  public:
    Gadgets(std::uint16_t options,
            const ::uml::platform::HostCapabilities &host) {
      if ((options & 0x3fcU) == 0U) {
        engine_ = ::uml::cpu::make_cpu_engine(
            EngineKind::gadgets, ::uml::cpu::EngineConfiguration{
                                     .host_capabilities = host,
                                 });
        return;
      }
      forced_ = std::make_unique<arm64::gadgets::GadgetExecutor>(
          arm64::gadgets::BuildOptions{
              .force_software_checksum = (options & 4U) != 0U,
              .force_software_flags = (options & 8U) != 0U,
              .force_software_sha3 = (options & 16U) != 0U,
              .force_software_fixed_transforms = (options & 32U) != 0U,
              .force_software_aes = (options & 64U) != 0U,
              .force_software_pmull = (options & 128U) != 0U,
              .force_software_dotprod = (options & 256U) != 0U,
              .force_software_i8mm = (options & 512U) != 0U,
              .host_capabilities = host,
          });
    }

    [[nodiscard]] ::uml::cpu::ExecutionResult
    run(::uml::cpu::ExecutionRequest &request) {
      if (forced_ != nullptr)
        return arm64::detail::run_translated(*forced_, request);
      return engine_->run(request);
    }

  private:
    std::unique_ptr<::uml::cpu::CpuEngine> engine_;
    std::unique_ptr<arm64::gadgets::GadgetExecutor> forced_;
  };

  ::uml::platform::HostCapabilities host_;
  std::optional<Gadgets> gadgets_;
  std::unique_ptr<::uml::cpu::CpuEngine> jit_;
};

/* ------------------------------------------------------------------------ */
/* Store-exclusives that lose their reservation                               */

/* A store-exclusive may fail without cause, as the architecture allows. The
 * engines run the host's own exclusives and the host's monitor ends at any
 * interrupt or preemption between the load and the store, so under load one
 * lands in that window now and then (a gadget stream has about twenty host
 * instructions between the pair, the JIT's fused pair none). A guest loops on
 * the status, but a store that discards it (STXR WZR, ...) cannot, so the
 * corpus runs it once. A case of the one straight-line shape below whose only
 * difference from the pack is that its store wrote nothing runs again, up to
 * exclusive_attempts times in all, and must then agree completely: a store
 * that never writes, writes other bytes, or writes elsewhere still fails. A
 * single lost attempt whose store did write the bytes it started with cannot
 * be told from a loss, so a transient omission of that kind is not caught. */

/* The store-exclusive of a program and the bytes it writes when it succeeds. */
struct ExclusiveStore final {
  /* Rs, the register that receives the status; 31 discards it. */
  unsigned status{};
  Footprint written;
};

/* The one store-exclusive in `program`, with the bytes at its base register's
 * final value (the register must be what the case started with, and the
 * address loses its tag under top-byte ignore). Nothing for a program with
 * none or several. */
[[nodiscard]] std::optional<ExclusiveStore>
exclusive_store(const Program &program, const arm64::CpuState &initial,
                const arm64::CpuState &expected) {
  std::optional<ExclusiveStore> found;
  for (const auto item : program.words) {
    const auto word = item.word;
    std::size_t size = 0;
    if ((word & 0x3fe0'7c00U) == 0x0800'7c00U) /* STXR, STLXR */
      size = std::size_t{1} << (word >> 30);
    else if ((word & 0xbfe0'0000U) == 0x8820'0000U) /* STXP, STLXP */
      size = std::size_t{8} << ((word >> 30) & 1U);
    else
      continue;
    if (found.has_value())
      return std::nullopt;
    const auto base = (word >> 5) & 31U;
    const auto pointer = [&](const arm64::CpuState &state) {
      return base == 31U ? state.sp : state.registers[base];
    };
    if (pointer(initial) != pointer(expected))
      return std::nullopt;
    const std::uint64_t address = initial.tbi_enabled
                                      ? pointer(expected) & 0x00ff'ffff'ffff'ffffULL
                                      : pointer(expected);
    found = ExclusiveStore{(word >> 16) & 31U, {address, size}};
  }
  return found;
}

/* The one program shape in which a store-exclusive is proved to execute
 * against the case's base register: four consecutive words from the entry,
 * `ldxr/ldxp; stxr/stxp; clrex; svc`. Nothing in it branches, so the retired
 * four instructions are these four; the load cannot change the base the
 * store uses. Retirement alone proves nothing: a branch over the store and a
 * plain store in its place retire as many. Every other shape stays strict. */
[[nodiscard]] bool straight_exclusive_pair(const Program &program,
                                           const Retirement &retired) {
  constexpr std::size_t length = 4;
  if (program.words.size() != length ||
      retired.kind != Retirement::Kind::exact || retired.minimum != length)
    return false;
  std::uint32_t words[length]{};
  for (std::size_t index = 0; index < length; ++index) {
    const auto found =
        std::ranges::find_if(program.words, [&](const ProgramWord &item) {
          return item.offset == program.entry + 4U * index;
        });
    if (found == program.words.end())
      return false;
    words[index] = found->word;
  }
  const bool scalar_load = (words[0] & 0x3fe0'7c00U) == 0x0840'7c00U;
  const bool pair_load = (words[0] & 0xbfe0'0000U) == 0x8860'0000U;
  if (!scalar_load && !pair_load)
    return false;
  const bool scalar_store = (words[1] & 0x3fe0'7c00U) == 0x0800'7c00U;
  const bool pair_store = (words[1] & 0xbfe0'0000U) == 0x8820'0000U;
  if (!scalar_store && !pair_store)
    return false;
  const auto base = (words[1] >> 5) & 31U;
  /* The load writes Rt and, for a pair, Rt2; SP is not a destination. */
  const bool base_kept = base == 31U ||
                         ((words[0] & 31U) != base &&
                          (!pair_load || ((words[0] >> 10) & 31U) != base));
  return ((words[0] >> 5) & 31U) == base && base_kept &&
         (words[2] & 0xffff'f0ffU) == 0xd503'305fU &&  /* CLREX */
         (words[3] & 0xffe0'001fU) == 0xd400'0001U;    /* SVC */
}

/* The bytes a case's lost store-exclusive leaves unwritten, when the case is
 * the one-attempt shape: the straight-line pair above, completing at its
 * system call, whose store discards its status, so the loss changes nothing
 * but those bytes. Nothing for any other case, which a guest's own loop or an
 * expected failure already covers. */
[[nodiscard]] std::optional<Footprint>
lost_store(const CorpusCase &test_case, const Program &program,
           const arm64::CpuState &initial, const arm64::CpuState &expected) {
  if (test_case.event != Event::system_call ||
      test_case.validator != no_validator ||
      !straight_exclusive_pair(program, test_case.retired))
    return std::nullopt;
  const auto store = exclusive_store(program, initial, expected);
  if (!store.has_value() || store->status != 31U)
    return std::nullopt;
  return store->written;
}

/* Faults --inject makes an engine lane commit on purpose, in the first
 * attempt at each case of the one-attempt shape, to prove what the retry
 * takes for a lost reservation and what it refuses. A plan that is `retried`
 * must be taken for one, run again and pass; any other must fail on the
 * attempt it was made in, and `every_attempt` makes it in all of them. */
enum class Fault : std::uint8_t {
  lost,
  lost_and_register,
  lost_and_elsewhere,
  wrong_bytes,
  partly_written,
  never_written,
};

struct FaultPlan final {
  Fault fault;
  const char *name;
  bool retried;
  bool every_attempt;
};

constexpr FaultPlan fault_plans[] = {
    /* The store reports nothing and wrote nothing. */
    {Fault::lost, "lost", true, false},
    /* A loss with another register wrong. */
    {Fault::lost_and_register, "lost+register", false, false},
    /* A loss with a byte outside the store's changed. */
    {Fault::lost_and_elsewhere, "lost+elsewhere", false, false},
    /* The store wrote, and wrote other bytes. */
    {Fault::wrong_bytes, "wrong-bytes", false, false},
    /* The store wrote some of its bytes. */
    {Fault::partly_written, "partly-written", false, false},
    /* A store that never writes: every attempt is a loss. */
    {Fault::never_written, "never-written", false, true},
};

/* The region that holds the bytes of a lost store, and which of them the case
 * changes. */
struct StoreBytes final {
  const CaseRegion *region{};
  std::size_t offset{};
  std::vector<std::size_t> changed;
};

[[nodiscard]] StoreBytes
store_bytes(const Footprint &store,
            const std::vector<std::unique_ptr<CaseRegion>> &regions) {
  StoreBytes bytes;
  const auto found = std::ranges::find_if(regions, [&](const auto &region) {
    return region->contains(store.address, store.size);
  });
  if (found == regions.end())
    return bytes;
  bytes.region = found->get();
  bytes.offset =
      static_cast<std::size_t>(store.address - bytes.region->address());
  const auto initial = bytes.region->initial();
  const auto expected = bytes.region->expected();
  for (std::size_t byte = bytes.offset; byte < bytes.offset + store.size;
       ++byte)
    if (initial[byte] != expected[byte])
      bytes.changed.push_back(byte);
  return bytes;
}

/* Whether `fault` can be made in a case whose lost store is `store`: the
 * store changes a byte, and two for a fault that writes only some of them. */
[[nodiscard]] bool
fault_fits(Fault fault, const Footprint &store,
           const std::vector<std::unique_ptr<CaseRegion>> &regions) {
  const auto bytes = store_bytes(store, regions);
  return bytes.region != nullptr &&
         bytes.changed.size() >= (fault == Fault::partly_written ? 2U : 1U);
}

/* Makes the run `outcome`, a correct run of a case whose lost store is
 * `store`, one that ended with `fault` (see fault_fits). */
void inject_fault(Fault fault, const Footprint &store,
                  const std::vector<std::unique_ptr<CaseRegion>> &regions,
                  Outcome &outcome) {
  const auto bytes = store_bytes(store, regions);
  const auto live = bytes.region->live();
  const auto initial = bytes.region->initial();
  const auto expected = bytes.region->expected();
  /* Puts the store's bytes back to what the case started with, except the
   * changed byte `keep` if there is one, which the store wrote. Whatever the
   * engine did, the outcome is then what the fault says. */
  const auto put_back = [&](std::optional<std::size_t> keep) {
    for (std::size_t byte = bytes.offset; byte < bytes.offset + store.size;
         ++byte)
      live[byte] = keep.has_value() && byte == *keep ? expected[byte]
                                                     : initial[byte];
  };
  switch (fault) {
  case Fault::lost:
  case Fault::never_written:
    put_back(std::nullopt);
    break;
  case Fault::lost_and_register:
    put_back(std::nullopt);
    outcome.cpu.registers[28] ^= 1U;
    break;
  case Fault::lost_and_elsewhere: {
    put_back(std::nullopt);
    const auto other = (bytes.offset + store.size) % live.size();
    live[other] = ~live[other];
    break;
  }
  case Fault::wrong_bytes:
    for (const auto byte : bytes.changed)
      live[byte] = expected[byte] ^ std::byte{0x5a};
    break;
  case Fault::partly_written:
    put_back(bytes.changed.front());
    break;
  }
}

/* A structural guard against an unretried flake: a case that completes with
 * exactly one store-exclusive at a stable base, whose status register ends 0
 * and whose retirement is not a pattern, is refused, because it is run once
 * and lost now and then. It does not prove a pack's control flow: a program
 * with several store-exclusives, a changed base, an overwritten status or a
 * pattern retirement is not examined. The other shapes are the guest's own
 * loop, a store that discards its status (retried only in the straight-line
 * shape above) and one the case expects to fail. */
void require_exclusive_loop(const Record &record, const CorpusCase &test_case,
                            const Program &program,
                            const arm64::CpuState &initial,
                            const arm64::CpuState &expected) {
  if (test_case.event != Event::system_call ||
      test_case.retired.kind == Retirement::Kind::pattern)
    return;
  const auto store = exclusive_store(program, initial, expected);
  if (store.has_value() && store->status != 31U &&
      expected.registers[store->status] == 0U)
    throw CorpusError{std::string{record.name} + ": " +
                      std::string{test_case.name} +
                      ": a store-exclusive that reports its status and "
                      "completes must loop on it"};
}

/* ------------------------------------------------------------------------ */
/* Runner                                                                     */

struct Options final {
  const char *pack{};
  /* Feature tokens the engines treat as host features. A host without them
   * still runs every case that does not need them natively, which is how the
   * FEAT_AFP twins are exercised on hosts that cannot select them. */
  std::vector<std::string_view> assumed_features;
  /* The streaming vector length, in bytes, an assumed SME profile has. */
  unsigned assumed_sme_svl{};
  std::vector<std::string_view> records;
  std::vector<std::string_view> producers;
  std::vector<std::string_view> cases;
  std::vector<std::uint32_t> ordinals;
  bool run_gadgets{true};
  bool run_jit{true};
  std::size_t max_failures{25};
  bool list{};
  bool emitted_registers{};
  /* Interrupt the thread that runs the guest continuously, and go on until a
   * store-exclusive has lost its reservation to it. */
  bool interrupt{};
  /* Make each lane commit the faults of fault_plans, and check that a loss is
   * retried and anything else refused. */
  bool inject{};
  /* Run the selected records at least this many times. */
  std::size_t passes{1};
};

struct Counts final {
  std::size_t records{};
  std::size_t cases{};
  std::size_t gadgets{};
  std::size_t jit{};
  std::size_t jit_skipped_host_feature{};
  std::size_t gadgets_skipped_host_feature{};
  std::size_t jit_unavailable{};
  std::size_t failures{};
  /* Attempts that were a store-exclusive losing its reservation, the cases
   * they were retried in, and those that then passed. */
  std::size_t exclusive_lost{};
  std::size_t exclusive_retried{};
  std::size_t exclusive_recovered{};
  /* Under --inject, the cases a fault was committed in. */
  std::size_t injected{};
  std::map<std::string_view, std::size_t> missing_features;
  /* The features the gadget lane's virtual CPU lacks, by case. */
  std::map<std::string_view, std::size_t> gadgets_missing_features;
};

/* Whether a case needs an SVE token. */
[[nodiscard]] bool sve_case(const ::uml::cpu::ArmFeatureSet &native) {
  using ::uml::platform::ArmFeature;
  return std::ranges::any_of(
      std::array{ArmFeature::sve, ArmFeature::sve2, ArmFeature::sve_aes,
                 ArmFeature::sve_pmull128, ArmFeature::sve_sha3},
      [&](ArmFeature feature) {
        return native.test(static_cast<std::size_t>(feature));
      });
}

/* The feature a case needs that a lane's virtual CPU for `host` lacks
 * although the host may have it: FEAT_SVE for a case with an SVE
 * requirement, which no virtual CPU implements; FEAT_AFP or
 * FEAT_RPRES outside their predicates; FEAT_SME for a case with SME state or
 * an SME requirement unless the virtual CPU implements the profile, at the
 * record's SVL when the record has one, and the lane's engine is complete;
 * and FEAT_SME2 outside the SME2 profile or for an engine without the SME2
 * forms. */
[[nodiscard]] std::optional<ArmFeature>
virtual_cpu_lacks(const Record &record,
                  const ::uml::cpu::ArmFeatureSet &native,
                  const ::uml::platform::HostCapabilities &host, Lane lane) {
  if (sve_case(native))
    return ArmFeature::sve;
  const bool sme_case =
      record.svl != 0U ||
      std::ranges::any_of(sme_features, [&](ArmFeature feature) {
        return native.test(static_cast<std::size_t>(feature));
      });
  if (sme_case) {
    const auto support = lane_sme_support(lane);
    if (!arm64::implements_sme(host) ||
        (record.svl != 0U && record.svl != host.sme_svl_bytes) ||
        support < arm64::SmeSupport::complete)
      return ArmFeature::sme;
    if (native.test(static_cast<std::size_t>(ArmFeature::sme2)) &&
        (arm64::sme_profile(host) != arm64::SmeProfile::sme2 ||
         support < arm64::SmeSupport::sme2))
      return ArmFeature::sme2;
  }
  if (native.test(static_cast<std::size_t>(ArmFeature::afp)) &&
      !arm64::implements_afp(host))
    return ArmFeature::afp;
  if (native.test(static_cast<std::size_t>(ArmFeature::rpres)) &&
      !arm64::implements_rpres(host))
    return ArmFeature::rpres;
  return std::nullopt;
}

/* The probed host plus any assumed features. */
[[nodiscard]] ::uml::platform::HostCapabilities
runner_host(const Options &options) {
  auto host = ::uml::platform::apple::probed_host_capabilities();
  const auto &tokens = ::uml::platform::arm_feature_tokens;
  for (const auto token : options.assumed_features) {
    const auto found = std::ranges::find(tokens, token);
    if (found == tokens.end())
      throw CorpusError{"unknown feature token " + std::string{token}};
    host.supported.set(static_cast<std::size_t>(found - tokens.begin()));
  }
  if (options.assumed_sme_svl != 0U)
    host.sme_svl_bytes = static_cast<std::uint16_t>(options.assumed_sme_svl);
  return host;
}

class Runner final {
public:
  Runner(const Corpus &corpus, const Options &options,
         const FaultPlan *fault = nullptr)
      : corpus_{corpus}, options_{options}, fault_{fault},
        host_{runner_host(options)},
        code_backing_{page_size},
        code_mapping_{code_base, code_backing_, page_size,
                      PROT_READ | PROT_EXEC, true, false} {
    if (!::uml::cpu::cpu_engine_available(EngineKind::gadgets))
      throw std::runtime_error{"the gadget engine is unavailable"};
  }

  /* Returns false once the failure limit stops the run. */
  bool run(const std::vector<const Record *> &records) {
    std::optional<std::uint16_t> engine_options;
    std::unique_ptr<Engines> engines;
    for (const auto *record : records) {
      if (!engine_options.has_value() || *engine_options != record->options) {
        engines.reset();
        engines = std::make_unique<Engines>(record->options, host_);
        engine_options = record->options;
      }
      if (!run_record(*record, *engines))
        return false;
    }
    return true;
  }

  [[nodiscard]] const Counts &counts() const noexcept { return counts_; }

private:
  [[nodiscard]] bool selected(const CorpusCase &test_case) const {
    if (!options_.ordinals.empty() &&
        std::ranges::find(options_.ordinals, test_case.ordinal_4k) ==
            options_.ordinals.end())
      return false;
    if (options_.cases.empty())
      return true;
    return std::ranges::any_of(options_.cases, [&](std::string_view text) {
      return test_case.name.find(text) != std::string_view::npos;
    });
  }

  bool run_record(const Record &record, Engines &engines) {
    std::vector<std::vector<std::size_t>> by_program(record.programs.size());
    bool any = false;
    for (std::size_t index = 0; index < record.cases.size(); ++index)
      if (selected(record.cases[index])) {
        by_program[record.cases[index].program].push_back(index);
        any = true;
      }
    if (any)
      ++counts_.records;
    for (std::size_t program = 0; program < by_program.size(); ++program) {
      if (by_program[program].empty())
        continue;
      install(record.programs[program]);
      for (const auto index : by_program[program])
        if (!run_case(record, index, engines))
          return false;
    }
    return true;
  }

  void install(const Program &program) {
    page_.fill(std::byte{});
    for (const auto word : program.words)
      for (unsigned byte = 0; byte < 4U; ++byte)
        page_[word.offset + byte] =
            static_cast<std::byte>((word.word >> (byte * 8U)) & 0xffU);
    std::size_t first = 0;
    while (first != page_.size() && page_[first] == installed_[first])
      ++first;
    if (first == page_.size())
      return;
    std::size_t end = page_.size();
    while (end != first && page_[end - 1U] == installed_[end - 1U])
      --end;
    if (darwin_uml_static_vm_copy_from_buffer(code_base + first,
                                              page_.data() + first,
                                              end - first) != 0)
      throw std::runtime_error{"cannot install a corpus program"};
    installed_ = page_;
  }

  bool run_case(const Record &record, std::size_t index, Engines &engines) {
    const auto &test_case = record.cases[index];
    const auto &program = record.programs[test_case.program];
    ++counts_.cases;
    /* The states and regions are decoded and mapped for the first lane that
     * runs the case: SME state decodes only where the virtual CPU has it. */
    std::optional<arm64::CpuState> initial;
    std::optional<arm64::CpuState> expected;
    std::vector<std::unique_ptr<CaseRegion>> regions;
    /* A record with an SVL holds its cases' Z, P, ZA and ZT0 in the SME
     * register file's layout. */
    std::vector<std::byte> initial_sme;
    std::vector<std::byte> expected_sme;

    /* FEAT_AFP and FEAT_RPRES are implemented exactly where the virtual
     * CPU's predicates say so, and only the hardware's own floating-point
     * instructions honour them, so a case that needs either runs in neither
     * lane elsewhere. SME runs where the profile and the lane's engine
     * implement it. */
    const auto &native = corpus_.native_sets[test_case.native];

    for (const auto lane : {Lane::gadgets, Lane::jit}) {
      const auto lacking =
          virtual_cpu_lacks(record, native, engines.host(), lane);
      if (lane == Lane::gadgets) {
        if (!options_.run_gadgets)
          continue;
        if (lacking.has_value()) {
          ++counts_.gadgets_skipped_host_feature;
          ++counts_.gadgets_missing_features
                [::uml::platform::arm_feature_tokens
                     [static_cast<std::size_t>(*lacking)]];
          continue;
        }
        /* The gadget engine emulates every other feature, but it completes
         * a misaligned single-copy-atomic access with the host's own
         * FEAT_LSE2, so a case that needs LSE2 has no gadget result on a
         * host without it. */
        if (native.test(static_cast<std::size_t>(ArmFeature::lse2)) &&
            !native_host_.has(ArmFeature::lse2)) {
          ++counts_.gadgets_skipped_host_feature;
          continue;
        }
        /* An assumed feature the hardware lacks selects native gadgets the
         * hardware cannot run. */
        if ((native & engines.host().supported & ~native_host_.supported)
                .any()) {
          ++counts_.gadgets_skipped_host_feature;
          continue;
        }
      }
      if (lane == Lane::jit) {
        if (!options_.run_jit)
          continue;
        if (!engines.jit_available()) {
          ++counts_.jit_unavailable;
          continue;
        }
        if (!test_case.invalid_encoding && !native_host_.supports(native)) {
          ++counts_.jit_skipped_host_feature;
          const auto missing = native & ~native_host_.supported;
          for (std::size_t feature = 0; feature < missing.size(); ++feature)
            if (missing.test(feature))
              ++counts_.missing_features
                    [::uml::platform::arm_feature_tokens[feature]];
          continue;
        }
        if (lacking.has_value()) {
          ++counts_.jit_skipped_host_feature;
          ++counts_.missing_features[::uml::platform::arm_feature_tokens
                                         [static_cast<std::size_t>(*lacking)]];
          continue;
        }
      }
      if (!initial.has_value()) {
        initial = base_state(program.entry);
        if (record.svl != 0U)
          initial_sme = sme_base_image(record.svl);
        apply_sparse(*initial, test_case.initial, record.svl, initial_sme);
        streaming_view(*initial, initial_sme, record.svl);
        expected = *initial;
        expected_sme = initial_sme;
        apply_sparse(*expected, test_case.expected, record.svl, expected_sme);
        streaming_view(*expected, expected_sme, record.svl);
        regions.reserve(test_case.memory.size());
        for (const auto &region : test_case.memory)
          regions.push_back(std::make_unique<CaseRegion>(
              region, corpus_.blobs[region.blob]));
        require_exclusive_loop(record, test_case, program, *initial,
                               *expected);
      }
      const auto lost = lost_store(test_case, program, *initial, *expected);
      /* Under --inject only the cases a fault fits. */
      if (fault_ != nullptr &&
          (!lost.has_value() || !fault_fits(fault_->fault, *lost, regions)))
        continue;
      ++(lane == Lane::gadgets ? counts_.gadgets : counts_.jit);
      std::optional<std::string> failure;
      bool lost_attempt = false;
      unsigned attempts = 0;
      do {
        ++attempts;
        for (const auto &region : regions)
          region->restore();
        /* Each run starts from the initial SME state, as the task storage
         * the kernel would hand the engine, with an empty register file. */
        auto start = *initial;
        std::vector<std::byte> task = initial_sme;
        arm64::SmeRegisterFile file;
        if (record.svl != 0U) {
          file = arm64::SmeRegisterFile{record.svl};
          start.bind_sme_registers(file);
          start.defer_sme_load(task.data(), initial->svcr, initial->tpidr2);
        }
        auto outcome = engines.run(lane, start, memory_);
        if (record.svl != 0U)
          outcome.cpu.write_back_sme_state();
        if (fault_ != nullptr && (attempts == 1U || fault_->every_attempt)) {
          inject_fault(fault_->fault, *lost, regions, outcome);
          if (attempts == 1U)
            ++counts_.injected;
        }
        const SmeImages sme{record.svl, expected_sme, task};
        failure = check(test_case, program, *initial, *expected, outcome,
                        regions, sme);
        /* Only the store's bytes differ, as a lost reservation leaves them. */
        lost_attempt = failure.has_value() && lost.has_value() &&
                       !check(test_case, program, *initial, *expected,
                              outcome, regions, sme, &*lost);
        if (lost_attempt)
          ++counts_.exclusive_lost;
      } while (lost_attempt && attempts < exclusive_attempts);
      if (attempts > 1U) {
        ++counts_.exclusive_retried;
        if (!failure.has_value())
          ++counts_.exclusive_recovered;
      }
      if (failure.has_value()) {
        if (attempts > 1U)
          *failure += lost_attempt
                          ? " (the store-exclusive lost its reservation in "
                            "each of " + std::to_string(attempts) + " attempts)"
                          : " (attempt " + std::to_string(attempts) + ")";
        report(record, index, lane, *failure);
        if (++counts_.failures == options_.max_failures)
          return false;
      }
    }
    return true;
  }

  /* A run's SME register files: expected and actual, at the record's SVL. */
  struct SmeImages final {
    unsigned svl{};
    std::span<const std::byte> expected;
    std::span<const std::byte> actual;
  };

  [[nodiscard]] std::optional<std::string>
  check(const CorpusCase &test_case, const Program &program,
        const arm64::CpuState &initial, const arm64::CpuState &expected,
        const Outcome &outcome,
        const std::vector<std::unique_ptr<CaseRegion>> &regions,
        const SmeImages &sme, const Footprint *unwritten = nullptr) const {
    if (outcome.cpu.gadget_fp_active ||
        outcome.cpu.gadget_indirect_miss != nullptr)
      return std::string{
          "engine-private execution state escaped the public boundary"};
    if (outcome.event != test_case.event)
      return "event expected " + std::string{event_name(test_case.event)} +
             " actual " + std::string{event_name(outcome.event)} +
             " (instructions " + std::to_string(outcome.instructions) +
             ", fault " + hex(outcome.fault.address) + ")";
    const auto retired = outcome.instructions;
    switch (test_case.retired.kind) {
    case Retirement::Kind::exact:
      if (retired != test_case.retired.minimum)
        return "retired " + std::to_string(retired) +
               " instructions, expected " +
               std::to_string(test_case.retired.minimum);
      break;
    case Retirement::Kind::pattern: {
      const auto &pattern = test_case.retired;
      if (retired < pattern.minimum ||
          (pattern.maximum != unbounded && retired > pattern.maximum) ||
          (retired - pattern.minimum) % pattern.step != 0U)
        return "retired " + std::to_string(retired) +
               " instructions, expected " + std::to_string(pattern.minimum) +
               "+N*" + std::to_string(pattern.step);
      break;
    }
    case Retirement::Kind::unchecked:
      break;
    }
    /* Deferred state is part of the engine-entry contract; normalize only
     * copies after execution, exactly as the producing harness did. */
    auto compared_initial = initial;
    auto compared_expected = expected;
    auto compared_actual = outcome.cpu;
    compared_initial.access_fp_state();
    compared_expected.access_fp_state();
    compared_actual.access_fp_state();
    const auto state_failure =
        test_case.validator == no_validator
            ? first_architecture_difference(compared_expected, compared_actual)
            : corpus_.relations[test_case.validator](
                  {.initial = compared_initial,
                   .expected = compared_expected,
                   .actual = compared_actual,
                   .program = program});
    if (state_failure.has_value())
      return test_case.validator == no_validator
                 ? *state_failure
                 : std::string{corpus_.validator_names[test_case.validator]} +
                       ": " + *state_failure;
    if (sme.svl != 0U)
      if (auto failure = first_sme_register_difference(
              sme.svl, expected.svcr, sme.expected, sme.actual))
        return failure;
    if (test_case.compare_fault &&
        (outcome.fault.address != test_case.fault_address ||
         outcome.fault.access != test_case.fault_access))
      return "fault expected " +
             std::string{access_names[static_cast<std::size_t>(
                 test_case.fault_access)]} +
             " " + hex(test_case.fault_address) + " actual " +
             std::string{access_names[static_cast<std::size_t>(
                 outcome.fault.access)]} +
             " " + hex(outcome.fault.address);
    if (test_case.event == Event::floating_point_trap &&
        outcome.syndrome != test_case.syndrome)
      return "floating-point syndrome expected " + hex(test_case.syndrome) +
             " actual " + hex(outcome.syndrome);
    void *translated = nullptr;
    if (darwin_uml_static_vm_translate(generation, code_base,
                                       DARWIN_UML_STATIC_EXECUTE,
                                       &translated) !=
            DARWIN_UML_STATIC_TRANSLATED ||
        translated == nullptr)
      return std::string{"code mapping disappeared"};
    if (std::memcmp(translated, installed_.data(), installed_.size()) != 0)
      return std::string{"unexpected code-page write"};
    for (const auto &region : regions)
      if (auto failure = region->check(unwritten))
        return failure;
    return std::nullopt;
  }

  /* Names the case by record index and by the producer check ordinal that
   * exported it. */
  void report(const Record &record, std::size_t index, Lane lane,
              std::string_view message) const {
    if (fault_ != nullptr)
      return;
    const auto &test_case = record.cases[index];
    const auto &program = record.programs[test_case.program];
    std::string origin{record.producer};
    if (test_case.ordinal_4k != 0U)
      origin += " check #" + std::to_string(test_case.ordinal_4k);
    std::fprintf(stderr,
                 "FAIL %.*s case %zu \"%.*s\" (%s) entry %#010x %.*s: %.*s\n",
                 static_cast<int>(record.name.size()), record.name.data(),
                 index, static_cast<int>(test_case.name.size()),
                 test_case.name.data(), origin.c_str(), program.entry_word,
                 static_cast<int>(lane_name(lane).size()),
                 lane_name(lane).data(), static_cast<int>(message.size()),
                 message.data());
  }

  const Corpus &corpus_;
  const Options &options_;
  /* The fault each lane commits in the case it can, or none. */
  const FaultPlan *fault_;
  /* What the engines translate for, and what the hardware runs. */
  const ::uml::platform::HostCapabilities host_;
  const ::uml::platform::HostCapabilities &native_host_{
      ::uml::platform::apple::probed_host_capabilities()};
  AddressSpace address_space_;
  SharedBacking code_backing_;
  GuestMapping code_mapping_;
  StaticGuestMemory memory_;
  std::array<std::byte, page_size> page_{};
  std::array<std::byte, page_size> installed_{};
  Counts counts_;
};

/* ------------------------------------------------------------------------ */
/* Emitted registers                                                          */

/* The gadget region's parts (engine.S), each between its bound labels. */
extern "C" const char uml_arm64_gadget_bank_begin[];
extern "C" const char uml_arm64_gadget_bank_end[];
extern "C" const char uml_arm64_gadget_forward_section_begin[];
extern "C" const char uml_arm64_gadget_forward_section_end[];
extern "C" const char uml_arm64_gadget_vector_forward_section_begin[];
extern "C" const char uml_arm64_gadget_vector_forward_section_end[];
extern "C" const char uml_arm64_gadget_cold_section_begin[];
extern "C" const char uml_arm64_gadget_cold_section_end[];
extern "C" const char uml_arm64_gadget_afp_section_begin[];
extern "C" const char uml_arm64_gadget_afp_section_end[];
extern "C" const char uml_arm64_gadget_veneer_section_begin[];
extern "C" const char uml_arm64_gadget_veneer_section_end[];
extern "C" const char uml_arm64_gadget_sme_section_begin[];
extern "C" const char uml_arm64_gadget_sme_section_end[];
extern "C" const char uml_arm64_gadget_runs_section_begin[];
extern "C" const char uml_arm64_gadget_runs_section_end[];

namespace jit_test = arm64::jit::testing;

/* A host with every feature and SME at `svl` bytes, for which the JIT
 * selects each native path it has. Translations are only inspected. */
[[nodiscard]] ::uml::platform::HostCapabilities
every_feature_host(unsigned svl) {
  ::uml::platform::HostCapabilities host;
  for (std::size_t feature = 0; feature < ::uml::platform::arm_feature_count;
       ++feature)
    host.supported.set(feature);
  host.floating_point = host.advanced_simd = host.fp16 = host.cssc = true;
  host.aes = host.pmull = host.sha1 = host.sha2 = host.crc32 = true;
  host.sha3 = host.sha512 = host.jscvt_subnormal_flags_correct = true;
  host.sme_svl_bytes = static_cast<std::uint16_t>(svl);
  return host;
}

/* Darwin clears x18 when it interrupts a thread, so no host code the
 * engines run may name it. This checks every word of the gadget region and
 * the JIT's translations of the corpus:
 * - each program from each of its words, for this host;
 * - one program of each pattern of the bits outside the register fields,
 *   for this host in the other link shapes, and for a host with every
 *   feature and SME at the record's SVL (64 bytes for a record without SME
 *   state) in every SVCR mode;
 * - one word of each such pattern with each register field set to 18, for
 *   both hosts, so guest x18 reaches every operand position.
 * A word the register oracle cannot classify fails too. */
class EmittedRegisterScan final {
public:
  explicit EmittedRegisterScan(std::size_t max_failures)
      : max_failures_{max_failures} {}

  void region() {
    const std::array<std::pair<const char *, const char *>, 8> parts{{
        {uml_arm64_gadget_bank_begin, uml_arm64_gadget_bank_end},
        {uml_arm64_gadget_forward_section_begin,
         uml_arm64_gadget_forward_section_end},
        {uml_arm64_gadget_vector_forward_section_begin,
         uml_arm64_gadget_vector_forward_section_end},
        {uml_arm64_gadget_cold_section_begin,
         uml_arm64_gadget_cold_section_end},
        {uml_arm64_gadget_afp_section_begin, uml_arm64_gadget_afp_section_end},
        {uml_arm64_gadget_veneer_section_begin,
         uml_arm64_gadget_veneer_section_end},
        {uml_arm64_gadget_sme_section_begin, uml_arm64_gadget_sme_section_end},
        {uml_arm64_gadget_runs_section_begin,
         uml_arm64_gadget_runs_section_end},
    }};
    for (const auto &[begin, end] : parts) {
      if (end <= begin || (end - begin) % 4 != 0)
        throw CorpusError{"a gadget region part has no whole words"};
      const std::span words{reinterpret_cast<const std::uint32_t *>(begin),
                            static_cast<std::size_t>(end - begin) / 4U};
      for (std::size_t index = 0; index < words.size(); ++index) {
        ++region_words_;
        const auto verdict = classify(words[index]);
        if (verdict.empty())
          continue;
        Dl_info info{};
        const auto *address = begin + index * 4U;
        std::string where = hex(reinterpret_cast<std::uintptr_t>(address));
        if (dladdr(address, &info) != 0 && info.dli_sname != nullptr)
          where = std::string{info.dli_sname} + "+" +
                  hex(static_cast<std::uint64_t>(
                      address - static_cast<const char *>(info.dli_saddr)));
        fail("gadget region " + where + ": " + verdict + " " +
             hex(words[index]));
      }
    }
  }

  void translations(const std::vector<const Record *> &records) {
    const auto actual = ::uml::platform::apple::probed_host_capabilities();
    std::unordered_set<std::string> programs;
    std::unordered_set<std::string> seen;
    /* One word per pattern of everything outside the register fields. */
    constexpr std::uint32_t fields =
        (31U << 16) | (31U << 10) | (31U << 5) | 31U;
    std::map<std::uint32_t, std::uint32_t> patterns;
    for (const auto *record : records) {
      const auto every = every_feature_host(record->svl != 0U ? record->svl
                                                              : 64U);
      for (const auto &program : record->programs) {
        auto sorted = program.words;
        std::ranges::sort(sorted, {}, &ProgramWord::offset);
        std::size_t run = 0;
        while (run < sorted.size()) {
          std::size_t end = run + 1U;
          while (end < sorted.size() &&
                 sorted[end].offset == sorted[end - 1U].offset + 4U)
            ++end;
          std::vector<std::uint32_t> guest;
          for (std::size_t index = run; index < end; ++index) {
            guest.push_back(sorted[index].word);
            patterns.try_emplace(sorted[index].word & ~fields,
                                 sorted[index].word);
          }
          for (std::size_t start = 0; start < guest.size(); ++start) {
            const std::span suffix =
                std::span<const std::uint32_t>{guest}.subspan(start);
            const auto pc = code_base + sorted[run + start].offset;
            if (programs
                    .emplace(reinterpret_cast<const char *>(suffix.data()),
                             suffix.size_bytes())
                    .second)
              translate(actual, pc, suffix, Shapes::unlinked, "host");
            std::string key;
            for (const auto word : suffix) {
              const auto pattern = word & ~fields;
              key.append(reinterpret_cast<const char *>(&pattern),
                         sizeof pattern);
            }
            key.append(reinterpret_cast<const char *>(&every.sme_svl_bytes),
                       sizeof every.sme_svl_bytes);
            if (!seen.insert(std::move(key)).second)
              continue;
            translate(actual, pc, suffix, Shapes::linked, "host");
            translate(every, pc, suffix, Shapes::unlinked, "every feature");
          }
          run = end;
        }
      }
    }
    const auto every = every_feature_host(64U);
    for (const auto &[pattern, word] : patterns)
      for (const unsigned low : {0U, 5U, 10U, 16U}) {
        const auto variant =
            (word & ~(31U << low)) |
            (::uml::testing::a64::darwin_platform_register << low);
        if (variant == word)
          continue;
        const std::array guest{variant};
        translate(actual, code_base, guest, Shapes::all, "host");
        translate(every, code_base, guest, Shapes::unlinked, "every feature");
      }
  }

  [[nodiscard]] bool passed() const noexcept { return failures_ == 0U; }

  void print_summary(double elapsed) const {
    std::printf("arm64 emitted registers (%zu KiB pages): region-words=%zu "
                "translations=%zu host-words=%zu failures=%zu %.1fs\n",
                page_size / 1024U, region_words_, translations_, host_words_,
                failures_, elapsed);
  }

private:
  /* "" for a word that names no x18, else why it fails. */
  [[nodiscard]] static std::string classify(std::uint32_t word) {
    const auto names = ::uml::testing::a64::names_platform_register(word);
    if (!names.has_value())
      return "unclassified word";
    return *names ? "x18 operand" : "";
  }

  enum class Shapes { unlinked, linked, all };

  /* Translates in every SVCR mode the host has, in the link shapes asked
   * for. */
  void translate(const ::uml::platform::HostCapabilities &host,
                 std::uint64_t pc, std::span<const std::uint32_t> guest,
                 Shapes shapes, const char *host_name) {
    const ::uml::cpu::HostCapabilitiesScope scope{host};
    const bool sme = host.sme_svl_bytes != 0U;
    const std::uint8_t modes = sme ? arm64::svcr_mask : 0U;
    for (std::uint8_t mode = 0; mode <= modes; ++mode)
      for (const auto shape :
           {jit_test::LinkShape::unlinked, jit_test::LinkShape::direct,
            jit_test::LinkShape::indirect, jit_test::LinkShape::tracked}) {
        const bool unlinked = shape == jit_test::LinkShape::unlinked;
        if ((shapes == Shapes::unlinked && !unlinked) ||
            (shapes == Shapes::linked && unlinked))
          continue;
        const auto code =
            jit_test::compile_block_in_mode(pc, guest, mode, shape).code;
        ++translations_;
        const auto executable =
            std::span<const std::uint32_t>{code.words}.first(
                code.executable_words);
        host_words_ += executable.size();
        for (std::size_t index = 0; index < executable.size(); ++index) {
          const auto verdict = classify(executable[index]);
          if (verdict.empty())
            continue;
          std::string guest_words;
          for (const auto word : guest)
            guest_words += (guest_words.empty() ? "" : " ") + hex(word);
          fail("JIT translation of [" + guest_words + "] mode " +
               std::to_string(mode) + " shape " +
               std::to_string(static_cast<int>(shape)) + " (" + host_name +
               ") word " + std::to_string(index) + ": " + verdict + " " +
               hex(executable[index]));
        }
      }
  }

  void fail(const std::string &message) {
    ++failures_;
    if (max_failures_ == 0U || failures_ <= max_failures_)
      std::fprintf(stderr, "FAIL %s\n", message.c_str());
  }

  std::size_t max_failures_;
  std::size_t region_words_{};
  std::size_t translations_{};
  std::size_t host_words_{};
  std::size_t failures_{};
};

void usage() {
  std::fprintf(
      stderr,
      "usage: uml_arm64_instruction_corpus_tests PACK [options]\n"
      "  --record TEXT      run records whose name contains TEXT (repeatable)\n"
      "  --producer NAME    run records of one producer CTest (repeatable)\n"
      "  --case TEXT        run cases whose name contains TEXT (repeatable)\n"
      "  --ordinal N        run the case exported by producer check N\n"
      "                     (repeatable, with --producer)\n"
      "  --engine NAME      run only the gadgets or jit lane\n"
      "  --assume-host-feature TOKEN\n"
      "                     translate as if the host had feature TOKEN\n"
      "                     (repeatable); cases needing it natively still\n"
      "                     need the real host to have it\n"
      "  --assume-sme-svl BYTES\n"
      "                     the streaming vector length of an assumed SME\n"
      "                     profile; the SM = 0 vector-length family then runs\n"
      "                     on any host\n"
      "  --max-failures N   stop after N failures (default 25, 0: never)\n"
      "  --fail-fast        stop at the first failure\n"
      "  --list             list the selected records without running them\n"
      "  --emitted-registers\n"
      "                     check that neither the gadget region nor a JIT\n"
      "                     translation of the selected records names x18\n"
      "  --passes N         run the selected records at least N times\n"
      "  --interrupt        interrupt the thread that runs the guest with a\n"
      "                     timer signal every few microseconds, and go on past\n"
      "                     --passes until a store-exclusive has lost its\n"
      "                     reservation to it; fails if none does\n"
      "  --inject           make each lane commit on purpose the faults of\n"
      "                     fault_plans in its first attempt at a store-exclusive\n"
      "                     that discards its status, and check that a lost\n"
      "                     reservation is retried and anything else is not\n");
}

[[nodiscard]] std::optional<Options> parse_options(int argc, char **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    const auto value = [&]() -> std::optional<std::string_view> {
      if (index + 1 >= argc)
        return std::nullopt;
      return std::string_view{argv[++index]};
    };
    if (argument == "--record" || argument == "--producer" ||
        argument == "--case" || argument == "--ordinal" ||
        argument == "--engine" || argument == "--max-failures" ||
        argument == "--assume-host-feature" ||
        argument == "--assume-sme-svl" || argument == "--passes") {
      const auto text = value();
      if (!text.has_value())
        return std::nullopt;
      if (argument == "--assume-host-feature")
        options.assumed_features.push_back(*text);
      else if (argument == "--record")
        options.records.push_back(*text);
      else if (argument == "--producer")
        options.producers.push_back(*text);
      else if (argument == "--case")
        options.cases.push_back(*text);
      else if (argument == "--engine") {
        if (*text != "gadgets" && *text != "jit")
          return std::nullopt;
        options.run_gadgets = *text == "gadgets";
        options.run_jit = *text == "jit";
      } else {
        char *end = nullptr;
        const auto number = std::strtoull(argv[index], &end, 10);
        if (end == argv[index] || *end != '\0')
          return std::nullopt;
        if (argument == "--ordinal") {
          if (number == 0U || number > 0xffff'ffffULL)
            return std::nullopt;
          options.ordinals.push_back(static_cast<std::uint32_t>(number));
        } else if (argument == "--passes") {
          if (number == 0U)
            return std::nullopt;
          options.passes = static_cast<std::size_t>(number);
        } else if (argument == "--assume-sme-svl") {
          if (!arm64::valid_streaming_vector_length(
                  static_cast<unsigned>(number)) ||
              number > 256U)
            return std::nullopt;
          options.assumed_sme_svl = static_cast<unsigned>(number);
        } else {
          options.max_failures = static_cast<std::size_t>(number);
        }
      }
    } else if (argument == "--fail-fast") {
      options.max_failures = 1;
    } else if (argument == "--emitted-registers") {
      options.emitted_registers = true;
    } else if (argument == "--interrupt") {
      options.interrupt = true;
    } else if (argument == "--inject") {
      options.inject = true;
    } else if (argument == "--list") {
      options.list = true;
    } else if (!argument.starts_with("--") && options.pack == nullptr) {
      options.pack = argv[index];
    } else {
      return std::nullopt;
    }
  }
  if (options.pack == nullptr)
    return std::nullopt;
  return options;
}

/* Host interrupts on demand: an interval timer sends SIGALRM to the process
 * every few microseconds, and the process has one thread, the one that runs
 * the guest. Each delivery enters and leaves the kernel on the interrupted
 * thread, which ends the host's exclusive monitor as a timer tick, an
 * inter-processor interrupt or a preemption does on a loaded host. The timer
 * is the kernel's, so no thread of ours has to be scheduled for it to fire,
 * as one that sent the signals would. The handler does nothing and stays
 * installed after the timer stops, so that a signal already pending when it
 * does is not fatal. */
class InterruptStorm final {
public:
  InterruptStorm() {
    struct sigaction action {};
    action.sa_handler = [](int) {};
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    if (::sigaction(SIGALRM, &action, nullptr) != 0)
      host_failure("sigaction");
    struct itimerval timer {};
    timer.it_interval.tv_usec = timer.it_value.tv_usec = interval;
    if (::setitimer(ITIMER_REAL, &timer, nullptr) != 0)
      host_failure("setitimer");
  }

  ~InterruptStorm() {
    struct itimerval off {};
    (void)::setitimer(ITIMER_REAL, &off, nullptr);
  }

  InterruptStorm(const InterruptStorm &) = delete;
  InterruptStorm &operator=(const InterruptStorm &) = delete;

private:
  /* Microseconds. Shorter fills the host with timer interrupts for no more
   * losses; longer loses fewer. */
  static constexpr suseconds_t interval = 5;
};

/* Proves the retry's eligibility rule on programs of its own, as a pack
 * cannot hold the ones it must refuse: only the straight-line pair is
 * retried, and a program that retires as many instructions without reaching
 * its store-exclusive is not. Returns whether every program was classified
 * as it should be. */
[[nodiscard]] bool eligibility_self_check() {
  constexpr std::uint32_t ldxr_w4_x2 = 0x885f'7c44U;   /* ldxr w4, [x2] */
  constexpr std::uint32_t ldxr_w4_x3 = 0x885f'7c64U;   /* ldxr w4, [x3] */
  constexpr std::uint32_t ldxr_x2_x2 = 0xc85f'7c42U;   /* ldxr x2, [x2] */
  constexpr std::uint32_t ldaxp_x4_x6 = 0xc87f'9844U;  /* ldaxp x4, x6, [x2] */
  constexpr std::uint32_t ldaxp_x4_x2 = 0xc87f'8844U;  /* ldaxp x4, x2, [x2] */
  constexpr std::uint32_t stlxr = 0x881f'fc43U;        /* stlxr wzr, w3, [x2] */
  constexpr std::uint32_t stlxp = 0xc83f'fc5fU;        /* stlxp wzr, xzr, xzr, [x2] */
  constexpr std::uint32_t str_w3_x2 = 0xb900'0043U;    /* str w3, [x2] */
  constexpr std::uint32_t branch_over = 0x1400'0002U;  /* b +8 */
  constexpr std::uint32_t clrex = 0xd503'3f5fU;
  constexpr std::uint32_t svc = 0xd400'0001U;
  constexpr std::uint32_t nop = 0xd503'201fU;

  struct Shape final {
    const char *name;
    std::vector<std::uint32_t> words;
    Event event;
    Retirement retired;
    bool eligible;
  };
  const Retirement exact4{Retirement::Kind::exact, 4U};
  const std::vector<Shape> shapes{
      {"scalar pair", {ldxr_w4_x2, stlxr, clrex, svc}, Event::system_call,
       exact4, true},
      {"pair pair", {ldaxp_x4_x6, stlxp, clrex, svc}, Event::system_call,
       exact4, true},
      /* The review's counterexample: four retired instructions, the store
       * skipped, and the write done by a plain store. */
      {"branch over the store", {branch_over, stlxr, str_w3_x2, clrex, svc},
       Event::system_call, exact4, false},
      {"branch after the load", {ldxr_w4_x2, branch_over, stlxr, str_w3_x2,
                                 svc},
       Event::system_call, exact4, false},
      {"store after the system call", {ldxr_w4_x2, svc, stlxr, clrex},
       Event::system_call, exact4, false},
      {"filler between the pair", {ldxr_w4_x2, nop, stlxr, clrex, svc},
       Event::system_call, {Retirement::Kind::exact, 5U}, false},
      {"load replaces the base", {ldxr_x2_x2, stlxr, clrex, svc},
       Event::system_call, exact4, false},
      {"pair load replaces the base", {ldaxp_x4_x2, stlxp, clrex, svc},
       Event::system_call, exact4, false},
      {"load from another base", {ldxr_w4_x3, stlxr, clrex, svc},
       Event::system_call, exact4, false},
      {"no clrex", {ldxr_w4_x2, stlxr, nop, svc}, Event::system_call, exact4,
       false},
      {"no system call", {ldxr_w4_x2, stlxr, clrex, nop}, Event::system_call,
       exact4, false},
      {"retirement not exact", {ldxr_w4_x2, stlxr, clrex, svc},
       Event::system_call, {Retirement::Kind::pattern, 4U}, false},
      {"retirement of another count", {ldxr_w4_x2, stlxr, clrex, svc},
       Event::system_call, {Retirement::Kind::exact, 3U}, false},
      {"expected fault", {ldxr_w4_x2, stlxr, clrex, svc}, Event::memory_fault,
       exact4, false},
  };
  bool good = true;
  auto state = base_state(0);
  state.registers[2] = 0x5200'0200U;
  for (const auto &shape : shapes) {
    Program program;
    for (std::size_t index = 0; index < shape.words.size(); ++index)
      program.words.push_back({static_cast<std::uint16_t>(4U * index),
                               shape.words[index]});
    program.entry_word = shape.words.front();
    CorpusCase test_case;
    test_case.event = shape.event;
    test_case.retired = shape.retired;
    const auto lost = lost_store(test_case, program, state, state);
    const bool eligible = lost.has_value();
    if (eligible != shape.eligible ||
        (eligible && (lost->address != state.registers[2] ||
                      lost->size != (shape.words.front() == ldaxp_x4_x6
                                         ? 16U
                                         : 4U)))) {
      std::fprintf(stderr, "eligibility: %s is %s\n", shape.name,
                   eligible ? "retried" : "not retried");
      good = false;
    }
  }
  std::printf("  eligibility      %zu programs  %s\n", shapes.size(),
              good ? "as planned" : "NOT AS PLANNED");
  return good;
}

/* Runs each fault plan over the selected records and reports whether the
 * runner took every fault as the plan says: a lost reservation retried until
 * the case agreed, and anything else failed in the attempt it was made in. */
[[nodiscard]] bool inject_faults(const Corpus &corpus, const Options &options,
                                 const std::vector<const Record *> &records) {
  bool good = eligibility_self_check();
  auto quiet = options;
  quiet.max_failures = 0;
  for (const auto &plan : fault_plans) {
    const auto runner = std::make_unique<Runner>(corpus, quiet, &plan);
    (void)runner->run(records);
    const auto &counts = runner->counts();
    const auto run = counts.gadgets + counts.jit;
    const bool as_planned =
        run != 0U && counts.injected == run &&
        (plan.retried
             /* The attempt after the fault may itself be a real loss. */
             ? counts.failures == 0U && counts.exclusive_lost >= run &&
                   counts.exclusive_retried == run &&
                   counts.exclusive_recovered == run
             : counts.failures == run && counts.exclusive_recovered == 0U &&
                   (plan.every_attempt
                        ? counts.exclusive_lost == run * exclusive_attempts &&
                              counts.exclusive_retried == run
                        : counts.exclusive_lost == 0U &&
                              counts.exclusive_retried == 0U));
    std::printf("  inject %-16s run %3zu  failed %3zu  retried %3zu  "
                "recovered %3zu  %s\n",
                plan.name, run, counts.failures, counts.exclusive_retried,
                counts.exclusive_recovered,
                as_planned ? "as planned" : "NOT AS PLANNED");
    good = good && as_planned;
  }
  return good;
}

[[nodiscard]] bool record_selected(const Record &record,
                                   const Options &options) {
  if (!options.producers.empty() &&
      std::ranges::find(options.producers, record.producer) ==
          options.producers.end())
    return false;
  if (!options.records.empty() &&
      std::ranges::none_of(options.records, [&](std::string_view text) {
        return record.name.find(text) != std::string_view::npos;
      }))
    return false;
  return true;
}

} // namespace

int main(int argc, char **argv) {
  const auto options = parse_options(argc, argv);
  if (!options.has_value()) {
    usage();
    return 2;
  }
  try {
    const auto started = std::chrono::steady_clock::now();
    const auto corpus = load_corpus(options->pack);
    std::vector<const Record *> records;
    for (const auto &record : corpus.records)
      if (record_selected(record, *options))
        records.push_back(&record);
    /* One engine set per option combination; pack order within each. */
    std::stable_sort(records.begin(), records.end(),
                     [](const Record *left, const Record *right) {
                       return left->options < right->options;
                     });
    if (options->list) {
      for (const auto *record : records)
        std::printf("%.*s\t%zu cases\n", static_cast<int>(record->name.size()),
                    record->name.data(), record->cases.size());
      return 0;
    }
    if (records.empty()) {
      std::fprintf(stderr, "no corpus record matches the selection\n");
      return 2;
    }
    if (options->emitted_registers) {
      EmittedRegisterScan scan{options->max_failures};
      scan.region();
      scan.translations(records);
      scan.print_summary(std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - started)
                             .count());
      return scan.passed() ? 0 : 1;
    }
    if (options->inject)
      return inject_faults(corpus, *options, records) ? 0 : 1;
    /* The runner owns a full software TLB; keep it off the main stack. */
    const auto runner = std::make_unique<Runner>(corpus, *options);
    std::optional<InterruptStorm> storm;
    if (options->interrupt)
      storm.emplace();
    const auto deadline = std::chrono::steady_clock::now() + interrupt_deadline;
    bool completed = true;
    for (std::size_t pass = 1; completed; ++pass) {
      completed = runner->run(records);
      const bool enough = pass >= options->passes &&
                          (!options->interrupt ||
                           runner->counts().exclusive_lost != 0U);
      if (enough || (options->interrupt &&
                     std::chrono::steady_clock::now() >= deadline))
        break;
    }
    storm.reset();
    const auto &counts = runner->counts();
    const auto elapsed = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    std::printf("arm64 instruction corpus (%zu KiB pages): records=%zu "
                "cases=%zu gadgets=%zu gadgets-skipped-by-host-feature=%zu "
                "jit=%zu jit-skipped-by-host-feature=%zu "
                "jit-unavailable=%zu failures=%zu%s %.1fs\n",
                page_size / 1024U, counts.records, counts.cases, counts.gadgets,
                counts.gadgets_skipped_host_feature, counts.jit,
                counts.jit_skipped_host_feature, counts.jit_unavailable,
                counts.failures, completed ? "" : " (stopped at the failure limit)", elapsed);
    if (!counts.gadgets_missing_features.empty()) {
      std::printf("gadgets host-feature skips:");
      for (const auto &[feature, count] : counts.gadgets_missing_features)
        std::printf(" %.*s=%zu", static_cast<int>(feature.size()),
                    feature.data(), count);
      std::printf("\n");
    }
    if (!counts.missing_features.empty()) {
      std::printf("jit host-feature skips:");
      for (const auto &[feature, count] : counts.missing_features)
        std::printf(" %.*s=%zu", static_cast<int>(feature.size()),
                    feature.data(), count);
      std::printf("\n");
    }
    if (counts.exclusive_lost != 0U)
      std::printf("store-exclusives that lost their reservation: %zu "
                  "attempts, %zu cases run again, %zu passed\n",
                  counts.exclusive_lost, counts.exclusive_retried,
                  counts.exclusive_recovered);
    /* A storm that never ended a reservation proved nothing. */
    if (options->interrupt && counts.exclusive_lost == 0U) {
      std::fprintf(stderr, "--interrupt: no store-exclusive lost its "
                           "reservation, so none was interrupted\n");
      return 1;
    }
    return counts.failures == 0U ? 0 : 1;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "arm64 instruction corpus: %s\n", error.what());
    return 2;
  }
}
