// SPDX-License-Identifier: GPL-2.0-or-later
/* same_base::writes() is the builder's list of the general registers and SP
 * an instruction may write: a follower is paired with its leader only when
 * nothing between them writes their base, so a register the list leaves out
 * sends a follower through a base that changed. This test checks the list
 * against the engine. For every allocated encoding of the base and SIMD&FP
 * instruction sets in the architecture's decode table (the argument,
 * a64_instruction_decode_partitions.tsv), and for the system registers and
 * operations EL0 code reaches, it makes words with random values in the
 * bits the encoding leaves open, and with each register field at 31,
 * runs each word the gadget engine accepts as a block of its own from three
 * states in which every general register and SP holds a distinct value, and
 * requires every register the word changed to be in the list. A list may
 * name more than a word writes. */
#include "arm64_gadget_test_support.hpp"
#include "cpu/arm64/gadgets/program.hpp"
#include "cpu/arm64/gadgets/same_base.hpp"
#include "memory/tlb.hpp"
#include "require.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <memory_resource>
#include <sstream>
#include <string>
#include <vector>

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;
namespace same_base = gadgets::same_base;

namespace {

using uml::testing::require;

constexpr std::uint64_t page_size = 4096;
/* Guest [0, 1 MiB), every page held for reading and writing. Each register
 * holds less than 0xb000, so an address one names, plus an immediate or
 * another register scaled by 16, stays inside. */
constexpr std::uint64_t mapped = std::uint64_t{1} << 20;
constexpr std::size_t pages = mapped / page_size;
/* Inside, so a literal load may find its address mapped; guest memory is
 * not where the engine fetches from. */
constexpr std::uint64_t code_pc = 0x8'0000;
constexpr std::uint32_t svc = 0xd400'0001U;
constexpr std::uint32_t all = ~std::uint32_t{0};

struct Partition final {
	std::string name;
	std::uint32_t mask;
	std::uint32_t value;
};

/* The allocated encodings of the decode table's instruction layer. */
[[nodiscard]] std::vector<Partition> partitions(const char *path)
{
	std::ifstream file{path};
	require(file.good());
	std::vector<Partition> out;
	std::string line;
	std::getline(file, line); // the header
	while (std::getline(file, line)) {
		std::vector<std::string> fields;
		std::stringstream stream{line};
		std::string field;
		while (std::getline(stream, field, '\t'))
			fields.push_back(field);
		require(fields.size() >= 9);
		if (fields[1] != "instruction-table" || fields[2] != "allocated")
			continue;
		out.push_back({fields[4],
			static_cast<std::uint32_t>(
				std::stoul(fields[7], nullptr, 16)),
			static_cast<std::uint32_t>(
				std::stoul(fields[8], nullptr, 16))});
	}
	require(out.size() > 1000);
	return out;
}

/* MRS, MSR and SYS name a system register or operation in fields the
 * decode table leaves open, where random values find none the engine
 * implements. These are the ones EL0 code reaches, each with Rt open. */
[[nodiscard]] std::vector<Partition> system_operations()
{
	constexpr std::uint32_t rt_open = 0xffff'ffe0U;
	struct Named final {
		const char *name;
		std::uint32_t word; // as MRS; the MSR form clears bit 21
	};
	constexpr Named registers[] = {
		{"NZCV", 0xd53b'4200U}, {"DAIF", 0xd53b'4220U},
		{"SVCR", 0xd53b'4240U}, {"DIT", 0xd53b'42a0U},
		{"SSBS", 0xd53b'42c0U}, {"TCO", 0xd53b'42e0U},
		{"FPCR", 0xd53b'4400U}, {"FPSR", 0xd53b'4420U},
		{"FPMR", 0xd53b'4440U}, {"TPIDR_EL0", 0xd53b'd040U},
		{"TPIDRRO_EL0", 0xd53b'd060U}, {"TPIDR2_EL0", 0xd53b'd0a0U},
		{"CNTFRQ_EL0", 0xd53b'e000U}, {"CNTPCT_EL0", 0xd53b'e020U},
		{"CNTVCT_EL0", 0xd53b'e040U}, {"CNTPCTSS_EL0", 0xd53b'e0a0U},
		{"CNTVCTSS_EL0", 0xd53b'e0c0U}, {"CTR_EL0", 0xd53b'0020U},
		{"DCZID_EL0", 0xd53b'00e0U}, {"RNDR", 0xd53b'2400U},
		{"RNDRRS", 0xd53b'2420U}, {"GCSPR_EL0", 0xd53b'2520U},
		{"MIDR_EL1", 0xd538'0000U}, {"MPIDR_EL1", 0xd538'00a0U},
		{"REVIDR_EL1", 0xd538'00c0U}, {"ID_AA64PFR0_EL1", 0xd538'0400U},
		{"ID_AA64ZFR0_EL1", 0xd538'0480U},
		{"ID_AA64DFR0_EL1", 0xd538'0500U},
		{"ID_AA64ISAR0_EL1", 0xd538'0600U},
		{"ID_AA64MMFR0_EL1", 0xd538'0700U},
	};
	constexpr Named operations[] = {
		{"DC ZVA", 0xd50b'7420U}, {"DC CVAC", 0xd50b'7a20U},
		{"DC CVAU", 0xd50b'7b20U}, {"DC CVAP", 0xd50b'7c20U},
		{"DC CVADP", 0xd50b'7d20U}, {"DC CIVAC", 0xd50b'7e20U},
		{"IC IVAU", 0xd50b'7520U},
	};
	std::vector<Partition> out;
	for (const auto &named : registers) {
		out.push_back({std::string{"MRS "} + named.name, rt_open,
			named.word});
		out.push_back({std::string{"MSR "} + named.name, rt_open,
			named.word & ~(1U << 21)});
	}
	for (const auto &named : operations)
		out.push_back({named.name, rt_open, named.word});
	return out;
}

[[nodiscard]] std::uint64_t mix(std::uint64_t &state) noexcept
{
	state += 0x9e37'79b9'7f4a'7c15ULL;
	auto z = state;
	z = (z ^ (z >> 30)) * 0xbf58'476d'1ce4'e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d0'49bb'1331'11ebULL;
	return z ^ (z >> 31);
}

/* Words of an encoding: its fixed bits, the open ones random, then each
 * combination of the register fields at 4:0, 9:5, 14:10 and 20:16 set to 31
 * where the encoding leaves them open. */
[[nodiscard]] std::vector<std::uint32_t> words_of(const Partition &partition,
	std::uint64_t &seed)
{
	const auto open = ~partition.mask;
	std::vector<std::uint32_t> out{partition.value};
	for (unsigned fill = 0; fill < 16; ++fill)
		out.push_back(partition.value |
			(static_cast<std::uint32_t>(mix(seed)) & open));
	constexpr std::array<unsigned, 4> fields{0, 5, 10, 16};
	for (unsigned chosen = 1; chosen < 16; ++chosen) {
		auto word = partition.value |
			(static_cast<std::uint32_t>(mix(seed)) & open);
		for (unsigned index = 0; index < fields.size(); ++index)
			if ((chosen >> index) & 1U)
				word |= (31U << fields[index]) & open;
		out.push_back(word);
	}
	return out;
}

/* Host pages behind guest [0, mapped), and their first contents. */
struct Memory final {
	Memory()
		: bytes{static_cast<std::byte *>(
			  std::aligned_alloc(16384, mapped))},
		  pristine(mapped)
	{
		require(bytes != nullptr);
		for (std::size_t index = 0; index < pristine.size(); ++index)
			pristine[index] = static_cast<std::byte>(
				(index * 167U + (index >> 9) * 11U + 29U) &
				0xffU);
		restore();
	}
	~Memory() { std::free(bytes); }
	Memory(const Memory &) = delete;
	Memory &operator=(const Memory &) = delete;

	void restore() const
	{
		std::memcpy(bytes, pristine.data(), pristine.size());
	}

	std::byte *bytes;
	std::vector<std::byte> pristine;
};

/* Every page held for reading and writing; restored after each run, as a
 * miss on a tagged address re-keys the entry it resolves through. */
struct Translations final {
	explicit Translations(const Memory &memory)
		: tlb{std::make_unique<::uml::memory::Tlb>()}
	{
		for (std::size_t index = 0; index < pages; ++index) {
			const auto page = index * page_size;
			const auto delta = reinterpret_cast<std::uintptr_t>(
				memory.bytes + page) - page;
			held[index] = {page >> 12, delta, page >> 12, delta};
		}
		restore(false);
	}

	void restore(bool tagged)
	{
		std::memcpy(tlb->entries.data(), held.data(), sizeof held);
		tlb->fault_address = 0;
		tlb->fault_alignment = 0;
		tlb->tagged_addresses = tagged;
	}

	std::unique_ptr<::uml::memory::Tlb> tlb;
	std::array<::uml::memory::TlbEntry, pages> held{};
};
static_assert(pages <= ::uml::memory::tlb_entry_count);

/* Three states, each with a distinct value in every register and SP: two of
 * addresses in the mapped range, 16-byte aligned so that SP, atomics and
 * exclusive pairs take them, with the registers in opposite orders, and the
 * first again with a tag in the top byte and TBI on, so that writing a W
 * register changes the register. */
constexpr unsigned states = 3;

[[nodiscard]] std::uint64_t value(unsigned state, unsigned reg) noexcept
{
	switch (state) {
	case 0:
		return 0x1000U + reg * 0x210U;
	case 1:
		return 0x6000U + (31U - reg) * 0x250U;
	default:
		return (0xa5ULL << 56) | value(0, reg);
	}
}

[[nodiscard]] arm64::CpuState state_of(unsigned state)
{
	arm64::CpuState cpu{};
	for (unsigned reg = 0; reg < 31; ++reg)
		cpu.registers[reg] = value(state, reg);
	cpu.sp = value(state, 31);
	cpu.nzcv = state == 1 ? 0xf000'0000U : state == 2 ? 0x6000'0000U : 0U;
	cpu.tbi_enabled = state == 2;
	for (unsigned n = 0; n < cpu.vectors.size(); ++n)
		for (unsigned byte = 0; byte < 16; ++byte)
			cpu.vectors[n].bytes[byte] = static_cast<std::byte>(
				(n * 16U + byte + state * 7U) ^ 0x3cU);
	cpu.pc = code_pc;
	return cpu;
}

[[nodiscard]] std::uint32_t changed(const arm64::CpuState &before,
	const arm64::CpuState &after) noexcept
{
	std::uint32_t out = 0;
	for (unsigned reg = 0; reg < 31; ++reg)
		if (before.registers[reg] != after.registers[reg])
			out |= same_base::bit(reg);
	if (before.sp != after.sp)
		out |= same_base::bit(31);
	return out;
}

const gadgets::BlockLink *unlinked(std::uint64_t pc)
{
	static std::map<std::uint64_t, uml::emulator::tests::UnlinkedLink>
		records;
	return &records.try_emplace(pc, pc).first->second.link;
}

struct Site final {
	Site()
	{
		for (auto &entry : site.entries)
			entry = {~std::uint64_t{}, &empty.link};
	}
	gadgets::IndirectLink site{};
	uml::emulator::tests::UnlinkedLink empty{~std::uint64_t{}};
};

enum Class : unsigned {
	data_immediate,
	branch_system,
	load_store,
	data_register,
	vector,
	classes,
};
constexpr std::array<const char *, classes> class_names{
	"data processing (immediate)", "branches and system",
	"loads and stores", "data processing (register)", "SIMD&FP"};

[[nodiscard]] Class class_of(std::uint32_t word) noexcept
{
	const unsigned op0 = (word >> 25) & 15U;
	if ((op0 & 0b1110U) == 0b1000U)
		return data_immediate;
	if ((op0 & 0b1110U) == 0b1010U)
		return branch_system;
	if ((op0 & 0b0101U) == 0b0100U)
		return load_store;
	if ((op0 & 0b0111U) == 0b0101U)
		return data_register;
	return vector;
}

struct Tally final {
	std::size_t words{};
	std::size_t every{};	  // writes() names every register
	std::size_t refused{};	  // the engine does not accept the word
	std::size_t ran{};	  // ran from every state
	std::size_t observed{};	  // changed a register in some state
	/* Runs by how they ended: at the SVC, by a memory fault, at a
	 * branch out of the block, any other way. */
	std::array<std::size_t, 4> exits{};
};

} // namespace

int main(int argc, char **argv)
{
	require(argc == 2);
	auto table = partitions(argv[1]);
	for (auto &partition : system_operations())
		table.push_back(std::move(partition));
	Memory memory;
	Translations translations{memory};
	static Site site;
	std::array<Tally, classes> tally{};
	std::size_t encodings = 0;
	std::size_t failures = 0;
	std::uint64_t seed = 0x5ab5'0000'0000'0001ULL;
	for (const auto &partition : table) {
		bool counted = false;
		for (const auto word : words_of(partition, seed)) {
			const auto kills = same_base::writes(word);
			if ((word & 0x1800'0000U) == 0)
				continue; // SME, SVE, reserved and unallocated
			auto &line = tally[class_of(word)];
			++line.words;
			if (kills == all) {
				++line.every;
				continue;
			}
			const std::array<std::uint32_t, 2> code{word, svc};
			const auto built = gadgets::Builder{}.build(code_pc, code,
				unlinked, std::pmr::get_default_resource(),
				[](std::uint64_t) { return &site.site; });
			if (!built.complete) {
				++line.refused;
				continue;
			}
			bool refused = false;
			bool observed = false;
			for (unsigned state = 0; state < states && !refused;
			     ++state) {
				const auto before = state_of(state);
				auto after = before;
				translations.restore(state == 2);
				std::size_t executed = 0;
				const auto event = gadgets::
					execute_preserving_native_fp_environment(
						built.program, after,
						*translations.tlb, executed);
				if (class_of(word) == load_store ||
				    class_of(word) == branch_system)
					memory.restore();
				using ::uml::cpu::EngineEvent;
				if (event == EngineEvent::illegal_instruction) {
					refused = true;
					break;
				}
				line.exits[event == EngineEvent::system_call ? 0 :
					event == EngineEvent::memory_fault ? 1 :
					event == EngineEvent::continued ? 2 : 3]++;
				const auto written = changed(before, after);
				observed = observed || written != 0;
				if ((written & ~kills) == 0)
					continue;
				if (++failures <= 40)
					std::fprintf(stderr,
						"%s: %08x changed %08x in state "
						"%u (event %d), writes() %08x\n",
						partition.name.c_str(), word,
						written, state,
						static_cast<int>(event), kills);
			}
			if (refused) {
				++line.refused;
				continue;
			}
			++line.ran;
			line.observed += observed ? 1U : 0U;
			if (!counted) {
				++encodings;
				counted = true;
			}
		}
	}
	for (unsigned index = 0; index < classes; ++index) {
		const auto &line = tally[index];
		std::printf("%s: %zu words, %zu that writes() says may write "
			    "any register, %zu the engine refuses, %zu run from "
			    "each state, %zu of them changing a register; runs "
			    "ended %zu at the SVC, %zu by a memory fault, %zu by a "
			    "branch, %zu otherwise\n",
			class_names[index], line.words, line.every,
			line.refused, line.ran, line.observed, line.exits[0],
			line.exits[1], line.exits[2], line.exits[3]);
		/* Each class has words the engine runs and that write a
		 * register, so the check has something to find. */
		require(line.ran > 0 && line.observed > 0);
	}
	if (failures != 0) {
		std::fprintf(stderr,
			"arm64-gadget-same-base-writes: %zu runs changed a "
			"register writes() leaves out\n",
			failures);
		return 1;
	}
	std::printf("arm64-gadget-same-base-writes: %zu of %zu encodings "
		    "ran: ok\n",
		encodings, table.size());
	return 0;
}
