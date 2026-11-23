// SPDX-License-Identifier: GPL-2.0-or-later
/* The ahead-of-time code for every library of the configured set against the
 * gadget engine, which stays the reference: the same registers, the same
 * guest memory and the same faults must end in the same guest state and
 * memory, with the AOT code entered by the running engine. The harness is
 * general: it reads the libraries and functions the build generated and runs
 * the same cases over each. The scenarios are the unhappy paths of
 * docs/architecture.md, "Ahead-of-time code". */

#include "arm64_gadget_runtime_test_support.hpp"
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "static_mmu_test_support.hpp"

#include "cpu/arm64/aot/aot_diagnostics.hpp"
#include "cpu/arm64/aot/aot_runtime.hpp"
#include "cpu/arm64/code_synchronization.hpp"
#include "cpu/arm64/engine_adapters.hpp"
#include "cpu/arm64/fp_environment.hpp"
#include "cpu/arm64/gadgets/executor.hpp"
#include "cpu/host_capabilities.hpp"
#include "memory/count_audit.hpp"
#include "platform/apple/host_features.hpp"

#include <algorithm>
#include <array>
#include <arm_acle.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <fstream>
#include <functional>
#include <iterator>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <memory>
#include <set>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <pthread.h>
#include <thread>
#include <utility>
#include <vector>

namespace arm64 = uml::cpu::arm64;
namespace aot = arm64::aot::diagnostics;
namespace aot_runtime = arm64::aot;
namespace gadgets = arm64::gadgets;
using uml::emulator::tests::GenerationMapping;
using uml::emulator::tests::JitBacking;
using uml::emulator::tests::JitMapping;
using uml::emulator::tests::require;
using namespace uml::emulator::tests::gadget_runtime;

namespace {

/* Every row that main() calls is [[gnu::noinline]]: inlined, their locals (an
 * executor and an address space each) would share one frame of main, which
 * then takes most of the thread's stack. */
constexpr std::uint64_t page_bytes = 4096;
constexpr std::uint64_t stub_offset = 0x4000'0000ULL;
/* Beyond the text of the largest library under test. */
constexpr std::uint64_t arena_offset = 0x1000'0000ULL;
constexpr std::size_t arena_pages = 12;
constexpr std::size_t stack_pages = 2;
constexpr std::size_t max_blocks = 1500;
/* A tagged pointer never hits the AOT code's inline TLB probe (the engine
 * keys its entries by the untagged address), so each such access is one
 * engine step and one block: the AOT run needs far more blocks than the
 * reference for the same work. */
constexpr std::size_t aot_max_blocks = 500000;

struct SplitMix final {
	std::uint64_t state;
	std::uint64_t next() noexcept
	{
		std::uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
		z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
		z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
		return z ^ (z >> 31);
	}
	std::uint64_t below(std::uint64_t n) noexcept { return next() % n; }
};

struct Segment final {
	std::uint64_t vaddr, offset, filesz, memsz;
};

/* The library as the guest maps it: every page of every PT_LOAD from the
 * file at its own offset, zero past the end of the file. */
struct LibraryImage final {
	std::vector<std::byte> bytes;
	std::size_t pages{};
};

[[nodiscard]] LibraryImage load_library(const std::string &path)
{
	std::ifstream in{path, std::ios::binary};
	require(in.good());
	std::vector<std::byte> file;
	{
		std::vector<char> raw{std::istreambuf_iterator<char>{in}, {}};
		file.resize(raw.size());
		std::memcpy(file.data(), raw.data(), raw.size());
	}
	require(file.size() > 64 && std::memcmp(file.data(), "\x7f" "ELF", 4) == 0);
	const auto u16 = [&](std::size_t at) {
		std::uint16_t v;
		std::memcpy(&v, &file[at], 2);
		return v;
	};
	const auto u32 = [&](std::size_t at) {
		std::uint32_t v;
		std::memcpy(&v, &file[at], 4);
		return v;
	};
	const auto u64 = [&](std::size_t at) {
		std::uint64_t v;
		std::memcpy(&v, &file[at], 8);
		return v;
	};
	const auto phoff = u64(32);
	const auto phentsize = u16(54);
	const auto phnum = u16(56);
	std::vector<Segment> segments;
	std::uint64_t end = 0;
	for (unsigned i = 0; i < phnum; ++i) {
		const auto at = phoff + i * phentsize;
		if (u32(at) != 1)
			continue;
		const Segment s{u64(at + 16), u64(at + 8), u64(at + 32),
			u64(at + 40)};
		segments.push_back(s);
		end = std::max(end, s.vaddr + s.memsz);
	}
	LibraryImage image;
	image.pages = (end + page_bytes - 1) / page_bytes;
	image.bytes.assign(image.pages * page_bytes, std::byte{0});
	for (const auto &s : segments) {
		const auto first = s.vaddr / page_bytes;
		const auto last = (s.vaddr + s.filesz + page_bytes - 1) / page_bytes;
		for (auto page = first; page < last; ++page) {
			const auto from = (s.offset & ~(page_bytes - 1)) +
				(page - first) * page_bytes;
			if (from >= file.size())
				break;
			const auto n = std::min<std::uint64_t>(
				page_bytes, file.size() - from);
			std::memcpy(&image.bytes[page * page_bytes], &file[from], n);
		}
	}
	return image;
}

/* The library whose cases are being generated: a case that starts at an
 * instruction which writes SP from a register gets that register set to a
 * stack address, aligned or not (see sp_source). */
const LibraryImage *g_sites_image = nullptr;

/* Addresses in another library mapped in the same address space: some
 * registers (and the return address) of a case hold one. */
std::vector<std::uint64_t> g_cross_targets;

/* The register an instruction writes SP from, or -1: the 64-bit
 * add/sub (immediate and extended register) and logical (immediate) forms
 * whose destination is SP and whose first source is a register. */
[[nodiscard]] int sp_source(const LibraryImage &image,
	std::uint64_t offset) noexcept
{
	if (offset + 4 > image.bytes.size())
		return -1;
	std::uint32_t w;
	std::memcpy(&w, &image.bytes[offset], 4);
	if ((w & 31U) != 31U || (w >> 31) == 0U || ((w >> 5) & 31U) == 31U)
		return -1;
	const bool add_sub_immediate = (w & 0x1f000000U) == 0x11000000U;
	const bool add_sub_extended = (w & 0x1fe00000U) == 0x0b200000U;
	const bool logical_immediate = (w & 0x1f800000U) == 0x12000000U;
	return add_sub_immediate || add_sub_extended || logical_immediate ?
		static_cast<int>((w >> 5) & 31U) : -1;
}

struct Function final {
	std::uint64_t start, end;
};

[[nodiscard]] std::vector<Function> load_functions(
	const std::string &path, std::size_t library)
{
	std::ifstream in{path};
	require(in.good());
	std::vector<Function> out;
	std::size_t index;
	std::string a, b;
	while (in >> index >> a >> b)
		if (index == library)
			out.push_back({std::strtoull(a.c_str(), nullptr, 0),
				std::strtoull(b.c_str(), nullptr, 0)});
	require(!out.empty());
	return out;
}

/* A sample of each kind of instruction the rewriter inlined, per library:
 * `offset` of an instruction of `kind` (decode class and mnemonic). */
struct Site final {
	std::uint64_t offset;
	std::string kind;
};

[[nodiscard]] std::vector<Site> load_sites(const std::string &path,
	std::size_t library)
{
	std::ifstream in{path};
	require(in.good());
	std::vector<Site> out;
	std::size_t index;
	std::string offset, kind;
	while (in >> index >> offset >> kind)
		if (index == library)
			out.push_back({std::strtoull(offset.c_str(), nullptr, 16), kind});
	return out;
}

[[nodiscard]] std::set<std::uint64_t> load_steps(const std::string &path,
	std::size_t library)
{
	std::ifstream in{path};
	require(in.good());
	std::set<std::uint64_t> out;
	/* The cause may hold spaces: the line's first two fields only. */
	std::string line;
	while (std::getline(in, line)) {
		std::istringstream fields{line};
		std::size_t index;
		std::string offset;
		if (fields >> index >> offset && index == library)
			out.insert(std::strtoull(offset.c_str(), nullptr, 16));
	}
	return out;
}

/* One line per library the build rewrote: index, guest path, host file. */
struct LibraryFile final {
	std::size_t index;
	std::string guest, host;
};

[[nodiscard]] std::vector<LibraryFile> load_libraries(const std::string &path)
{
	std::ifstream in{path};
	require(in.good());
	std::vector<LibraryFile> out;
	std::string line;
	while (std::getline(in, line)) {
		const auto first = line.find('\t');
		const auto second = line.find('\t', first + 1);
		require(first != std::string::npos && second != std::string::npos);
		out.push_back({std::strtoul(line.c_str(), nullptr, 10),
			line.substr(first + 1, second - first - 1),
			line.substr(second + 1)});
	}
	require(!out.empty());
	return out;
}

/* Pages of guest data: each unmapped, read-only or writable, with contents a
 * case seeds. */
class Arena final {
public:
	Arena(unsigned long generation, std::uint64_t base)
		: generation_{generation}, base_{base}
	{
		for (std::size_t i = 0; i < arena_pages + stack_pages; ++i) {
			backings_.push_back(std::make_unique<JitBacking>(1));
			maps_.emplace_back();
			kinds_.push_back(0);
		}
		for (std::size_t i = arena_pages; i < backings_.size(); ++i)
			set_kind(i, 2);
	}

	void set_kind(std::size_t page, std::uint8_t kind)
	{
		if (kinds_[page] == kind)
			return;
		maps_[page].reset();
		kinds_[page] = kind;
		if (kind != 0)
			maps_[page] = std::make_unique<GenerationMapping>(
				generation_, base_ + page * page_bytes,
				*backings_[page], 1,
				kind == 2 ? PROT_READ | PROT_WRITE : PROT_READ,
				false);
	}

	void fill(SplitMix &rng)
	{
		std::array<std::byte, page_bytes> bytes;
		for (auto &backing : backings_) {
			for (std::size_t i = 0; i < bytes.size(); i += 8) {
				/* Mostly small values so loops and counts stay
				 * bounded, with some wide ones. */
				std::uint64_t v = rng.next();
				if (rng.below(4) != 0)
					v &= 0xff;
				std::memcpy(&bytes[i], &v, 8);
			}
			backing->put(0, bytes);
		}
	}

	[[nodiscard]] std::vector<std::byte> snapshot() const
	{
		std::vector<std::byte> out(backings_.size() * page_bytes);
		for (std::size_t i = 0; i < backings_.size(); ++i)
			backings_[i]->get(0,
				std::span{out}.subspan(i * page_bytes, page_bytes));
		return out;
	}

	/* The kinds and contents of another arena, as a run left them. */
	void adopt(const Arena &other)
	{
		std::array<std::byte, page_bytes> bytes;
		for (std::size_t i = 0; i < backings_.size(); ++i) {
			set_kind(i, other.kinds_[i]);
			other.backings_[i]->get(0, bytes);
			backings_[i]->put(0, bytes);
		}
	}

	[[nodiscard]] std::uint64_t base() const noexcept { return base_; }
	[[nodiscard]] std::uint8_t kind(std::size_t page) const
	{
		return kinds_[page];
	}
	[[nodiscard]] std::size_t total() const noexcept
	{
		return backings_.size();
	}

private:
	unsigned long generation_;
	std::uint64_t base_;
	std::vector<std::unique_ptr<JitBacking>> backings_;
	std::vector<std::unique_ptr<GenerationMapping>> maps_;
	std::vector<std::uint8_t> kinds_;
};

/* One vCPU: the guest memory view and the executor of a thread. */
struct Vcpu final {
	StaticGuestMemory memory;
	gadgets::GadgetExecutor executor;
};

/* One address space holding the library at `bias` and a stub that makes a
 * system call, with its own executor. `mapped_pages` maps only the first
 * pages of the library (map_rest() maps the others, as a loader would
 * later); add_library() maps a second library into the same address space. */
struct Environment final {
	/* `shared` is a backing several generations map (the library's bytes are
	 * its owner's to put); `guest_code` is the map call's argument for the
	 * library (3, the default: the kernel calls the pages immutable, which
	 * rewritten code needs of a page; 1: guest code the kernel does not
	 * promise to leave alone); `protection` is what the library's pages
	 * grant. */
	Environment(unsigned long generation_, std::uint64_t bias_,
		const LibraryImage &image_,
		std::size_t mapped_pages = SIZE_MAX,
		const JitBacking *shared = nullptr, int guest_code = 3,
		int protection = PROT_READ | PROT_EXEC)
		: generation{generation_}, bias{bias_}, guest_code{guest_code},
		  backing{image_.pages},
		  stub_backing{1},
		  mapped{std::min(mapped_pages, image_.pages)},
		  map{generation, bias, shared != nullptr ? *shared : backing, mapped,
			protection, guest_code},
		  stub_map{generation, bias + stub_offset, stub_backing, 1,
			PROT_READ | PROT_EXEC, true}
	{
		backing.put(0, image_.bytes);
		/* A run of the arm64 engines registers its generation as a holder
		 * of every frame it fetches code from (run_translated_loop), and
		 * the rewritten code's own reads of a page are fetches too. */
		memory.set_code_holder_registration(true);
		/* The stub: a system call, and after it `ldxr x0, [x1]; br x2`
		 * for the case that opens an exclusive monitor in the engine and
		 * then branches into the library. */
		stub_backing.put32(0, system_call);
		stub_backing.put32(4, 0xc85f7c20U);
		stub_backing.put32(8, 0xd61f0040U);
	}

	/* Map the pages of the library that were left out. */
	void map_rest(const LibraryImage &image)
	{
		const auto pages = image.pages - mapped;
		if (pages == 0)
			return;
		tail_backing = std::make_unique<JitBacking>(pages);
		tail_backing->put(0, std::span{image.bytes}.subspan(
			mapped * page_bytes, pages * page_bytes));
		tail_map = std::make_unique<GenerationMapping>(generation,
			bias + mapped * page_bytes, *tail_backing, pages,
			PROT_READ | PROT_EXEC, guest_code, false);
		mapped = image.pages;
	}

	void add_library(std::uint64_t other_bias, const LibraryImage &image)
	{
		extra_backing = std::make_unique<JitBacking>(image.pages);
		extra_backing->put(0, image.bytes);
		extra_map = std::make_unique<GenerationMapping>(generation,
			other_bias, *extra_backing, image.pages,
			PROT_READ | PROT_EXEC, guest_code, false);
		extra_stub_backing = std::make_unique<JitBacking>(1);
		extra_stub_backing->put32(0, system_call);
		extra_stub_map = std::make_unique<GenerationMapping>(generation,
			other_bias + stub_offset, *extra_stub_backing, 1,
			PROT_READ | PROT_EXEC, true);
	}

	unsigned long generation;
	std::uint64_t bias;
	int guest_code;
	JitBacking backing;
	JitBacking stub_backing;
	std::size_t mapped;
	JitMapping map;
	GenerationMapping stub_map;
	std::unique_ptr<JitBacking> tail_backing;
	std::unique_ptr<GenerationMapping> tail_map;
	std::unique_ptr<JitBacking> extra_backing;
	std::unique_ptr<GenerationMapping> extra_map;
	std::unique_ptr<JitBacking> extra_stub_backing;
	std::unique_ptr<GenerationMapping> extra_stub_map;
	StaticGuestMemory memory;
	gadgets::GadgetExecutor executor;
};

/* What the engine reports of a fault: where, which access, whether it was an
 * alignment fault, and the floating-point syndrome (zero unless a trapped
 * floating-point exception ended the block). */
struct Fault final {
	std::uint64_t pc, address;
	std::uint32_t access;
	bool alignment;
	std::uint32_t syndrome;
	/* The state the fault leaves, which the resumed run then builds on: the
	 * integer registers, the stack pointer, the flags, all vector registers
	 * (as a hash) and the arena (as a hash). A partial copy or set, or a
	 * vector access that changed a register before its fault, must leave
	 * exactly the engine's: the resumed run would repair it. */
	std::array<std::uint64_t, 31> registers;
	std::uint64_t sp;
	std::uint32_t nzcv;
	std::uint64_t vectors;
	std::uint64_t memory;
	bool operator==(const Fault &) const = default;
};

[[nodiscard]] std::uint64_t hash_of(const void *data, std::size_t size)
{
	std::uint64_t hash = 0xcbf29ce484222325ULL;
	const auto *bytes = static_cast<const unsigned char *>(data);
	for (std::size_t i = 0; i < size; ++i)
		hash = (hash ^ bytes[i]) * 0x100000001b3ULL;
	return hash;
}

[[nodiscard]] std::uint64_t hash_bytes(const std::vector<std::byte> &bytes)
{
	std::uint64_t hash = 0xcbf29ce484222325ULL;
	for (std::size_t i = 0; i + 8 <= bytes.size(); i += 8) {
		std::uint64_t word;
		std::memcpy(&word, &bytes[i], 8);
		hash = (hash ^ word) * 0x100000001b3ULL;
		hash ^= hash >> 29;
	}
	return hash;
}

struct Result final {
	arm64::CpuState cpu{};
	std::vector<Fault> faults;
	std::vector<std::byte> memory;
	EngineEvent last{EngineEvent::continued};
	/* The floating-point syndrome and alignment flag the engine reported
	 * for the event that ended the run. */
	std::uint32_t end_syndrome{};
	bool end_alignment{};
	bool finished{};
	std::size_t blocks{};
	std::size_t max_block{};

	/* The run ended in a system call or an exit the guest sees, rather than
	 * by running out of blocks. */
	[[nodiscard]] bool done() const noexcept
	{
		return finished || (last != EngineEvent::continued &&
			last != EngineEvent::interrupt);
	}
};

struct Case final {
	arm64::CpuState cpu;
	std::vector<std::uint8_t> kinds;
	std::uint64_t seed;
	std::uint64_t pc_offset;
	bool faulting;
	/* The saved vector registers of a case whose floating-point state is
	 * not loaded yet; cpu.deferred_vectors points into it. */
	std::shared_ptr<std::array<std::byte, sizeof(arm64::CpuState::vectors)>>
		deferred;
};

/* The vector registers the guest has: the loaded ones, or the image a
 * deferred load will bring in (arm64::CpuState::defer_fp_load). */
[[nodiscard]] const void *vector_image(const arm64::CpuState &cpu) noexcept
{
	return cpu.fp_loaded || cpu.deferred_vectors == nullptr ?
		static_cast<const void *>(cpu.vectors.data()) :
		static_cast<const void *>(cpu.deferred_vectors);
}

/* What a guest can observe of a vCPU at an instruction boundary: the
 * registers, the flags, the floating-point control and status words, the
 * thread pointer, all vector registers and the arena (the last two as
 * hashes). `memory` is zero where the caller did not hash the arena. */
struct Snapshot final {
	std::uint64_t pc, sp, x[31];
	std::uint32_t nzcv, fpcr, fpsr;
	std::uint64_t tls, vectors, memory;
	bool operator==(const Snapshot &) const = default;
};

/* The guest's FPSR where the state is between blocks: the gadget engine
 * may keep it in the host register until the run ends (the exit restores
 * it into the state, restore_native_fp_environment). */
[[nodiscard]] std::uint32_t guest_fpsr(const arm64::CpuState &cpu) noexcept
{
	if (!cpu.gadget_fp_active)
		return cpu.effective_fpsr();
	std::uint64_t native;
	asm volatile("mrs %0, fpsr" : "=r"(native));
	return static_cast<std::uint32_t>(native) & arm64::fpsr_writable_mask;
}

[[nodiscard]] Snapshot snapshot_of(const arm64::CpuState &cpu,
	std::uint64_t memory = 0)
{
	Snapshot s{};
	s.pc = cpu.pc;
	s.sp = cpu.sp;
	s.nzcv = cpu.nzcv;
	s.fpcr = cpu.effective_fpcr();
	s.fpsr = guest_fpsr(cpu);
	s.tls = cpu.tls;
	for (std::size_t i = 0; i < 31; ++i)
		s.x[i] = cpu.registers[i];
	s.vectors = hash_of(vector_image(cpu), sizeof(cpu.vectors));
	s.memory = memory;
	return s;
}

/* The states of a run one instruction at a time, each marked with whether it
 * was recorded at a fault (a state in the middle of an instruction, which a
 * memory copy or set leaves between its pages) or at an instruction boundary.
 * An exit of rewritten code that is a fault must lie on a state recorded at a
 * fault, and any other exit (a block, the budget, the end) on a boundary: a
 * state only a fault reaches is not one an ordinary exit may leave a guest
 * in, where the fault is still to be taken. */
struct Trace final {
	std::vector<Snapshot> states;
	std::vector<bool> at_fault;

	void push(const Snapshot &state, bool fault)
	{
		states.push_back(state);
		at_fault.push_back(fault);
	}
	[[nodiscard]] std::size_t size() const noexcept { return states.size(); }
	[[nodiscard]] const Snapshot &operator[](std::size_t i) const noexcept
	{
		return states[i];
	}
	[[nodiscard]] const Snapshot &back() const noexcept
	{
		return states.back();
	}
	/* The index of the first state at or after `from` that is `now` and of
	 * the kind of the exit (`fault`), or the size of the trace. */
	[[nodiscard]] std::size_t find(std::size_t from, const Snapshot &now,
		bool fault) const noexcept
	{
		for (std::size_t i = from; i < states.size(); ++i)
			if (at_fault[i] == fault && states[i] == now)
				return i;
		return states.size();
	}
};

/* A row that plays a wrong exit sets `g_faults_to_report_as_continued`: the
 * next faults of a run through rewritten code (SIZE_MAX: every one) are
 * returned as ordinary exits, as the end of a budget would, the instruction
 * runs again at the same address and takes the fault then. */
std::size_t g_faults_to_report_as_continued = 0;
std::size_t g_faults_reported_as_continued = 0;

/* run_once, through the ahead-of-time step while it is switched on, as the
 * engine's entry would choose (GadgetExecutor::aot_selected). */
[[nodiscard]] Outcome run_selected(GadgetExecutor &executor,
	StaticGuestMemory &memory, unsigned long generation, CpuState &cpu)
{
	/* The engine's entry loop takes a pending IC IVAU note after each block
	 * and drops a stale one when a run starts. This runs a block outside it,
	 * so a note an earlier case armed is dropped here, or the executor would
	 * end every later run early at its first stepped instruction. */
	arm64::discard_unmapped_code_synchronization();
	require(executor.begin_execution(generation, cpu.pc));
	std::size_t executed = 0;
	const auto event = aot_runtime::enabled() ?
		executor.run_aot_block(cpu, memory, generation, executed) :
		executor.run_block(cpu, memory, generation, executed);
	executor.end_execution();
	if (g_faults_to_report_as_continued != 0 &&
	    event == EngineEvent::memory_fault && aot_runtime::enabled()) {
		if (g_faults_to_report_as_continued != SIZE_MAX)
			--g_faults_to_report_as_continued;
		++g_faults_reported_as_continued;
		return {EngineEvent::continued, executed};
	}
	return {event, executed};
}

using Observer = std::function<void(const arm64::CpuState &)>;

/* The last blocks of a run, for the failure report. */
struct BlockLog final {
	bool enabled{};
	std::vector<std::array<std::uint64_t, 5>> blocks;
} g_log;

/* Give the host the writable page of a guest page before a run starts. A
 * host page first read through its mapping and then written takes two faults,
 * and the second one costs the thread its exclusive monitor, as an interrupt
 * would: a store-exclusive of the guest's first atomic on a page nothing has
 * written yet then fails once. The architecture allows it and the guest's
 * loop absorbs it, but a run that fails there leaves a state the stepped
 * trace (where every instruction starts a new entry and the software monitor
 * completes the store) never visits. The run being compared is the guest's
 * code, not the host's page tables, so the pages are written first. */
void warm_page(StaticGuestMemory &memory, unsigned long generation,
	const Arena &arena, std::size_t page)
{
	if (arena.kind(page) != 2)
		return;
	memory.set_generation(generation);
	volatile auto *byte = reinterpret_cast<volatile unsigned char *>(
		memory.atomic_pointer(arena.base() + page * page_bytes, 1,
			::uml::memory::MemoryAccess::write));
	if (byte != nullptr)
		*byte = *byte;
}

void warm_pages(StaticGuestMemory &memory, unsigned long generation,
	const Arena &arena)
{
	for (std::size_t page = 0; page < arena.total(); ++page)
		warm_page(memory, generation, arena, page);
}

/* The blocks of a run from the state in `cpu`, until the guest makes a system
 * call, an exit it sees, a fault no page can repair or `blocks` of them have
 * run. A fault inside the arena gets its page (writable) and the run goes on
 * at the same address, unless `stop_at_fault`: the run then ends at the first
 * memory fault, with the state it leaves, to be resumed by another call. */
[[nodiscard]] Result run_from(gadgets::GadgetExecutor &executor,
	StaticGuestMemory &memory, unsigned long generation, Arena &arena,
	arm64::CpuState cpu, std::size_t blocks, const Observer &observer = {},
	bool stop_at_fault = false, const Observer &on_fault = {})
{
	Result r;
	std::size_t faults_taken = 0;
	warm_pages(memory, generation, arena);
	for (; r.blocks < blocks; ++r.blocks) {
		const auto before = cpu.pc;
		const auto out = run_selected(
			executor, memory, generation, cpu);
		if (g_log.enabled)
			g_log.blocks.push_back({before,
				static_cast<std::uint64_t>(out.event), out.executed,
				cpu.pc, aot::stats().enters});
		r.max_block = std::max(r.max_block, out.executed);
		r.last = out.event;
		if (out.event == EngineEvent::continued ||
		    out.event == EngineEvent::interrupt) {
			if (observer)
				observer(cpu);
			continue;
		}
		arm64::restore_native_fp_environment(cpu);
		r.end_syndrome = executor.floating_point_syndrome();
		r.end_alignment = executor.fault_alignment();
		if (out.event == EngineEvent::system_call) {
			r.finished = true;
			break;
		}
		if (out.event != EngineEvent::memory_fault)
			break;
		const auto address = executor.fault_address();
		if (on_fault)
			on_fault(cpu);
		Fault fault{cpu.pc, address,
			static_cast<std::uint32_t>(executor.fault_access()),
			executor.fault_alignment(),
			executor.floating_point_syndrome(), {}, cpu.sp, cpu.nzcv,
			hash_of(vector_image(cpu), sizeof(cpu.vectors)),
			hash_bytes(arena.snapshot())};
		for (std::size_t i = 0; i < fault.registers.size(); ++i)
			fault.registers[i] = cpu.registers[i];
		r.faults.push_back(fault);
		if (stop_at_fault)
			break;
		const auto untagged = address & 0x00ff'ffff'ffff'ffffULL;
		if (untagged < arena.base() ||
		    untagged >= arena.base() + arena.total() * page_bytes ||
		    ++faults_taken > 8)
			break;
		const auto page = (untagged - arena.base()) / page_bytes;
		if (arena.kind(page) == 2)
			break;
		arena.set_kind(page, 2);
		warm_page(memory, generation, arena, page);
	}
	arm64::restore_native_fp_environment(cpu);
	/* Whether a run loaded a deferred floating-point state depends on the
	 * path (rewritten code loads it at every entry, the engine only when an
	 * instruction needs it); the guest's state is the same, and the results
	 * are compared with it loaded. */
	cpu.access_fp_state();
	r.cpu = cpu;
	r.memory = arena.snapshot();
	return r;
}

[[nodiscard]] Result drive_on(gadgets::GadgetExecutor &executor,
	StaticGuestMemory &memory, unsigned long generation, std::uint64_t bias,
	Arena &arena, Case c, std::size_t blocks, const Observer &observer = {},
	bool stop_at_fault = false, const Observer &on_fault = {})
{
	c.cpu.pc = bias + c.pc_offset;
	for (std::size_t i = 0; i < arena.total(); ++i)
		if (i < c.kinds.size())
			arena.set_kind(i, c.kinds[i]);
	SplitMix fill{c.seed};
	arena.fill(fill);
	return run_from(executor, memory, generation, arena, c.cpu, blocks,
		observer, stop_at_fault, on_fault);
}

[[nodiscard]] Result drive(Environment &env, Arena &arena, Case c,
	std::size_t blocks, const Observer &observer = {},
	const Observer &on_fault = {})
{
	return drive_on(env.executor, env.memory, env.generation, env.bias,
		arena, std::move(c), blocks, observer, false, on_fault);
}

/* The case of a report, its fields in the order `--case` reads them
 * (seed:pc_offset:faulting:tbi). */
void describe(const char *what, const Case &c)
{
	std::fprintf(stderr, "%s: seed %llx pc_offset %llx faulting %d tbi %d\n",
		what, static_cast<unsigned long long>(c.seed),
		static_cast<unsigned long long>(c.pc_offset),
		c.faulting ? 1 : 0, c.cpu.tbi_enabled ? 1 : 0);
}

/* Whether two runs ended in the same state, without a report. */
[[nodiscard]] bool identical(const Result &a, const Result &b)
{
	if (a.finished != b.finished || a.last != b.last ||
	    a.end_syndrome != b.end_syndrome ||
	    a.end_alignment != b.end_alignment ||
	    a.faults != b.faults || a.cpu.sp != b.cpu.sp ||
	    a.cpu.pc != b.cpu.pc || a.cpu.nzcv != b.cpu.nzcv ||
	    a.cpu.fpcr != b.cpu.fpcr || a.cpu.fpsr != b.cpu.fpsr ||
	    a.memory != b.memory)
		return false;
	for (std::size_t i = 0; i < a.cpu.registers.size(); ++i)
		if (a.cpu.registers[i] != b.cpu.registers[i])
			return false;
	return std::memcmp(a.cpu.vectors.data(), b.cpu.vectors.data(),
		sizeof(a.cpu.vectors)) == 0;
}

[[nodiscard]] bool same(const Result &a, const Result &b, const Case &c)
{
	bool ok = true;
	const auto say = [&](const char *what) {
		if (ok)
			describe(what, c);
		ok = false;
	};
	if (a.finished != b.finished || a.last != b.last)
		say("event");
	if (a.end_syndrome != b.end_syndrome ||
	    a.end_alignment != b.end_alignment) {
		say("syndrome");
		std::fprintf(stderr, "  syndrome ref %x aot %x alignment ref %d "
			"aot %d\n", a.end_syndrome, b.end_syndrome,
			a.end_alignment ? 1 : 0, b.end_alignment ? 1 : 0);
	}
	if (a.faults != b.faults) {
		say("faults");
		std::fprintf(stderr, "  faults ref %zu aot %zu\n", a.faults.size(),
			b.faults.size());
		for (std::size_t i = 0; i < std::min(a.faults.size(),
		     b.faults.size()); ++i) {
			const auto &x = a.faults[i];
			const auto &y = b.faults[i];
			if (x == y)
				continue;
			std::fprintf(stderr, "  fault %zu: pc %llx/%llx address %llx/%llx "
				"access %u/%u alignment %d/%d syndrome %x/%x nzcv %x/%x "
				"sp %llx/%llx vectors %s memory %s\n", i,
				static_cast<unsigned long long>(x.pc),
				static_cast<unsigned long long>(y.pc),
				static_cast<unsigned long long>(x.address),
				static_cast<unsigned long long>(y.address), x.access,
				y.access, x.alignment ? 1 : 0, y.alignment ? 1 : 0,
				x.syndrome, y.syndrome, x.nzcv, y.nzcv,
				static_cast<unsigned long long>(x.sp),
				static_cast<unsigned long long>(y.sp),
				x.vectors == y.vectors ? "same" : "DIFFERENT",
				x.memory == y.memory ? "same" : "DIFFERENT");
			for (std::size_t r = 0; r < x.registers.size(); ++r)
				if (x.registers[r] != y.registers[r])
					std::fprintf(stderr, "    x%zu ref %llx aot %llx\n", r,
						static_cast<unsigned long long>(x.registers[r]),
						static_cast<unsigned long long>(y.registers[r]));
			break;
		}
	}
	for (std::size_t i = 0; i < a.cpu.registers.size(); ++i)
		if (a.cpu.registers[i] != b.cpu.registers[i]) {
			say("register");
			std::fprintf(stderr, "  x%zu ref %llx aot %llx\n", i,
				static_cast<unsigned long long>(a.cpu.registers[i]),
				static_cast<unsigned long long>(b.cpu.registers[i]));
		}
	if (a.cpu.sp != b.cpu.sp) {
		say("sp");
		std::fprintf(stderr, "  sp ref %llx aot %llx\n",
			static_cast<unsigned long long>(a.cpu.sp),
			static_cast<unsigned long long>(b.cpu.sp));
	}
	if (a.cpu.tls != b.cpu.tls) {
		say("tls");
		std::fprintf(stderr, "  tls ref %llx aot %llx\n",
			static_cast<unsigned long long>(a.cpu.tls),
			static_cast<unsigned long long>(b.cpu.tls));
	}
	if (a.cpu.pc != b.cpu.pc) {
		say("pc");
		std::fprintf(stderr, "  pc ref %llx aot %llx\n",
			static_cast<unsigned long long>(a.cpu.pc),
			static_cast<unsigned long long>(b.cpu.pc));
	}
	if (a.cpu.nzcv != b.cpu.nzcv) {
		say("nzcv");
		std::fprintf(stderr, "  nzcv ref %x aot %x\n", a.cpu.nzcv,
			b.cpu.nzcv);
	}
	if (std::memcmp(a.cpu.vectors.data(), b.cpu.vectors.data(),
		    sizeof(a.cpu.vectors)) != 0)
		say("vectors");
	if (a.cpu.fpcr != b.cpu.fpcr || a.cpu.fpsr != b.cpu.fpsr) {
		say("fpcr/fpsr");
		std::fprintf(stderr, "  fpcr ref %x aot %x fpsr ref %x aot %x\n",
			a.cpu.fpcr, b.cpu.fpcr, a.cpu.fpsr, b.cpu.fpsr);
	}
	if (a.memory != b.memory)
		say("memory");
	return ok;
}

[[nodiscard]] Case make_case_impl(SplitMix &rng, std::uint64_t seed,
	std::uint64_t bias, std::uint64_t pc_offset, bool faulting, bool tbi,
	std::uint64_t arena_base)
{
	Case c{};
	c.seed = seed;
	c.pc_offset = pc_offset;
	c.faulting = faulting;
	c.cpu.tbi_enabled = tbi;
	c.kinds.assign(arena_pages + stack_pages, 2);
	/* The data pages and, in a faulting case, the stack pages are each
	 * unmapped, read-only or writable (a store to the stack that faults, a
	 * pre-index pair store that faults after the address was formed). */
	if (faulting)
		for (std::size_t i = 0; i < arena_pages + stack_pages; ++i) {
			const auto roll = rng.below(10);
			c.kinds[i] = roll < 2 ? 0 : roll < 4 ? 1 : 2;
		}
	const auto pointer = [&]() -> std::uint64_t {
		std::uint64_t page = rng.below(arena_pages);
		std::uint64_t offset = rng.below(page_bytes);
		if (rng.below(4) == 0)
			offset = page_bytes - 1 - rng.below(16);
		/* Half the pointers are 8-aligned: the atomics and ordered
		 * accesses take the engine's alignment path for the others. */
		else if (rng.below(2) == 0)
			offset &= ~std::uint64_t{7};
		std::uint64_t p = arena_base + page * page_bytes + offset;
		if (tbi && rng.below(2) != 0)
			p |= (rng.below(256) | 1) << 56;
		return p;
	};
	for (std::size_t i = 0; i < 29; ++i) {
		switch (rng.below(8)) {
		case 0:
		case 1:
		case 2:
			c.cpu.registers[i] = rng.below(65);
			break;
		case 3:
			c.cpu.registers[i] = rng.below(4097);
			break;
		case 4:
		case 5:
		case 6:
			c.cpu.registers[i] = pointer();
			break;
		default:
			c.cpu.registers[i] = rng.below(3) == 0 ? ~0ULL :
				bias + 0x1a000 + rng.below(0x2000);
		}
	}
	const auto stack_top = arena_base + (arena_pages + stack_pages) *
		page_bytes - 0x40;
	c.cpu.sp = stack_top;
	/* One case in eight starts with an SP that is not 16-byte aligned: the
	 * engine faults an SP-based access, so AOT code must not be entered. */
	if (rng.below(8) == 0)
		c.cpu.sp -= 8 * (1 + rng.below(3));
	c.cpu.registers[29] = stack_top;
	/* One case in five makes the frame pointer a stack address that is not
	 * 16-byte aligned: `mov sp, x29` and `add sp, x29, #n` then leave SP
	 * misaligned inside rewritten code, where the engine faults the next
	 * SP-based access. */
	if (rng.below(5) == 0)
		c.cpu.registers[29] = stack_top - 8 * (1 + rng.below(3)) -
			16 * rng.below(8);
	if (g_sites_image != nullptr) {
		/* A case that starts at an SP write takes the value from a stack
		 * address, aligned in one case in three. */
		const int source = sp_source(*g_sites_image, pc_offset);
		if (source >= 0 && source < 29) {
			const bool aligned = rng.below(3) == 0;
			c.cpu.registers[source] = stack_top - 16 * rng.below(8) -
				(aligned ? 0 : 8 * (1 + rng.below(3)));
		}
	}
	/* A thread pointer that points into guest data, so that the stack
	 * protector's canary load works and the function runs to its return. */
	c.cpu.tls = arena_base + rng.below(arena_pages) * page_bytes +
		0x40 * rng.below(32);
	c.cpu.registers[30] = bias + stub_offset;
	if (!g_cross_targets.empty()) {
		for (std::size_t i = 0; i < 29; ++i)
			if (rng.below(6) == 0)
				c.cpu.registers[i] = g_cross_targets[
					rng.below(g_cross_targets.size())];
		if (rng.below(4) == 0)
			c.cpu.registers[30] = g_cross_targets[
				rng.below(g_cross_targets.size())];
	}
	c.cpu.nzcv = static_cast<std::uint32_t>(rng.below(16)) << 28;
	for (auto &v : c.cpu.vectors)
		for (auto &b : v.bytes)
			b = static_cast<std::byte>(rng.next());
	/* Rounding mode, flush-to-zero and default NaN, which the engine and
	 * the rewritten code must honour alike. */
	c.cpu.fpcr = static_cast<std::uint32_t>(
		(rng.below(4) << 22) | (rng.below(2) << 24) | (rng.below(2) << 25));
	/* FIZ, AH and NEP, which rewritten code is not entered under. */
	if (rng.below(8) == 0)
		c.cpu.fpcr |= static_cast<std::uint32_t>(1 + rng.below(7));
	/* A floating-point trap enable (IOE, DZE, OFE, UFE, IXE, IDE): the
	 * engine runs the code one instruction at a time and reports the
	 * syndrome of an exception that traps. */
	if (rng.below(16) == 0)
		c.cpu.fpcr |= static_cast<std::uint32_t>(
			(1 + rng.below(31)) << 8) | (rng.below(2) << 15);
	c.cpu.fpsr = 0;
	return c;
}

/* The floating-point state a random case starts with, drawn from its seed so
 * that a failure report reproduces it: cumulative exception flags and QC
 * from before the function, which a native floating-point instruction must
 * leave set and add to, and in one case in eight a state not loaded yet, as
 * the runtime hands it over after a context load (the register file holds
 * stale words and the guest's are in the saved image). The directed rows set
 * their own and do not call it. */
void randomize_fp_state(Case &c)
{
	SplitMix rng{c.seed ^ 0x6670'7374'6174'65ULL};
	c.cpu.fpsr = rng.below(4) == 0 ?
		static_cast<std::uint32_t>(rng.next()) & arm64::fpsr_writable_mask :
		0U;
	if (rng.below(8) != 0)
		return;
	c.deferred = std::make_shared<
		std::array<std::byte, sizeof(c.cpu.vectors)>>();
	std::memcpy(c.deferred->data(), c.cpu.vectors.data(), c.deferred->size());
	const auto fpcr = c.cpu.fpcr, fpsr = c.cpu.fpsr;
	c.cpu.defer_fp_load(c.deferred->data(), fpcr, fpsr, ~0U);
	std::memset(c.cpu.vectors.data(), 0x5a, sizeof(c.cpu.vectors));
	c.cpu.fpcr = 0;
	c.cpu.fpsr = 0;
}

[[nodiscard]] Case make_case_from_seed(std::uint64_t seed,
	std::uint64_t bias, std::uint64_t pc_offset, bool faulting, bool tbi,
	std::uint64_t arena_base)
{
	SplitMix rng{seed};
	return make_case_impl(rng, seed, bias, pc_offset, faulting, tbi,
		arena_base);
}

[[nodiscard]] Case make_case(SplitMix &rng, std::uint64_t bias,
	std::uint64_t pc_offset, bool faulting, bool tbi,
	std::uint64_t arena_base)
{
	return make_case_from_seed(rng.next(), bias, pc_offset, faulting, tbi,
		arena_base);
}

[[nodiscard]] std::uint64_t arena_base(std::uint64_t bias)
{
	return bias + arena_offset;
}

/* Where an AOT run first leaves the reference's path: the reference single
 * steps and records every instruction's state, the state at each fault it
 * repairs and the state it ends in; each state the AOT run returns the engine
 * at, at a fault and at the end must be one of them, in order. */
[[nodiscard]] bool diagnose(const Case &c, const LibraryImage &image,
	std::uint64_t bias)
{
	constexpr std::uint64_t id = 5101;
	{
		/* The same case on fresh address spaces: a difference that
		 * goes away is state a former case left behind. */
		Environment fresh_ref{id + 10, bias, image};
		Environment fresh_aot{id + 11, bias, image};
		Arena fresh_ref_arena{fresh_ref.generation, arena_base(bias)};
		Arena fresh_aot_arena{fresh_aot.generation, arena_base(bias)};
		aot::set_enabled(false);
		const auto a = drive(fresh_ref, fresh_ref_arena, c, max_blocks);
		aot::set_enabled(true);
		const auto b = drive(fresh_aot, fresh_aot_arena, c, aot_max_blocks);
		aot::set_enabled(false);
		std::fprintf(stderr, "fresh spaces: ref done %d event %d blocks %zu "
			"faults %zu, aot done %d event %d blocks %zu faults %zu\n",
			a.done(), static_cast<int>(a.last), a.blocks,
			a.faults.size(), b.done(), static_cast<int>(b.last),
			b.blocks, b.faults.size());
		(void)same(a, b, c);
	}
	Environment ref{id, bias, image};
	Environment test{id + 1, bias, image};
	Arena ref_arena{ref.generation, arena_base(bias)};
	Arena test_arena{test.generation, arena_base(bias)};
	Trace trace;
	aot::set_enabled(false);
	ref.executor.set_single_step(true);
	trace.push([&] {
		auto first = c.cpu;
		first.pc = bias + c.pc_offset;
		return snapshot_of(first);
	}(), false);
	/* The states at a fault the run repairs are on the path, marked as such,
	 * and so is the state it ends in. */
	const auto record = [&](const arm64::CpuState &cpu, bool fault) {
		trace.push(snapshot_of(cpu), fault);
	};
	const auto ref_ended = drive(ref, ref_arena, c, 300000,
		[&](const arm64::CpuState &cpu) { record(cpu, false); },
		[&](const arm64::CpuState &cpu) { record(cpu, true); });
	if (ref_ended.last != EngineEvent::memory_fault)
		record(ref_ended.cpu, false);
	aot::set_enabled(true);
	std::size_t at = 0;
	std::size_t exits = 0;
	bool reported = false;
	g_log.enabled = true;
	g_log.blocks.clear();
	const auto check = [&](const arm64::CpuState &cpu, bool fault) {
		if (reported)
			return;
		++exits;
		const auto now = snapshot_of(cpu);
		const auto found = trace.find(at, now, fault);
		if (found != trace.size()) {
			at = found;
			return;
		}
		reported = true;
		std::fprintf(stderr, "first state off the reference path after "
			"exit %zu (last matching reference step %zu of %zu)\n",
			exits, at, trace.size());
		const auto show = [](const char *tag, const Snapshot &s) {
			std::fprintf(stderr, "  %s pc %llx sp %llx nzcv %x\n", tag,
				static_cast<unsigned long long>(s.pc),
				static_cast<unsigned long long>(s.sp), s.nzcv);
		};
		show("aot", now);
		for (std::size_t i = at; i < std::min(trace.size(), at + 6); ++i) {
			show("ref", trace[i]);
			for (std::size_t r = 0; r < 31; ++r)
				if (trace[i].x[r] != now.x[r])
					std::fprintf(stderr, "    x%zu ref %llx aot %llx\n",
						r, static_cast<unsigned long long>(trace[i].x[r]),
						static_cast<unsigned long long>(now.x[r]));
		}
	};
	const auto ended = drive(test, test_arena, c, 4000,
		[&](const arm64::CpuState &cpu) { check(cpu, false); },
		[&](const arm64::CpuState &cpu) { check(cpu, true); });
	/* A run that ends at a fault ends in the state of that fault. */
	check(ended.cpu, ended.last == EngineEvent::memory_fault);
	/* A run that is done took the faults of the reference: one that takes
	 * fewer has left the path, whatever its exits say. */
	if (!reported && ended.done() && ended.faults != ref_ended.faults) {
		reported = true;
		std::fprintf(stderr, "the AOT run took %zu faults, the reference "
			"%zu\n", ended.faults.size(), ref_ended.faults.size());
	}
	aot::set_enabled(false);
	g_log.enabled = false;
	for (std::size_t i = g_log.blocks.size() > 6 ? g_log.blocks.size() - 6 : 0;
	     i < g_log.blocks.size(); ++i)
		std::fprintf(stderr, "  block %zu: pc %llx event %llu executed %llu "
			"-> pc %llx enters %llu\n", i,
			static_cast<unsigned long long>(g_log.blocks[i][0]),
			static_cast<unsigned long long>(g_log.blocks[i][1]),
			static_cast<unsigned long long>(g_log.blocks[i][2]),
			static_cast<unsigned long long>(g_log.blocks[i][3]),
			static_cast<unsigned long long>(g_log.blocks[i][4]));
	std::fprintf(stderr, "aot run: event %d blocks %zu faults %zu done %d "
		"pc %llx\n", static_cast<int>(ended.last), ended.blocks,
		ended.faults.size(), ended.done(),
		static_cast<unsigned long long>(ended.cpu.pc));
	for (const auto &f : ended.faults)
		std::fprintf(stderr, "  fault pc %llx address %llx access %u "
			"alignment %d syndrome %x\n",
			static_cast<unsigned long long>(f.pc),
			static_cast<unsigned long long>(f.address), f.access,
			f.alignment ? 1 : 0, f.syndrome);
	if (!reported) {
		std::fprintf(stderr, "every AOT exit state is on the reference path "
			"(%zu exits, reference step %zu of %zu)\n", exits, at,
			trace.size());
		const auto show = [](const char *tag, const Snapshot &s) {
			std::fprintf(stderr, "  %s pc %llx x8 %llx x9 %llx x10 %llx "
				"x11 %llx\n", tag, static_cast<unsigned long long>(s.pc),
				static_cast<unsigned long long>(s.x[8]),
				static_cast<unsigned long long>(s.x[9]),
				static_cast<unsigned long long>(s.x[10]),
				static_cast<unsigned long long>(s.x[11]));
		};
		show("ref end", trace.back());
		show("ref at", trace[at]);
	}
	return !reported;
}

struct Totals final {
	std::size_t cases{}, compared{}, skipped{}, faulted{}, fault_events{},
		finished{}, nondeterministic{}, slow{}, deferred{}, flagged{};
};

struct Config final {
	std::string libraries, functions, sites, steps;
	/* Run only the library with this index (-1: all). */
	long library{-1};
	/* At most this many functions of a library are sampled. */
	std::size_t sample{400};
	/* seed:pc_offset:faulting:tbi of one case to run alone. */
	std::string only;
	/* Run only the scan of the words the engine rejects. */
	bool words_only{};
	/* Run only the row about IC IVAU in rewritten code. */
	bool code_synchronize_only{};
	std::size_t starts{12}, interior{8}, sp_sites{6}, site_cases{4};
	/* Cases the stepped-trace row follows, and the longest stepped run it
	 * records (a case that takes more is left out). */
	std::size_t trace_cases{48}, trace_cap{20000};
	std::uint64_t master{0x5eed};
};

[[nodiscard]] Config config(int argc, char **argv)
{
	Config c;
	c.libraries = AOT_TEST_LIBRARIES;
	c.functions = AOT_TEST_FUNCTIONS;
	c.sites = AOT_TEST_SITES;
	c.steps = AOT_TEST_STEPS;
	for (int i = 1; i + 1 < argc; i += 2) {
		const std::string key{argv[i]};
		if (key == "--starts")
			c.starts = std::strtoul(argv[i + 1], nullptr, 0);
		else if (key == "--interior")
			c.interior = std::strtoul(argv[i + 1], nullptr, 0);
		else if (key == "--sp-sites")
			c.sp_sites = std::strtoul(argv[i + 1], nullptr, 0);
		else if (key == "--site-cases")
			c.site_cases = std::strtoul(argv[i + 1], nullptr, 0);
		else if (key == "--trace-cases")
			c.trace_cases = std::strtoul(argv[i + 1], nullptr, 0);
		else if (key == "--trace-cap")
			c.trace_cap = std::strtoul(argv[i + 1], nullptr, 0);
		else if (key == "--master")
			c.master = std::strtoull(argv[i + 1], nullptr, 0);
		else if (key == "--case")
			c.only = argv[i + 1];
		else if (key == "--words-only")
			c.words_only = std::strtoul(argv[i + 1], nullptr, 0) != 0;
		else if (key == "--code-synchronize-only")
			c.code_synchronize_only =
				std::strtoul(argv[i + 1], nullptr, 0) != 0;
		else if (key == "--library")
			c.library = std::strtol(argv[i + 1], nullptr, 0);
		else if (key == "--sample")
			c.sample = std::strtoul(argv[i + 1], nullptr, 0);
	}
	return c;
}

/* Every function of the library, from its first instruction and from random
 * interior ones, and a sample of every kind of instruction the rewriter
 * inlined, against random states and fault patterns. */
[[gnu::noinline]]
Totals differential(const Config &config, const LibraryImage &image,
	const std::vector<Function> &functions, const std::vector<Site> &sites)
{
	constexpr std::uint64_t bias = 0x7123'0000'0000ULL;
	Environment ref{4101, bias, image};
	Environment aot_env{4102, bias, image};
	Arena ref_arena{ref.generation, arena_base(bias)};
	Arena aot_arena{aot_env.generation, arena_base(bias)};
	SplitMix rng{config.master};
	Totals t;
	aot::reset_stats();
	const auto one = [&](std::uint64_t pc, const char *kind) {
		const bool faulting = rng.below(3) == 0;
		const bool tbi = rng.below(4) != 0;
		auto c = make_case(rng, bias, pc, faulting, tbi,
			arena_base(bias));
		randomize_fp_state(c);
		t.deferred += c.deferred ? 1 : 0;
		t.flagged += c.deferred || c.cpu.fpsr != 0 ? 1 : 0;
		if (std::getenv("AOT_TRACE") != nullptr) {
			describe("reference", c);
			std::fflush(stderr);
		}
		aot::set_enabled(false);
		const auto want = drive(ref, ref_arena, c, max_blocks);
		++t.cases;
		if (!want.done()) {
			++t.skipped;
			return;
		}
		if (std::getenv("AOT_TRACE") != nullptr) {
			describe("running", c);
			std::fflush(stderr);
		}
		aot::set_enabled(true);
		const auto got = drive(aot_env, aot_arena, c, aot_max_blocks);
		aot::set_enabled(false);
		if (!identical(want, got)) {
			/* The counter registers and the like differ between two runs
			 * of the same code: if the engine disagrees with itself the
			 * case says nothing. */
			aot::set_enabled(false);
			const auto again = drive(ref, ref_arena, c, max_blocks);
			if (!identical(want, again)) {
				++t.skipped;
				++t.nondeterministic;
				return;
			}
		}
		if (!same(want, got, c)) {
			std::fprintf(stderr, "  start %llx (%s)\n",
				static_cast<unsigned long long>(pc), kind);
			const bool consistent = diagnose(c, image, bias);
			/* An AOT run that is still going after the limit but has
			 * left the engine's path nowhere is a run that steps a
			 * great deal, not a wrong one. */
			if (consistent && !got.done()) {
				++t.skipped;
				++t.slow;
				return;
			}
			require(false);
		}
		++t.compared;
		t.finished += want.finished ? 1 : 0;
		t.faulted += faulting ? 1 : 0;
		t.fault_events += want.faults.size();
	};
	for (const auto &function : functions) {
		for (std::size_t i = 0; i < config.starts; ++i)
			one(function.start, "function");
		for (std::size_t i = 0; i < config.interior; ++i)
			one(function.start + 4 * rng.below(
				(function.end - function.start) / 4), "interior");
		/* Every instruction that writes SP from a register, from itself,
		 * with that register aligned or not. */
		for (auto at = function.start; at < function.end; at += 4)
			if (sp_source(image, at) >= 0)
				for (std::size_t i = 0; i < config.sp_sites; ++i)
					one(at, "sp write");
	}
	for (const auto &site : sites)
		for (std::size_t i = 0; i < config.site_cases; ++i)
			one(site.offset, site.kind.c_str());
	return t;
}

/* A run of the same case on a fresh environment, for the scenarios below. */
struct Fixture final {
	static constexpr std::uint64_t bias = 0x7133'0000'0000ULL;
	Fixture(unsigned long generation, const LibraryImage &image,
		std::uint64_t bias_ = bias)
		: env{generation, bias_, image}, arena{generation, arena_base(bias_)}
	{
	}
	Environment env;
	Arena arena;
};

[[gnu::noinline]]
void default_off_and_single_step(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	require(!aot_runtime::enabled());
	SplitMix rng{7};
	const auto bias = Fixture::bias;
	Fixture ref{4201, image}, one{4202, image};
	for (std::size_t f = 0; f < functions.size(); ++f) {
		const auto c = make_case(rng, bias, functions[f].start, false,
			true, arena_base(bias));
		aot::reset_stats();
		const auto want = drive(ref.env, ref.arena, c, max_blocks);
		require(aot::stats().enters == 0);
		if (!want.finished || want.blocks > 40)
			continue;
		/* Enabled, but single-stepping: the engine runs one
		 * instruction a block and the AOT code is never entered. */
		aot::set_enabled(true);
		one.env.executor.set_single_step(true);
		const auto got = drive(one.env, one.arena, c, 400000);
		one.env.executor.set_single_step(false);
		aot::set_enabled(false);
		require(aot::stats().enters == 0);
		require(got.finished);
		require(same(want, got, c));
		return;
	}
	require(false);
}

[[gnu::noinline]]
void preemption_is_bounded(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	/* Any function that loops long on a large count: the blocks the engine
	 * returns from stay short, so an interrupt waits for at most one. */
	SplitMix rng{11};
	const auto bias = Fixture::bias;
	Fixture ref{4211, image}, fast{4212, image};
	std::size_t longest = 0;
	std::size_t entries = 0;
	for (std::size_t f = 0; f < functions.size(); ++f) {
		auto c = make_case(rng, bias, functions[f].start, false, true,
			arena_base(bias));
		for (std::size_t i = 0; i < 8; ++i)
			c.cpu.registers[i] = rng.below(2) ? 3000 + rng.below(1000) :
				c.cpu.registers[i];
		aot::set_enabled(false);
		const auto want = drive(ref.env, ref.arena, c, max_blocks);
		if (!want.done())
			continue;
		aot::reset_stats();
		aot::set_enabled(true);
		const auto got = drive(fast.env, fast.arena, c, aot_max_blocks);
		aot::set_enabled(false);
		require(same(want, got, c));
		longest = std::max(longest, got.max_block);
		entries += aot::stats().enters;
	}
	require(entries > 0);
	/* One entry's budget is 4096 instructions; a check at a loop header
	 * or function entry lets a few more through. */
	require(longest <= 4096 + 1024);
}

[[gnu::noinline]]
void two_load_biases(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	SplitMix rng{13};
	constexpr std::uint64_t bias_a = 0x7143'0000'0000ULL;
	constexpr std::uint64_t bias_b = 0x7155'1234'0000ULL & ~0xfffULL;
	Fixture ref_a{4221, image, bias_a}, ref_b{4222, image, bias_b};
	Fixture a{4223, image, bias_a}, b{4224, image, bias_b};
	aot::reset_stats();
	std::size_t compared = 0;
	for (std::size_t f = 0; f < functions.size(); f += 3) {
		for (int which = 0; which < 2; ++which) {
			const auto bias = which ? bias_b : bias_a;
			const auto c = make_case(rng, bias, functions[f].start,
				false, true, arena_base(bias));
			aot::set_enabled(false);
			const auto want = drive(which ? ref_b.env : ref_a.env,
				which ? ref_b.arena : ref_a.arena, c, max_blocks);
			if (!want.done())
				continue;
			aot::set_enabled(true);
			const auto got = drive(which ? b.env : a.env,
				which ? b.arena : a.arena, c, aot_max_blocks);
			aot::set_enabled(false);
			/* Pointers differ by bias between the two, so each is
			 * compared with its own reference. */
			require(same(want, got, c));
			++compared;
		}
	}
	require(compared > 20);
	const auto st = aot::stats();
	require(st.enters > 0);
	require(st.poisoned == 0);
}

/* The same bytes at a covered address are not what the AOT code stands for
 * when a page differs: no entry, the engine runs it as usual. */
[[gnu::noinline]]
void other_bytes_fall_back(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	LibraryImage other = image;
	/* Change one instruction of every code page: a NOP (a BRK where it is
	 * one already) in place of the first word of each function, and of
	 * the first word on every page a function reaches, which the engine
	 * will run. */
	const auto change = [&](std::uint64_t at) {
		std::uint32_t word;
		std::memcpy(&word, &other.bytes[at], 4);
		word = word == 0xd503201fU ? 0xd4200000U : 0xd503201fU;
		std::memcpy(&other.bytes[at], &word, 4);
	};
	for (const auto &f : functions) {
		change(f.start + 4);
		for (auto page = (f.start / page_bytes + 1) * page_bytes;
		     page < f.end; page += page_bytes)
			change(page);
	}
	SplitMix rng{17};
	const auto bias = Fixture::bias;
	Fixture ref{4231, other}, got{4232, other};
	aot::reset_stats();
	std::size_t compared = 0;
	for (std::size_t f = 0; f < functions.size(); f += 4) {
		const auto c = make_case(rng, bias, functions[f].start, false,
			true, arena_base(bias));
		aot::set_enabled(false);
		const auto want = drive(ref.env, ref.arena, c, max_blocks);
		if (!want.done())
			continue;
		aot::set_enabled(true);
		const auto have = drive(got.env, got.arena, c, aot_max_blocks);
		aot::set_enabled(false);
		require(same(want, have, c));
		++compared;
	}
	require(compared > 10);
	require(aot::stats().enters == 0);
}

/* A guest that rewrites covered text after the AOT code was entered: the
 * page it changed is no longer the code the framework stands for, so the
 * engine runs it, and the pages that did not change are still entered. When
 * the bytes are back the page is compared again and entered. */
[[gnu::noinline]]
void changed_text_falls_back(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	SplitMix rng{19};
	const auto bias = Fixture::bias;
	const auto fn = functions[5];
	auto c = make_case(rng, bias, fn.start, false, true, arena_base(bias));
	/* A function on another code page, whose first instruction is
	 * rewritten below. */
	const std::uint32_t patched = 0xd2800f00U; /* mov x0, #0x78 */
	std::uint64_t offset = 0;
	for (const auto &f : functions)
		if ((f.start >> 12) != (fn.start >> 12)) {
			offset = f.start;
			break;
		}
	require(offset != 0);
	std::uint32_t original;
	std::memcpy(&original, &image.bytes[offset], 4);
	LibraryImage changed = image;
	std::memcpy(&changed.bytes[offset], &patched, 4);
	const auto d = make_case(rng, bias, offset, false, true, arena_base(bias));
	/* What the engine does with the bytes the guest holds before and after,
	 * on fresh guests (invalidate_code publishes in the generation the
	 * thread ran last, so the guest under test runs last before each). */
	Fixture ref_changed{4241, changed}, ref_original{4243, image};
	aot::set_enabled(false);
	const auto want_c = drive(ref_changed.env, ref_changed.arena, c,
		max_blocks);
	const auto want_changed = drive(ref_changed.env, ref_changed.arena, d,
		max_blocks);
	const auto want_back = drive(ref_original.env, ref_original.arena, d,
		max_blocks);

	Fixture got{4242, image};
	aot::reset_stats();
	aot::set_enabled(true);
	const auto first = drive(got.env, got.arena, c, aot_max_blocks);
	aot::set_enabled(false);
	auto entered = aot::stats().enters;
	require(entered > 0);
	require(first.done());
	const auto publish = [&](std::uint32_t word) {
		got.env.backing.put32(offset, word);
		got.env.memory.invalidate_code(got.env.bias + offset);
	};
	publish(patched);
	/* fn's page did not change: still entered. */
	aot::set_enabled(true);
	const auto have = drive(got.env, got.arena, c, aot_max_blocks);
	aot::set_enabled(false);
	require(same(want_c, have, c));
	auto st = aot::stats();
	require(st.poisoned == 0);
	require(st.enters > entered);
	entered = st.enters;
	/* The changed page itself: a function there is the engine's. */
	aot::set_enabled(true);
	const auto have_changed = drive(got.env, got.arena, d, aot_max_blocks);
	aot::set_enabled(false);
	require(same(want_changed, have_changed, d));
	st = aot::stats();
	require(st.enters == entered);
	require(st.poisoned == 0);
	/* The bytes are back and published: the page is compared again. */
	publish(original);
	aot::set_enabled(true);
	const auto have_back = drive(got.env, got.arena, d, aot_max_blocks);
	aot::set_enabled(false);
	require(same(want_back, have_back, d));
	st = aot::stats();
	require(st.enters > entered);
	require(st.poisoned == 0);
}

/* A frame several address spaces map: the library's bytes in one backing,
 * pinned by a mapping that is not code so that the records of its pages (and
 * their counts and epochs) outlive the executable mappings that come and go.
 * Each guest maps it at the same bias; `immutable` is the kernel's fact for
 * the mapping. */
struct SharedFrame final {
	SharedFrame(const LibraryImage &image, unsigned long pin_generation)
		: backing{image.pages}, pin{pin_generation, 0x1000, backing, 1,
			PROT_READ, false}
	{
		backing.put(0, image.bytes);
	}
	JitBacking backing;
	GenerationMapping pin;
};

struct SharedGuest final {
	SharedGuest(unsigned long generation, const LibraryImage &image,
		const SharedFrame &frame, bool immutable,
		int protection = PROT_READ | PROT_EXEC)
		: env{generation, Fixture::bias, image, SIZE_MAX, &frame.backing,
			immutable ? 3 : 1, protection},
		  arena{generation, arena_base(Fixture::bias)}
	{
	}
	Environment env;
	Arena arena;
};

struct SharedRun final {
	Result result;
	aot::Stats stats;
};

[[nodiscard]] SharedRun run_shared(SharedGuest &guest, const Case &c)
{
	aot::reset_stats();
	aot::set_enabled(true);
	auto result = drive(guest.env, guest.arena, c, aot_max_blocks);
	aot::set_enabled(false);
	return {std::move(result), aot::stats()};
}

#ifndef AOT_TEST_SHARED_VERDICTS
#define AOT_TEST_SHARED_VERDICTS 1
#endif

/* Whether a second address space on a shared immutable frame is answered by
 * the table (the default) or hashes the page again, which is all a build
 * with UML_AOT_SHARED_VERDICTS off does: every row that expects an answer
 * expects the other outcome there, and the results stay the same. */
[[nodiscard]] bool answered_by_the_table(const aot::Stats &s)
{
	if (AOT_TEST_SHARED_VERDICTS != 0)
		return s.shared_hits >= 1 && s.verifications == 0;
	return s.shared_hits == 0 && s.verifications >= 1;
}

/* A verdict that a physical page held a library page's bytes is shared by
 * the address spaces that map the page, but only while the kernel says the
 * page is immutable in the mapping asked, the memory layer says nothing has
 * published or ended it, and the mapping can still execute. */
void enterable(Case &c);
[[nodiscard]] LibraryImage with_equal_digest(const LibraryImage &image,
	std::uint64_t start);

[[gnu::noinline]]
void verdicts_are_shared_between_address_spaces(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	SplitMix rng{23};
	const auto bias = Fixture::bias;
	const auto fn = functions[5];
	auto c = make_case(rng, bias, fn.start, false, true, arena_base(bias));
	enterable(c);
	unsigned long generation = 5100;
	/* The same library with the first word of fn changed and the last word
	 * of its page chosen so that the page has the stored digest: the digest
	 * binds it and only the stored hash tells it from the stored page. */
	const auto changed = with_equal_digest(image, fn.start);
	aot::set_enabled(false);
	/* An environment is large (its memory keeps a TLB): the rows hold them
	 * on the heap, one at a time where they can. */
	auto ref = std::make_unique<Fixture>(generation++, image);
	const auto want = drive(ref->env, ref->arena, c, max_blocks);
	ref.reset();
	auto ref_changed = std::make_unique<Fixture>(generation++, changed);
	const auto want_changed = drive(ref_changed->env, ref_changed->arena, c,
		max_blocks);
	ref_changed.reset();
	require(want.done());
	require(!same(want, want_changed, c));
	auto ref_noexec = std::make_unique<Environment>(generation++, bias, image,
		SIZE_MAX, nullptr, 1, PROT_READ);
	auto ref_noexec_arena = std::make_unique<Arena>(ref_noexec->generation,
		arena_base(bias));
	const auto want_noexec = drive(*ref_noexec, *ref_noexec_arena, c,
		max_blocks);
	ref_noexec_arena.reset();
	ref_noexec.reset();
	require(!same(want, want_noexec, c));
	const auto guest = [&](const SharedFrame &frame, bool immutable,
			       int protection = PROT_READ | PROT_EXEC) {
		return std::make_unique<SharedGuest>(generation++, image, frame,
			immutable, protection);
	};
	std::fprintf(stderr, "shared verdicts: ");

	/* Two address spaces, one immutable frame: the second is answered. */
	{
		aot::clear_shared_verdicts();
		SharedFrame frame{image, generation++};
		auto first = guest(frame, true);
		const auto a = run_shared(*first, c);
		require(same(want, a.result, c));
		require(a.stats.enters > 0 && a.stats.verifications > 0 &&
			a.stats.shared_hits == 0);
		auto second = guest(frame, true);
		const auto b = run_shared(*second, c);
		require(same(want, b.result, c));
		require(b.stats.enters > 0 && answered_by_the_table(b.stats) &&
			b.stats.poisoned == 0);
		/* Nothing was filed under another epoch: no hash is blamed on one. */
		require(a.stats.epoch_rehashes == 0 && b.stats.epoch_rehashes == 0);
		/* A verification reads the page's stamp once to admit the page,
		 * which is also the stamp it holds before the fetch, and once more
		 * after a hash that matched: the page that was answered cost one
		 * query, the page that was hashed two (a build that shares
		 * nothing reads no second stamp). */
		if (AOT_TEST_SHARED_VERDICTS != 0) {
			require(a.stats.stamp_queries == 2 * a.stats.verifications);
			require(b.stats.stamp_queries == b.stats.shared_hits);
		} else {
			require(a.stats.stamp_queries == a.stats.verifications &&
				b.stats.stamp_queries == b.stats.verifications);
		}
		/* The memory layer's count of this frame's executable mappings is
		 * what the two guests' words say, as a guest run audits it. */
		const auto audit = ::uml::memory::audit_executable_counts();
		require(audit.exact && audit.records >= 1 && audit.mismatches == 0);
		/* The first guest, asked again, finds its pages compared. */
		std::fprintf(stderr, "hits %llu; ",
			static_cast<unsigned long long>(b.stats.shared_hits));
	}

	/* A mapping without the fact is not entered at all: it is never
	 * answered, never files and never compares the page, also beside
	 * mappings that have the fact. */
	{
		aot::clear_shared_verdicts();
		SharedFrame frame{image, generation++};
		auto first = guest(frame, false);
		const auto a = run_shared(*first, c);
		auto second = guest(frame, false);
		const auto b = run_shared(*second, c);
		require(same(want, a.result, c) && same(want, b.result, c));
		require(a.stats.enters == 0 && b.stats.enters == 0);
		require(a.stats.verifications == 0 && b.stats.verifications == 0);
		require(a.stats.shared_hits == 0 && b.stats.shared_hits == 0);
		require(a.stats.mutable_pages >= 1 && b.stats.mutable_pages >= 1);

		/* The third guest has the fact; nothing was filed for it. */
		auto third = guest(frame, true);
		const auto d = run_shared(*third, c);
		require(same(want, d.result, c));
		require(d.stats.enters > 0 && d.stats.shared_hits == 0 &&
			d.stats.verifications >= 1);
		/* It filed its own: a fourth with the fact is answered; a fifth
		 * without it is not, and is not entered either. */
		auto fourth = guest(frame, true);
		const auto e = run_shared(*fourth, c);
		require(same(want, e.result, c));
		require(answered_by_the_table(e.stats));
		auto fifth = guest(frame, false);
		const auto f = run_shared(*fifth, c);
		require(same(want, f.result, c));
		require(f.stats.enters == 0 && f.stats.shared_hits == 0 &&
			f.stats.verifications == 0);
	}

	/* A mapping that cannot execute gets no answer from the table, however
	 * warm it is, and the page is never entered. */
	{
		aot::clear_shared_verdicts();
		SharedFrame frame{image, generation++};
		auto warm = guest(frame, true);
		const auto a = run_shared(*warm, c);
		require(same(want, a.result, c) && a.stats.enters > 0);
		auto locked = guest(frame, true, PROT_READ);
		const auto b = run_shared(*locked, c);
		require(same(want_noexec, b.result, c));
		require(b.stats.enters == 0 && b.stats.shared_hits == 0);
		/* And a mapping that loses execute after its pages were compared:
		 * the table is asked again and the stamp query refuses. */
		require(darwin_uml_static_vm_activate(warm->env.generation) == 0);
		require(darwin_uml_static_vm_protect(
			reinterpret_cast<void *>(warm->env.bias),
			image.pages * page_bytes, PROT_READ) == 0);
		const auto d = run_shared(*warm, c);
		require(same(want_noexec, d.result, c));
		require(d.stats.enters == 0 && d.stats.shared_hits == 0);
	}

	/* The page under test, as one guest maps it again after it lost it. */
	const auto page_offset = fn.start & ~(page_bytes - 1);
	const auto unmap_page = [&](SharedGuest &guest) {
		require(darwin_uml_static_vm_unmap_generation(
			guest.env.generation,
			reinterpret_cast<void *>(guest.env.bias + page_offset),
			page_bytes) == 0);
	};
	const auto put_changed_page = [&](SharedFrame &frame) {
		frame.backing.put(page_offset, std::span{changed.bytes}.subspan(
			page_offset, page_bytes));
	};
	const auto map_page = [&](SharedGuest &guest, const SharedFrame &frame) {
		require(darwin_uml_static_vm_map_generation(guest.env.generation,
			reinterpret_cast<void *>(guest.env.bias + page_offset),
			frame.backing.descriptor(), page_offset, page_bytes,
			PROT_READ | PROT_EXEC, 3, 0) == 0);
	};

	/* A publication moves the epoch. A guest that was not told (the code
	 * change history of its own address space has nothing) and compares
	 * the page again later, after a mapping change of its own, is not
	 * answered from the verdict filed for the old bytes. */
	{
		aot::clear_shared_verdicts();
		SharedFrame frame{image, generation++};
		auto first = guest(frame, true);
		auto second = guest(frame, true);
		const auto a = run_shared(*first, c);
		const auto b = run_shared(*second, c);
		require(same(want, a.result, c) && same(want, b.result, c));
		require(a.stats.enters > 0 && b.stats.enters > 0 &&
			(AOT_TEST_SHARED_VERDICTS != 0 ? b.stats.shared_hits >= 1 :
			 b.stats.shared_hits == 0));
		put_changed_page(frame);
		require(darwin_uml_static_vm_activate(first->env.generation) == 0);
		first->env.memory.invalidate_code(first->env.bias + fn.start);
		unmap_page(*second);
		map_page(*second, frame);
		const auto d = run_shared(*second, c);
		require(same(want_changed, d.result, c));
		require(d.stats.verifications >= 1 && d.stats.poisoned >= 1);
		/* The verdict filed for the old bytes names this frame, this page
		 * and the epoch before the publication: the page is hashed again
		 * because the epoch moved, and the counter says so (a build that
		 * shares nothing files nothing and has none to blame). */
		require(AOT_TEST_SHARED_VERDICTS != 0 ?
			d.stats.epoch_rehashes >= 1 : d.stats.epoch_rehashes == 0);
	}

	/* The last executable mapping of the page goes, the frame is reused for
	 * other bytes with no publication at all, and a guest that kept its
	 * place in the library maps the frame again: the epoch moved at the last
	 * departure, so the verdict filed for the old bytes is not answered. */
	{
		aot::clear_shared_verdicts();
		SharedFrame frame{image, generation++};
		auto first = guest(frame, true);
		auto second = guest(frame, true);
		const auto a = run_shared(*first, c);
		const auto b = run_shared(*second, c);
		require(same(want, a.result, c) && same(want, b.result, c));
		require(a.stats.enters > 0 && b.stats.enters > 0 &&
			(AOT_TEST_SHARED_VERDICTS != 0 ? b.stats.shared_hits >= 1 :
			 b.stats.shared_hits == 0));
		unmap_page(*first);
		unmap_page(*second);
		put_changed_page(frame);
		map_page(*second, frame);
		const auto d = run_shared(*second, c);
		require(same(want_changed, d.result, c));
		require(d.stats.verifications >= 1 && d.stats.poisoned >= 1);
		/* The frame came back for other bytes under a later epoch: the
		 * counter is how a guest run sees that a frame was given back. */
		require(AOT_TEST_SHARED_VERDICTS != 0 ?
			d.stats.epoch_rehashes >= 1 : d.stats.epoch_rehashes == 0);
	}

	/* The backing is replaced: the same address, the same offset and the
	 * kernel's fact, from another backing object that holds other bytes. This
	 * models what a loop device whose file was swapped would do to the host (a
	 * read of the same block lands in a new page, under the same mapping
	 * facts): another frame with another record. No loop device and no kernel
	 * is operated here. Nothing filed for the old frame answers the new one,
	 * for a guest that maps the new backing from the start and for one that
	 * has the old one under it and is given the new. */
	{
		aot::clear_shared_verdicts();
		SharedFrame frame{image, generation++};
		SharedFrame replacement{changed, generation++};
		auto first = guest(frame, true);
		auto second = guest(frame, true);
		const auto a = run_shared(*first, c);
		const auto b = run_shared(*second, c);
		require(same(want, a.result, c) && same(want, b.result, c));
		require(a.stats.enters > 0 && b.stats.enters > 0 &&
			(AOT_TEST_SHARED_VERDICTS != 0 ? b.stats.shared_hits >= 1 :
			 b.stats.shared_hits == 0));
		auto third = guest(replacement, true);
		const auto d = run_shared(*third, c);
		require(same(want_changed, d.result, c));
		require(d.stats.shared_hits == 0 && d.stats.verifications >= 1 &&
			d.stats.poisoned >= 1);
		/* Another frame is another identity: nothing was filed under an
		 * older epoch of it, and no hash is blamed on one. */
		require(d.stats.epoch_rehashes == 0);
		unmap_page(*second);
		map_page(*second, replacement);
		const auto e = run_shared(*second, c);
		require(same(want_changed, e.result, c));
		require(e.stats.verifications >= 1 && e.stats.poisoned >= 1);
	}
	std::fprintf(stderr, "ok\n");
}

/* One page of a shared frame as another address space of the guest maps it,
 * writable (and executable or not, or not mapped at all): the guest's store
 * through that view and the ways the change is published, by the guest's own
 * code synchronization (IC IVAU) and by the kernel when it writes the frame
 * on a process's behalf. A generation that ran code of the frame is told
 * only if it was registered as a holder of the frame when the change was
 * published, and a verdict filed for the frame is answered only under the
 * stamp the publication left. The kernel's write through an address space
 * that does not map the page yet (its software page table fills at faults)
 * names no frame: it tells every generation and moves every stamp at once. */
struct ForeignWriter final {
	static constexpr std::uint64_t view = 0x4b00'0000ULL;

	/* `executable`: the view is guest code too, as the view of a process
	 * that runs the pages it writes; `cannot_execute`: as a process that
	 * writes a file maps it; `unmapped`: the address space maps nothing there
	 * when the kernel writes. */
	enum class View { cannot_execute, executable, unmapped };

	ForeignWriter(unsigned long generation_, SharedFrame &frame_,
		std::uint64_t page_offset_, View kind)
		: generation{generation_}, frame{frame_}, page_offset{page_offset_},
		  mapped{kind != View::unmapped}
	{
		if (!mapped)
			return;
		const bool executable = kind == View::executable;
		require(darwin_uml_static_vm_map_generation(generation,
			reinterpret_cast<void *>(view), frame.backing.descriptor(),
			page_offset, page_bytes, executable ?
				PROT_READ | PROT_WRITE | PROT_EXEC : PROT_READ | PROT_WRITE,
			executable ? 1 : 0, 1) == 0);
	}
	~ForeignWriter()
	{
		if (mapped)
			require(darwin_uml_static_vm_unmap_generation(generation,
				reinterpret_cast<void *>(view), page_bytes) == 0);
	}
	ForeignWriter(const ForeignWriter &) = delete;
	ForeignWriter &operator=(const ForeignWriter &) = delete;

	/* `kernel_write` names the address written (an AArch32 cacheflush);
	 * `kernel_write_frame` names the frame the kernel holds, as a ptrace or
	 * process_vm_writev() store does. */
	enum class Route { store_and_synchronize, kernel_write,
		kernel_write_frame };

	/* The page of `image` is stored, and the change published by `route`. */
	void change(const LibraryImage &image, Route route)
	{
		frame.backing.put(page_offset, std::span{image.bytes}.subspan(
			page_offset, page_bytes));
		if (route == Route::store_and_synchronize) {
			require(darwin_uml_static_vm_activate(generation) == 0);
			std::atomic_thread_fence(std::memory_order_seq_cst);
			require(darwin_uml_static_vm_synchronize_code(view) == 0);
		} else if (route == Route::kernel_write_frame) {
			require(darwin_uml_static_vm_invalidate_code_frame(generation,
				view, page_bytes, frame.backing.descriptor(),
				page_offset) == 0);
		} else {
			require(darwin_uml_static_vm_invalidate_code_generation(
				generation, view, page_bytes) == 0);
		}
	}

	unsigned long generation;
	SharedFrame &frame;
	std::uint64_t page_offset;
	bool mapped;
};

/* Two generations map one frame; one runs a function of it, and the other
 * changes the frame's bytes. The first must run what the engine runs on the
 * new bytes, and never the native code of the old ones: the memory layer
 * publishes the change to the generations that registered as holders of the
 * frame (by their fetch of it), which is how a generation whose own history
 * has nothing hears of it. This is the engine alone, a page the rewritten
 * code compared by hashing it, and a page it was told by the table of
 * verdicts is the same (which fetches nothing of its own beyond the
 * registration), each for the guest's store with its code synchronization
 * and for the kernel's write, which names the address written or the frame
 * the kernel holds.
 *
 * The same rows run with the writer's view in the executing generation
 * itself: a generation that holds the frame is told of a change made
 * through another of its own views of it (a mapping that cannot execute, an
 * executable alias, or no mapping at all when the kernel writes) at every
 * address, as the entry it verified or warmed stands at the library's, not
 * at the view's. The foreign generation is the control. */
[[gnu::noinline]]
void another_generations_change_reaches_native_code(
	const LibraryImage &image, const std::vector<Function> &functions)
{
	using Route = ForeignWriter::Route;
	const auto bias = Fixture::bias;
	/* The function the row runs is one the rewritten code runs whole, with
	 * no instruction left to the engine: the engine fetches nothing of the
	 * page, so the registration of the generation as a holder of the frame
	 * is the binder's own. Its first word is the one `changed` replaces, so
	 * that the new bytes give another result. */
	unsigned long probe_generation = 6400;
	std::optional<Function> chosen;
	for (std::size_t i = 0; i < functions.size() && i < 120 && !chosen; ++i) {
		const auto &f = functions[i];
		const auto fixup = f.start / page_bytes * page_bytes + page_bytes - 4;
		if (f.start + 4 >= fixup)
			continue;
		SplitMix probe_rng{29};
		auto k = make_case(probe_rng, bias, f.start, false, true,
			arena_base(bias));
		enterable(k);
		aot::set_enabled(false);
		auto ref = std::make_unique<Fixture>(probe_generation++, image);
		const auto w = drive(ref->env, ref->arena, k, max_blocks);
		ref.reset();
		if (!w.done())
			continue;
		auto got = std::make_unique<Fixture>(probe_generation++, image);
		aot::reset_stats();
		aot::set_enabled(true);
		const auto h = drive(got->env, got->arena, k, aot_max_blocks);
		aot::set_enabled(false);
		const auto st = aot::stats();
		got.reset();
		if (!identical(w, h) || st.enters == 0 || st.steps != 0)
			continue;
		const auto other = with_equal_digest(image, f.start);
		auto ref_other = std::make_unique<Fixture>(probe_generation++, other);
		const auto w_other = drive(ref_other->env, ref_other->arena, k,
			max_blocks);
		ref_other.reset();
		if (!identical(w, w_other))
			chosen = f;
	}
	require(chosen.has_value());
	const auto fn = *chosen;
	SplitMix rng{29};
	auto c = make_case(rng, bias, fn.start, false, true, arena_base(bias));
	enterable(c);
	unsigned long generation = 6100;
	const auto changed = with_equal_digest(image, fn.start);
	const auto page_offset = fn.start & ~(page_bytes - 1);
	aot::set_enabled(false);
	auto ref = std::make_unique<Fixture>(generation++, image);
	const auto want = drive(ref->env, ref->arena, c, max_blocks);
	ref.reset();
	auto ref_changed = std::make_unique<Fixture>(generation++, changed);
	const auto want_changed = drive(ref_changed->env, ref_changed->arena, c,
		max_blocks);
	ref_changed.reset();
	require(want.done());
	require(!identical(want, want_changed));
	std::fprintf(stderr, "function at %llx; ",
		static_cast<unsigned long long>(fn.start));
	const auto guest = [&](const SharedFrame &frame) {
		return std::make_unique<SharedGuest>(generation++, image, frame,
			true);
	};
	const auto run_engine = [&](SharedGuest &g) {
		aot::set_enabled(false);
		return drive(g.env, g.arena, c, max_blocks);
	};
	using View = ForeignWriter::View;
	std::fprintf(stderr, "foreign and own-generation changes: ");
	/* The first five arms write through a view of another generation, the
	 * next five through a view of the executing guest's own; the last six
	 * are the kernel's writes of those that name the frame they hold, three
	 * for each. */
	for (const auto arm_of_sixteen : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
		     12, 13, 14, 15}) {
		const bool named = arm_of_sixteen >= 10;
		const bool own_generation = named ? arm_of_sixteen >= 13 :
			arm_of_sixteen >= 5;
		const auto arm = named ? std::array{1, 3, 4}[arm_of_sixteen % 3] :
			arm_of_sixteen % 5;
		const auto writer_generation = [&](const SharedGuest &g) {
			return own_generation ? g.env.generation : generation++;
		};
		const auto route = named ? Route::kernel_write_frame :
			arm % 2 == 0 && arm < 4 ?
			Route::store_and_synchronize : Route::kernel_write;
		const auto view_kind = arm == 2 || arm == 3 ? View::executable :
			arm == 4 ? View::unmapped : View::cannot_execute;
		const std::string name = std::string{own_generation ? "own " : ""} +
			std::string{route == Route::kernel_write_frame ?
			"named kernel write" : route == Route::kernel_write ?
			"kernel write" : "store and synchronization"} + (
			view_kind == View::executable ? " through an executable view" :
			view_kind == View::unmapped ? " through a view nothing maps" :
			" through a view that cannot execute");

		/* A page that was refused and is retried. Its mapping lacked the
		 * kernel's fact, so the binder refused it and backed off; the kernel
		 * then gave the mapping the fact, which publishes no code change, and
		 * a later find retries the verification on the slot it already has,
		 * with no new binding of the page. The table answers it, which reads
		 * no byte. The generation is registered already, by the binding's
		 * own fetch and the engine's fetches of the refused page (a change
		 * that makes the slot stale tells it again), so a change must reach
		 * this retry however the verification itself registers. */
		{
			aot::clear_shared_verdicts();
			SharedFrame frame{image, generation++};
			auto warm = guest(frame);
			auto retried = std::make_unique<SharedGuest>(generation++, image,
				frame, false);
			ForeignWriter writer{writer_generation(*retried), frame,
				page_offset, view_kind};
			require(same(want, run_shared(*warm, c).result, c));
			const auto a = run_shared(*retried, c);
			require(same(want, a.result, c) && a.stats.enters == 0 &&
				a.stats.mutable_pages >= 1);
			require(darwin_uml_static_vm_activate(retried->env.generation) == 0);
			require(darwin_uml_static_vm_map_generation(
				retried->env.generation, reinterpret_cast<void *>(bias),
				frame.backing.descriptor(), 0, image.pages * page_bytes,
				PROT_READ | PROT_EXEC, 3, 0) == 0);
			/* Every run binds the stub's page again (it is no library's).
			 * The run that enters verifies the pages it reaches, and binds
			 * each of them but the one that was refused before: that page
			 * keeps its slot and is only verified again. */
			SharedRun b;
			std::uint64_t refused_binds = 0;
			int refused = 0;
			for (int attempt = 0; attempt < 5000 && b.stats.enters == 0;
			     ++attempt) {
				refused_binds = b.stats.binds;
				b = run_shared(*retried, c);
				require(same(want, b.result, c));
				refused += b.stats.enters == 0 ? 1 : 0;
			}
			/* The page's retry is answered by the table when verdicts are
			 * shared, and by one hash when they are not. */
			require(b.stats.enters > 0 && b.stats.steps == 0 && refused > 1 &&
				answered_by_the_table(b.stats) &&
				b.stats.binds - refused_binds + 1 ==
					(AOT_TEST_SHARED_VERDICTS != 0 ? b.stats.shared_hits :
					 b.stats.verifications));
			writer.change(changed, route);
			const auto d = run_shared(*retried, c);
			if (!same(want_changed, d.result, c) || d.stats.enters != 0) {
				std::fprintf(stderr, "  a page retried after a refusal ran "
					"old code after a %s: enters %llu\n", name.c_str(),
					static_cast<unsigned long long>(d.stats.enters));
				require(false);
			}
			require(d.stats.poisoned >= 1);
		}

		/* The engine alone: it translated the function, and runs the new
		 * bytes after the change. */
		{
			SharedFrame frame{image, generation++};
			auto first = guest(frame);
			ForeignWriter writer{writer_generation(*first), frame,
				page_offset, view_kind};
			require(same(want, run_engine(*first), c));
			require(same(want, run_engine(*first), c));
			writer.change(changed, route);
			if (!same(want_changed, run_engine(*first), c)) {
				std::fprintf(stderr, "  the engine alone ran old code "
					"after a %s\n", name.c_str());
				require(false);
			}
		}

		/* A page the rewritten code compared by hashing it. */
		{
			aot::clear_shared_verdicts();
			SharedFrame frame{image, generation++};
			auto first = guest(frame);
			ForeignWriter writer{writer_generation(*first), frame,
				page_offset, view_kind};
			const auto a = run_shared(*first, c);
			require(same(want, a.result, c));
			require(a.stats.enters > 0 && a.stats.verifications >= 1);
			writer.change(changed, route);
			const auto d = run_shared(*first, c);
			if (!same(want_changed, d.result, c) || d.stats.enters != 0) {
				std::fprintf(stderr, "  rewritten code ran old code after "
					"a %s: enters %llu\n", name.c_str(),
					static_cast<unsigned long long>(d.stats.enters));
				require(false);
			}
			require(d.stats.poisoned >= 1);
		}

		/* A page a generation was told of by the table. Both generations
		 * ran the original; the frame changed and came back, so the
		 * first compares the page and the second is answered by the table,
		 * which fetches nothing of the page except for the registration;
		 * then the frame changes again. */
		{
			aot::clear_shared_verdicts();
			SharedFrame frame{image, generation++};
			auto first = guest(frame);
			auto second = guest(frame);
			ForeignWriter writer{writer_generation(*second), frame,
				page_offset, view_kind};
			const auto w = run_shared(*first, c);
			require(same(want, w.result, c) && w.stats.steps == 0);
			require(same(want, run_shared(*second, c).result, c));
			writer.change(changed, route);
			writer.change(image, route);
			const auto a = run_shared(*first, c);
			require(same(want, a.result, c));
			require(a.stats.enters > 0 && a.stats.verifications >= 1);
			const auto b = run_shared(*second, c);
			require(same(want, b.result, c));
			require(b.stats.enters > 0 && b.stats.steps == 0 &&
				answered_by_the_table(b.stats));
			writer.change(changed, route);
			const auto d = run_shared(*second, c);
			if (!same(want_changed, d.result, c) || d.stats.enters != 0) {
				std::fprintf(stderr, "  a generation told by the table ran "
					"old code after a %s: enters %llu\n", name.c_str(),
					static_cast<unsigned long long>(d.stats.enters));
				require(false);
			}
			require(d.stats.poisoned >= 1);
			const auto e = run_shared(*first, c);
			require(same(want_changed, e.result, c) && e.stats.enters == 0);
		}
		std::fprintf(stderr, "%s ok; ", name.c_str());
	}
	aot::set_enabled(false);
	std::fprintf(stderr, "\n");
}

/* A loop over a buffer takes the inline hit path: adler32_z over 40000 bytes
 * (the library's own checksum loop, 0xd020), tagged or not, steps only for
 * the pages it first touches. */
[[gnu::noinline]]
void inline_hit_path_is_taken(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	constexpr std::uint64_t adler32_z = 0xd020;
	bool found = false;
	for (const auto &f : functions)
		found = found || f.start == adler32_z;
	require(found);
	const auto bias = Fixture::bias;
	for (int tagged = 0; tagged < 2; ++tagged) {
		Fixture ref{static_cast<unsigned long>(4251 + 2 * tagged), image},
			got{static_cast<unsigned long>(4252 + 2 * tagged), image};
		SplitMix rng{23};
		auto c = make_case(rng, bias, adler32_z, false, true,
			arena_base(bias));
		const std::uint64_t tag = tagged ? 0xb4ULL << 56 : 0;
		c.cpu.registers[0] = 1;
		c.cpu.registers[1] = tag | arena_base(bias);
		c.cpu.registers[2] = 40000;
		aot::set_enabled(false);
		const auto want = drive(ref.env, ref.arena, c, max_blocks);
		require(want.finished);
		aot::reset_stats();
		aot::set_enabled(true);
		const auto have = drive(got.env, got.arena, c, aot_max_blocks);
		aot::set_enabled(false);
		require(same(want, have, c));
		const auto st = aot::stats();
		std::fprintf(stderr, "hit path (%s): enters %llu steps %llu blocks %zu\n",
			tagged ? "tagged" : "untagged",
			static_cast<unsigned long long>(st.enters),
			static_cast<unsigned long long>(st.steps), have.blocks);
		require(st.enters > 0);
		require(st.steps < 100);
	}
}

/* Every access that leaves the page of the last one refills, and the
 * refill callback leaves every register and flag the thunk saves scrambled:
 * the results must still be the engine's. A release library's functions
 * touch memory in nearly every case, so its cases refill more than once on
 * average; the generated fixture's functions often touch none, and only a
 * share of its cases refill. */
[[gnu::noinline]]
void refill_keeps_state(const LibraryImage &image,
	const std::vector<Function> &functions, bool fixture_library)
{
	SplitMix rng{29};
	const auto bias = Fixture::bias;
	Fixture ref{4261, image}, got{4262, image};
	aot::reset_stats();
	aot::set_refill_stress(true);
	std::size_t compared = 0;
	for (std::size_t f = 0; f < functions.size(); ++f)
		for (int which = 0; which < 3; ++which) {
			const auto pc = which == 0 ? functions[f].start :
				functions[f].start + 4 * rng.below(
					(functions[f].end - functions[f].start) / 4);
			const auto c = make_case(rng, bias, pc, rng.below(4) == 0,
				true, arena_base(bias));
			aot::set_enabled(false);
			const auto want = drive(ref.env, ref.arena, c, max_blocks);
			if (!want.done())
				continue;
			aot::set_enabled(true);
			const auto have = drive(got.env, got.arena, c,
				aot_max_blocks);
			aot::set_enabled(false);
			require(same(want, have, c));
			++compared;
		}
	aot::set_refill_stress(false);
	const auto st = aot::stats();
	std::fprintf(stderr, "refill stress: %zu cases; enters %llu refills %llu "
		"refill failures %llu steps %llu\n", compared,
		static_cast<unsigned long long>(st.enters),
		static_cast<unsigned long long>(st.refills),
		static_cast<unsigned long long>(st.refill_failures),
		static_cast<unsigned long long>(st.steps));
	require(compared > 20);
	require(st.refills > (fixture_library ? compared / 16 : compared));
}

/* A library whose last code page is not mapped yet: the code on the pages
 * that are mapped is entered, a function on the missing page is the
 * engine's, and once the page is mapped its code is entered too. */
[[gnu::noinline]]
void partially_mapped_library(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	std::uint64_t code_end = 0;
	for (const auto &f : functions)
		code_end = std::max(code_end, f.end);
	const auto last_page = (code_end - 1) / page_bytes;
	require(last_page >= 1);
	/* Whole functions below the last page, and a pc on the last page in
	 * each function that reaches it. */
	std::vector<Function> below;
	std::vector<std::uint64_t> on_last;
	for (const auto &f : functions) {
		if (f.end <= last_page * page_bytes)
			below.push_back(f);
		else
			on_last.push_back(std::max(f.start, last_page * page_bytes));
	}
	require(below.size() > 8);
	require(!on_last.empty());
	SplitMix rng{31};
	const auto bias = Fixture::bias;
	Environment ref{4281, bias, image};
	Arena ref_arena{ref.generation, arena_base(bias)};
	Environment got{4282, bias, image, last_page};
	Arena got_arena{got.generation, arena_base(bias)};
	aot::reset_stats();
	std::size_t compared = 0;
	for (std::size_t f = 0; f < below.size() && compared < 6; ++f) {
		const auto c = make_case(rng, bias, below[f].start, false, true,
			arena_base(bias));
		aot::set_enabled(false);
		auto want = drive(ref, ref_arena, c, max_blocks);
		if (!want.done())
			continue;
		aot::set_enabled(true);
		const auto have = drive(got, got_arena, c, aot_max_blocks);
		aot::set_enabled(false);
		/* The code ran with the tail of the library missing: only
		 * calls into the missing page differ from a full mapping, so
		 * compare cases that never fault on an instruction fetch. */
		if (have.faults.empty())
			require(same(want, have, c));
		++compared;
	}
	require(compared > 3);
	auto st = aot::stats();
	require(st.enters > 0);
	require(st.poisoned == 0);
	/* The missing page is not entered: it cannot be read. */
	const auto entered = st.enters;
	for (int attempt = 0; attempt < 3; ++attempt) {
		const auto c = make_case(rng, bias, on_last[0], false, true,
			arena_base(bias));
		aot::set_enabled(true);
		(void)drive(got, got_arena, c, 40);
		aot::set_enabled(false);
	}
	st = aot::stats();
	require(st.enters == entered);
	require(st.finds[2] > 0); /* not_ready */
	/* The loader maps the rest: the next time the page is due it is
	 * compared and the code on it is entered. */
	got.map_rest(image);
	std::size_t attempts = 0;
	while (aot::stats().enters == entered && attempts < 4000) {
		const auto c = make_case(rng, bias,
			on_last[attempts % on_last.size()], false, true,
			arena_base(bias));
		aot::set_enabled(true);
		(void)drive(got, got_arena, c, aot_max_blocks);
		aot::set_enabled(false);
		++attempts;
	}
	st = aot::stats();
	std::fprintf(stderr, "partial mapping: after the rest was mapped, entered "
		"after %zu cases; unreadable %llu\n", attempts,
		static_cast<unsigned long long>(st.unreadable));
	require(st.enters > entered);
	require(st.poisoned == 0);
	/* And the code on it behaves as the engine's. */
	for (std::size_t f = 0; f < on_last.size(); ++f) {
		const auto c = make_case(rng, bias, on_last[f], false, true,
			arena_base(bias));
		aot::set_enabled(false);
		const auto want = drive(ref, ref_arena, c, max_blocks);
		if (!want.done())
			continue;
		aot::set_enabled(true);
		const auto have = drive(got, got_arena, c, aot_max_blocks);
		aot::set_enabled(false);
		require(same(want, have, c));
	}
}

/* Two vCPUs of one address space run rewritten code at once, each with its
 * own binder, TLB and arena; every case ends as the engine's would. */
[[gnu::noinline]]
void two_vcpus(const LibraryImage &image, const std::vector<Function> &functions)
{
	constexpr std::uint64_t bias = 0x7163'0000'0000ULL;
	constexpr std::size_t per_thread = 120;
	const std::uint64_t base[2] = {arena_base(bias),
		arena_base(bias) + 0x40'0000};
	Environment ref{4291, bias, image};
	Arena ref_arena[2] = {{ref.generation, base[0]},
		{ref.generation, base[1]}};
	Environment env{4292, bias, image};
	Arena arena[2] = {{env.generation, base[0]}, {env.generation, base[1]}};
	Vcpu second;
	SplitMix rng{37};
	std::vector<Case> cases[2];
	std::vector<Result> want[2], have[2];
	aot::set_enabled(false);
	for (int t = 0; t < 2; ++t)
		for (std::size_t i = 0; i < per_thread; ++i) {
			const auto &f = functions[rng.below(functions.size())];
			cases[t].push_back(make_case(rng, bias, f.start +
				4 * rng.below((f.end - f.start) / 4), rng.below(3) == 0,
				rng.below(4) != 0, base[t]));
			want[t].push_back(drive_on(ref.executor, ref.memory,
				ref.generation, bias, ref_arena[t], cases[t].back(),
				max_blocks));
		}
	aot::reset_stats();
	aot::set_enabled(true);
	std::atomic<bool> go{false};
	std::thread threads[2];
	for (int t = 0; t < 2; ++t)
		threads[t] = std::thread{[&, t] {
			auto &executor = t == 0 ? env.executor : second.executor;
			auto &memory = t == 0 ? env.memory : second.memory;
			while (!go.load(std::memory_order_acquire))
				std::this_thread::yield();
			for (const auto &c : cases[t])
				have[t].push_back(drive_on(executor, memory,
					env.generation, bias, arena[t], c,
					aot_max_blocks));
		}};
	go.store(true, std::memory_order_release);
	for (auto &thread : threads)
		thread.join();
	aot::set_enabled(false);
	std::size_t compared = 0;
	for (int t = 0; t < 2; ++t)
		for (std::size_t i = 0; i < per_thread; ++i) {
			if (!want[t][i].done())
				continue;
			require(same(want[t][i], have[t][i], cases[t][i]));
			++compared;
		}
	const auto st = aot::stats();
	std::fprintf(stderr, "two vCPUs: %zu cases compared; enters %llu binds "
		"%llu steps %llu\n", compared,
		static_cast<unsigned long long>(st.enters),
		static_cast<unsigned long long>(st.binds),
		static_cast<unsigned long long>(st.steps));
	require(compared > per_thread);
	require(st.enters > compared / 2);
	require(st.binds >= 2);
	require(st.poisoned == 0);
}

/* Two libraries in one address space, each reaching the other through a
 * register (as a call through a PLT does): the branch stays in rewritten
 * code when the other library is verified. */
[[gnu::noinline]]
void cross_library_branches(const LibraryImage &image,
	const std::vector<Function> &functions, const LibraryImage &other,
	const std::vector<Function> &other_functions)
{
	constexpr std::uint64_t bias = 0x7173'0000'0000ULL;
	constexpr std::uint64_t other_bias = 0x7183'0000'0000ULL;
	Environment ref{4301, bias, image};
	Environment got{4302, bias, image};
	ref.add_library(other_bias, other);
	got.add_library(other_bias, other);
	Arena ref_arena{ref.generation, arena_base(bias)};
	Arena got_arena{got.generation, arena_base(bias)};
	g_cross_targets.clear();
	for (const auto &f : other_functions)
		g_cross_targets.push_back(other_bias + f.start);
	SplitMix rng{41};
	aot::reset_stats();
	std::size_t compared = 0;
	for (std::size_t round = 0; round < 3; ++round)
		for (std::size_t i = 0; i < functions.size(); ++i) {
			/* Some cases start in the other library, so that its
			 * instance is verified before library one branches. */
			const bool in_other = rng.below(4) == 0;
			const auto &f = in_other ?
				other_functions[rng.below(other_functions.size())] :
				functions[i];
			const auto at = in_other ? other_bias : bias;
			auto c = make_case(rng, at, f.start, false, true,
				arena_base(bias));
			c.pc_offset = f.start;
			aot::set_enabled(false);
			const auto want = drive_on(ref.executor, ref.memory,
				ref.generation, at, ref_arena, c, max_blocks);
			if (!want.done())
				continue;
			aot::set_enabled(true);
			const auto have = drive_on(got.executor, got.memory,
				got.generation, at, got_arena, c, aot_max_blocks);
			aot::set_enabled(false);
			require(same(want, have, c));
			++compared;
		}
	g_cross_targets.clear();
	const auto st = aot::stats();
	std::fprintf(stderr, "cross-library: %zu cases; enters %llu steps %llu "
		"cross hits %llu cross misses %llu\n", compared,
		static_cast<unsigned long long>(st.enters),
		static_cast<unsigned long long>(st.steps),
		static_cast<unsigned long long>(st.cross_hits),
		static_cast<unsigned long long>(st.cross_misses));
	require(compared > 20);
	/* A branch into the other library stays in rewritten code only where
	 * the binder holds both instances. */
	require(st.cross_hits > 0 || aot::instance_capacity() < 2);
	require(st.poisoned == 0);
}

/* What the engine does with each word of a library's functions. A word it
 * rejects as an illegal instruction must be one the rewriter leaves to it:
 * copied natively, the host raises the fault itself and ends the emulator
 * where the guest's task should get its signal. Code is entered at any word
 * (an indirect branch, a wild one), and data sits inside the code of a method
 * (the literal pool and jump tables of ART's compiled Java), where a word
 * may be a reserved encoding the instruction tables cannot tell from an
 * instruction. */
[[gnu::noinline]]
void words_the_engine_rejects_are_left_to_it(const LibraryImage &image,
	const std::vector<Function> &functions,
	const std::set<std::uint64_t> &steps)
{
	constexpr std::uint64_t bias = 0x7123'0000'0000ULL;
	/* The engine keeps what it translated: a fresh environment now and
	 * then holds that to a bounded size. */
	constexpr std::size_t per_environment = 1U << 14;
	unsigned long generation = 6001;
	std::unique_ptr<Environment> env;
	SplitMix rng{61};
	auto c = make_case(rng, bias, 0, false, true, arena_base(bias));
	enterable(c);
	aot::set_enabled(false);
	std::size_t words = 0, rejected = 0, defects = 0;
	for (const auto &f : functions)
		for (auto at = f.start; at < f.end; at += 4) {
			if (words++ % per_environment == 0) {
				env = std::make_unique<Environment>(generation++, bias, image);
			}
			const auto illegal = [&](const Case &state) {
				auto cpu = state.cpu;
				cpu.pc = bias + at;
				const auto out = run_selected(env->executor, env->memory,
					env->generation, cpu);
				return out.event == EngineEvent::illegal_instruction &&
					cpu.pc == bias + at;
			};
			/* A memory copy or set stage is rejected unless the registers
			 * hold what the stage before it leaves, whatever its
			 * encoding: the rewriter steps the encodings that are
			 * invalid (mops-invalid), and the rows of its own hold the
			 * rest against the engine. */
			std::uint32_t word;
			std::memcpy(&word, &image.bytes[at], 4);
			if ((word & 0x3f200c00U) == 0x19000400U ||
			    (word & 0x3f200c00U) == 0x1d000400U ||
			    (word & 0x3fe00c00U) == 0x19c00400U ||
			    !illegal(c))
				continue;
			/* Another word the registers decide is no reserved encoding
			 * either: the word has to be rejected in every state. */
			bool always = true;
			for (unsigned again = 0; again < 7 && always; ++again) {
				auto other = make_case(rng, bias, 0, false, true,
					arena_base(bias));
				enterable(other);
				always = illegal(other);
			}
			if (!always)
				continue;
			++rejected;
			if (steps.contains(at))
				continue;
			if (++defects <= 60) {
				std::fprintf(stderr, "  rejected by the engine, not left to "
					"it: %llx: %08x\n", static_cast<unsigned long long>(at),
					word);
			}
		}
	std::fprintf(stderr, "words: %zu, rejected by the engine %zu, rewritten "
		"anyway %zu\n", words, rejected, defects);
	require(defects == 0);
}

/* A loop whose backward branch lands inside another unwind range
 * (tools/aot/fixture.py): every pass through it is charged against the
 * entry's budget, so a long run comes back to the engine about every 4096
 * instructions and ends where the engine's run ends. */
[[gnu::noinline]]
void loop_through_another_range(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	/* The two ranges of the loop, the PLT stub after them and the
	 * instruction forms. */
	require(functions.size() >= 3);
	require(functions[0].start == 0x1000);
	const auto bias = Fixture::bias;
	Fixture ref{4311, image}, got{4312, image};
	SplitMix rng{43};
	auto c = make_case(rng, bias, functions[0].start, false, true,
		arena_base(bias));
	c.cpu.registers[0] = 0;
	c.cpu.registers[1] = 0;
	c.cpu.registers[3] = 50000;
	aot::set_enabled(false);
	const auto want = drive(ref.env, ref.arena, c, max_blocks);
	require(want.finished);
	require(want.cpu.registers[0] == 50000);
	aot::reset_stats();
	aot::set_enabled(true);
	const auto have = drive(got.env, got.arena, c, aot_max_blocks);
	aot::set_enabled(false);
	require(same(want, have, c));
	const auto st = aot::stats();
	std::fprintf(stderr, "loop through another range: %llu entries, longest "
		"block %zu instructions, %zu blocks\n",
		static_cast<unsigned long long>(st.enters), have.max_block,
		have.blocks);
	require(st.enters >= 40);
	require(have.max_block <= 4096 + 64);
}

/* The instructions between two polls are the instructions that ran. A
 * framework made with UML_AOT_COUNT_RETIRED counts each one it starts; the
 * product's layout reports the budget the entry spent, which the same bound
 * covers. */
constexpr std::uint64_t entry_bound = 4096 + 32 + 2; /* RUN_LIMIT, rewrite.py */

/* A state rewritten code may be entered with: SP aligned and none of the
 * FPCR controls the entry refuses. */
void enterable(Case &c)
{
	c.cpu.sp &= ~0xfULL;
	c.cpu.fpcr &= ~static_cast<std::uint32_t>(arm64::fpcr_afp_controls);
	/* Nor under a floating-point trap enable, which the engine runs one
	 * instruction at a time. */
	c.cpu.fpcr &= ~0x9f00U;
}

/* Forty instructions and a RET that returns to the start of the run: a lap
 * the budget used to charge four for (libz 0x11638..0x116d8). */
[[gnu::noinline]]
void return_loop_is_bounded(const LibraryImage &image)
{
	constexpr std::uint64_t first = 0x11638, last = 0x116d8;
	std::uint32_t word;
	std::memcpy(&word, &image.bytes[last], 4);
	require(word == 0xd65f03c0U); /* ret */
	const auto bias = Fixture::bias;
	Fixture got{4321, image};
	SplitMix rng{47};
	auto c = make_case(rng, bias, first, false, true, arena_base(bias));
	c.cpu.registers[30] = bias + first;
	enterable(c);
	aot::reset_stats();
	aot::set_enabled(true);
	const auto run = drive(got.env, got.arena, c, 60);
	aot::set_enabled(false);
	const auto st = aot::stats();
	std::fprintf(stderr, "return loop: %llu entries, longest entry %llu "
		"instructions (%zu blocks)\n",
		static_cast<unsigned long long>(st.enters),
		static_cast<unsigned long long>(st.max_retired), run.blocks);
	require(st.enters >= 20);
	require(st.max_retired > 1000);
	require(st.max_retired <= entry_bound);
}

/* STP X29, X30, [SP, #-32]! (libz 0x8028) entered with SP = 8 (mod 16): the
 * engine faults without writing, so rewritten code is not entered. */
[[gnu::noinline]]
void misaligned_sp_at_a_pair_store(const LibraryImage &image)
{
	constexpr std::uint64_t at = 0x8028;
	std::uint32_t word;
	std::memcpy(&word, &image.bytes[at], 4);
	require(word == 0xa9be7bfdU);
	const auto bias = Fixture::bias;
	Fixture ref{4331, image}, got{4332, image};
	SplitMix rng{53};
	auto c = make_case(rng, bias, at, false, true, arena_base(bias));
	c.cpu.sp = (c.cpu.sp & ~0xfULL) - 8;
	aot::reset_stats();
	aot::set_enabled(false);
	const auto want = drive(ref.env, ref.arena, c, max_blocks);
	aot::set_enabled(true);
	const auto have = drive(got.env, got.arena, c, aot_max_blocks);
	aot::set_enabled(false);
	require(!want.faults.empty());
	require(same(want, have, c));
	const auto st = aot::stats();
	require(st.refusals[1] > 0);
}

/* An exclusive monitor the gadget engine opened (LDXR, then a branch into
 * the library) is the engine's to complete: nothing is entered. */
[[gnu::noinline]]
void open_exclusive_is_not_entered(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	const auto bias = Fixture::bias;
	Fixture ref{4341, image}, got{4342, image};
	SplitMix rng{59};
	auto c = make_case(rng, bias, stub_offset + 4, false, true,
		arena_base(bias));
	enterable(c);
	c.cpu.registers[1] = arena_base(bias) + 0x40;
	c.cpu.registers[2] = bias + functions[3].start;
	aot::reset_stats();
	aot::set_enabled(false);
	const auto want = drive(ref.env, ref.arena, c, max_blocks);
	aot::set_enabled(true);
	const auto have = drive(got.env, got.arena, c, aot_max_blocks);
	aot::set_enabled(false);
	require(same(want, have, c));
	const auto st = aot::stats();
	require(st.refusals[2] > 0);
	require(st.enters == 0);
}

/* A task's floating-point state the runtime has not loaded yet
 * (CpuState::defer_fp_load) keeps its FPCR aside: the register file's
 * `fpcr` is stale until the first access, and the entry conditions must
 * read the guest's. An FPCR with FIZ, AH or NEP is the engine's alone;
 * the same state without them is entered, whatever the stale value holds
 * (both directions: stale zero under current bits, stale bits under a
 * current zero), and either way the result is the engine's. */
[[gnu::noinline]]
void deferred_fp_state_is_judged_by_its_guest_fpcr(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	const auto bias = Fixture::bias;
	struct Combination final {
		std::uint32_t stale, guest;
	};
	constexpr Combination combinations[] = {
		{0, 1}, {0, 2}, {0, 4}, {0, 7}, {1, 0}, {2, 0}, {4, 0}, {7, 0},
		{7, 7}, {0, 0}};
	unsigned long generation = 4500;
	for (const auto &combination : combinations) {
		Fixture ref{generation++, image}, got{generation++, image};
		SplitMix rng{67};
		auto c = make_case(rng, bias, functions[3].start, false, true,
			arena_base(bias));
		enterable(c);
		std::vector<std::byte> saved(sizeof(c.cpu.vectors));
		std::memcpy(saved.data(), c.cpu.vectors.data(), saved.size());
		const std::uint32_t guest_fpcr =
			(c.cpu.fpcr & ~0x7U) | combination.guest;
		c.cpu.defer_fp_load(saved.data(), guest_fpcr, 0, ~0U);
		/* The register file's own, stale value: not loaded yet. */
		c.cpu.fpcr = combination.stale;
		c.cpu.fpsr = 0;
		require(!c.cpu.fp_loaded);
		const bool refused = combination.guest != 0;
		aot::reset_stats();
		aot::set_enabled(false);
		auto want = drive(ref.env, ref.arena, c, max_blocks);
		aot::set_enabled(true);
		auto have = drive(got.env, got.arena, c, aot_max_blocks);
		aot::set_enabled(false);
		/* Whether a run reached the state or not, compare the guest's. */
		want.cpu.access_fp_state();
		have.cpu.access_fp_state();
		require(want.done());
		require(same(want, have, c));
		const auto st = aot::stats();
		std::fprintf(stderr, "deferred FP state, stale %u guest %u: %llu "
			"entries, %llu fp refusals\n", combination.stale,
			combination.guest, static_cast<unsigned long long>(st.enters),
			static_cast<unsigned long long>(st.refusals[3]));
		if (refused) {
			require(st.refusals[3] > 0);
			require(st.enters == 0);
		} else {
			require(st.refusals[3] == 0);
			require(st.enters > 0);
		}
	}
}

/* Another vCPU's code change, published in the window between the
 * dispatcher reading the sequences and AOT looking its entry up. */
struct WindowChange final {
	Environment *env;
	std::uint64_t offset;
	std::uint32_t word;
	bool fired;
	/* Publish from another thread (which the window waits for) rather than
	 * from the vCPU's own, by the generation form of the bridge. */
	bool other_thread;
	int rc;
};

void publish_in_the_window(void *context)
{
	auto &change = *static_cast<WindowChange *>(context);
	if (change.fired)
		return;
	change.fired = true;
	if (change.other_thread) {
		std::thread other{[&change] {
			change.env->backing.put32(change.offset, change.word);
			change.rc = darwin_uml_static_vm_invalidate_code_generation(
				change.env->generation,
				change.env->bias + change.offset, 4);
		}};
		other.join();
		return;
	}
	change.env->backing.put32(change.offset, change.word);
	change.env->memory.invalidate_code(change.env->bias + change.offset);
}

/* `isb; mov x0, #1; ret` is entered after a change to the MOV was published
 * in the window. The instance was verified before the change and the
 * engine's ISB sees the change (it compares with the sequence the dispatcher
 * read), so the run must leave at the ISB and look the MOV up again at the
 * new sequence, which finds it changed; rewritten code must not run it. */
[[gnu::noinline]]
void code_change_in_the_dispatch_window(const LibraryImage &image,
	const std::vector<Function> &functions, bool other_thread)
{
	/* The fixture's `isb; mov x0, #1` form (tools/aot/fixture.py). */
	const Function *found = nullptr;
	for (const auto &f : functions) {
		std::uint32_t words[2];
		std::memcpy(words, &image.bytes[f.start], sizeof(words));
		if (words[0] == 0xd5033fdfU && words[1] == 0xd2800020U)
			found = &f;
	}
	require(found != nullptr);
	const auto form = *found;
	const std::uint32_t patched = 0xd2800f00U; /* mov x0, #0x78 */
	const auto bias = Fixture::bias;
	Fixture ref{4371UL + (other_thread ? 2 : 0), image},
		got{4372UL + (other_thread ? 2 : 0), image};
	SplitMix rng{71};
	auto c = make_case(rng, bias, form.start, false, true, arena_base(bias));
	enterable(c);
	/* The engine, running the changed code. */
	ref.env.backing.put32(form.start + 4, patched);
	ref.env.memory.invalidate_code(ref.env.bias + form.start + 4);
	aot::set_enabled(false);
	const auto want = drive(ref.env, ref.arena, c, max_blocks);
	require(want.finished);
	require(want.cpu.registers[0] == 0x78);
	/* AOT code verified against the original text, then the window. */
	aot::reset_stats();
	aot::set_enabled(true);
	const auto warm = drive(got.env, got.arena, c, aot_max_blocks);
	require(warm.finished);
	require(warm.cpu.registers[0] == 1);
	const auto entered = aot::stats().enters;
	require(entered > 0);
	WindowChange change{&got.env, form.start + 4, patched, false,
		other_thread, 0};
	aot::set_run_hook(publish_in_the_window, &change);
	const auto have = drive(got.env, got.arena, c, aot_max_blocks);
	aot::set_run_hook(nullptr, nullptr);
	aot::set_enabled(false);
	require(change.fired);
	require(!other_thread || change.rc == 0);
	require(same(want, have, c));
	const auto st = aot::stats();
	std::fprintf(stderr, "code change in the dispatch window (%s): %llu "
		"entries before, %llu after\n",
		other_thread ? "from a second thread" : "from the vCPU's own",
		static_cast<unsigned long long>(entered),
		static_cast<unsigned long long>(st.enters));
	/* Entered once, at the ISB; the changed page is no library page. */
	require(st.enters == entered + 1);
}

/* The fixture's page edges (tools/aot/fixture.py: EDGES): functions on one
 * text page that leave it for the next page by each kind of edge. The
 * target there adds 100 to x0 and returns through x15; the function that
 * falls into the page adds 4 on it (1 and 2 on the page before). */
struct PageEdge final {
	const char *name;
	std::uint64_t at;
};
constexpr PageEdge page_edges[] = {
	{"fall", 0x3ff8}, {"b", 0x3000}, {"bl", 0x3020}, {"bcond", 0x3040},
	{"cbz", 0x3060}, {"tbz", 0x3080}, {"br", 0x30a0}, {"blr", 0x30c0},
	{"ret", 0x30e0}};
constexpr std::uint64_t edge_next_page = 0x4000, edge_target = 0x4100;

[[nodiscard]] LibraryImage with_next_page_changed(const LibraryImage &image)
{
	LibraryImage other = image;
	const std::uint32_t eight = 0x91002000U;       /* add x0, x0, #8 */
	const std::uint32_t two_hundred = 0x91032000U; /* add x0, x0, #200 */
	std::memcpy(&other.bytes[edge_next_page], &eight, 4);
	std::memcpy(&other.bytes[edge_target], &two_hundred, 4);
	return other;
}

[[nodiscard]] Case edge_case(const PageEdge &edge, std::uint64_t seed,
	std::uint64_t bias)
{
	SplitMix rng{seed};
	auto c = make_case(rng, bias, edge.at, false, true, arena_base(bias));
	enterable(c);
	c.cpu.registers[0] = 0;
	c.cpu.registers[1] = 5;
	c.cpu.registers[30] = bias + stub_offset;
	return c;
}

/* Every kind of edge from a page that matches the framework to a page whose
 * mapped bytes differ from it. The code of the page it left is entered, the
 * edge ends the entry, and the engine runs the other page's bytes; an edge
 * that went on in the rewritten code of that page would add the old
 * amount. */
[[gnu::noinline]]
void edges_into_a_page_that_differs(const LibraryImage &image)
{
	const auto other = with_next_page_changed(image);
	const auto bias = Fixture::bias;
	unsigned long generation = 4400;
	for (const auto &edge : page_edges) {
		Fixture ref{generation++, other}, got{generation++, other};
		const auto c = edge_case(edge, 73, bias);
		aot::reset_stats();
		aot::set_enabled(false);
		const auto want = drive(ref.env, ref.arena, c, max_blocks);
		require(want.finished);
		require(want.cpu.registers[0] ==
			(std::string{edge.name} == "fall" ? 11U : 200U));
		aot::set_enabled(true);
		const auto have = drive(got.env, got.arena, c, aot_max_blocks);
		aot::set_enabled(false);
		if (!same(want, have, c))
			std::fprintf(stderr, "  edge %s\n", edge.name);
		require(same(want, have, c));
		const auto st = aot::stats();
		std::fprintf(stderr, "edge %s into a changed page: %llu entries\n",
			edge.name, static_cast<unsigned long long>(st.enters));
		require(st.enters >= 1);
		require(st.poisoned == 0);
	}
}

/* The same edges into a page that matches: it is compared when the edge
 * first reaches it, and the code on it is entered. */
[[gnu::noinline]]
void edges_into_a_page_that_matches(const LibraryImage &image)
{
	const auto bias = Fixture::bias;
	unsigned long generation = 4420;
	for (const auto &edge : page_edges) {
		Fixture ref{generation++, image}, got{generation++, image};
		const auto c = edge_case(edge, 79, bias);
		aot::reset_stats();
		aot::set_enabled(false);
		const auto want = drive(ref.env, ref.arena, c, max_blocks);
		require(want.finished);
		require(want.cpu.registers[0] ==
			(std::string{edge.name} == "fall" ? 7U : 100U));
		aot::set_enabled(true);
		const auto have = drive(got.env, got.arena, c, aot_max_blocks);
		aot::set_enabled(false);
		require(same(want, have, c));
		const auto st = aot::stats();
		require(st.enters >= 2);
		require(st.verifications >= 2);
		require(st.poisoned == 0);
	}
}

/* A page that changes after the page it is entered from was compared: the
 * edge into it is the engine's from then on; when the bytes are back, it is
 * compared again. */
[[gnu::noinline]]
void next_page_changes_after_its_neighbour_was_compared(
	const LibraryImage &image)
{
	const auto other = with_next_page_changed(image);
	const auto bias = Fixture::bias;
	for (const auto &edge : page_edges) {
		Fixture ref_old{4440, image}, ref_new{4441, other};
		Fixture got{4442, image};
		const auto c = edge_case(edge, 83, bias);
		aot::reset_stats();
		aot::set_enabled(false);
		const auto old_result = drive(ref_old.env, ref_old.arena, c,
			max_blocks);
		const auto new_result = drive(ref_new.env, ref_new.arena, c,
			max_blocks);
		require(old_result.finished && new_result.finished);
		aot::set_enabled(true);
		const auto first = drive(got.env, got.arena, c, aot_max_blocks);
		aot::set_enabled(false);
		require(same(old_result, first, c));
		auto entered = aot::stats().enters;
		require(entered >= 2);
		const auto change = [&](const LibraryImage &to) {
			for (const auto offset : {edge_next_page, edge_target}) {
				std::uint32_t word;
				std::memcpy(&word, &to.bytes[offset], 4);
				got.env.backing.put32(offset, word);
			}
			got.env.memory.invalidate_code(got.env.bias + edge_next_page);
		};
		change(other);
		aot::set_enabled(true);
		const auto second = drive(got.env, got.arena, c, aot_max_blocks);
		aot::set_enabled(false);
		if (!same(new_result, second, c))
			std::fprintf(stderr, "  edge %s after the change\n", edge.name);
		require(same(new_result, second, c));
		/* The page the edge starts on is still entered. */
		auto st = aot::stats();
		require(st.enters > entered);
		require(st.poisoned == 0);
		entered = st.enters;
		change(image);
		aot::set_enabled(true);
		const auto third = drive(got.env, got.arena, c, aot_max_blocks);
		aot::set_enabled(false);
		require(same(old_result, third, c));
		st = aot::stats();
		require(st.enters >= entered + 2);
		require(st.poisoned == 0);
	}
}

/* An indirect branch into another instance of the library (a second load
 * address in the same address space) lands on a page that matches, or on
 * one that differs: either way the result is the engine's, and the code of
 * a page that differs is not entered. */
[[gnu::noinline]]
void edge_into_another_instance(const LibraryImage &image)
{
	const auto other = with_next_page_changed(image);
	constexpr std::uint64_t bias_b = 0x7155'0000'0000ULL;
	const auto bias = Fixture::bias;
	for (const bool differs : {true, false}) {
		const unsigned long generation = differs ? 4460 : 4462;
		Environment ref{generation, bias, image};
		Environment got{generation + 1, bias, image};
		ref.add_library(bias_b, differs ? other : image);
		got.add_library(bias_b, differs ? other : image);
		Arena ref_arena{ref.generation, arena_base(bias)};
		Arena got_arena{got.generation, arena_base(bias)};
		/* The second instance is bound by running a function on its
		 * first code page. */
		const PageEdge warm{"b", 0x3000};
		auto w = edge_case(warm, 89, bias_b);
		aot::reset_stats();
		aot::set_enabled(true);
		(void)drive_on(got.executor, got.memory, got.generation, bias_b,
			got_arena, w, aot_max_blocks);
		aot::set_enabled(false);
		const auto warmed = aot::stats().enters;
		require(warmed >= 1);
		const PageEdge via_register{"reg", 0x3100};
		auto c = edge_case(via_register, 97, bias);
		c.cpu.registers[16] = bias_b + edge_target;
		aot::set_enabled(false);
		const auto want = drive_on(ref.executor, ref.memory, ref.generation,
			bias, ref_arena, c, max_blocks);
		require(want.finished);
		require(want.cpu.registers[0] == (differs ? 200U : 100U));
		aot::set_enabled(true);
		const auto have = drive_on(got.executor, got.memory, got.generation,
			bias, got_arena, c, aot_max_blocks);
		aot::set_enabled(false);
		require(same(want, have, c));
		const auto st = aot::stats();
		std::fprintf(stderr, "edge into another instance (page %s): %llu "
			"entries, %llu cross misses\n", differs ? "differs" : "matches",
			static_cast<unsigned long long>(st.enters),
			static_cast<unsigned long long>(st.cross_misses));
		require(st.cross_misses >= 1);
		require(st.poisoned == 0);
	}
}

/* A direct branch whose target lies below the first byte of the library, as
 * a method of boot-framework.oat has into boot.oat mapped before it
 * (tools/aot/fixture.py, build_oat): a second instance of the file is mapped
 * 0x10000 bytes lower, and each kind of direct branch of the first reaches
 * its callee there (the offset from the first instance is negative). The
 * rewritten code leaves by the absolute exit and the result is the engine's:
 * the callee adds 100, whether the lower instance was bound before or not. */
[[gnu::noinline]]
void direct_branches_below_the_library(const LibraryImage &image)
{
	constexpr std::uint64_t bias = 0x7193'0000'0000ULL;
	constexpr std::uint64_t lower_by = 0x10000;
	constexpr std::uint64_t lower_bias = bias - lower_by;
	constexpr PageEdge kinds[] = {
		{"b", 0x1100}, {"bl", 0x1120}, {"bcond", 0x1140}, {"cbz", 0x1160},
		{"tbz", 0x1180}};
	unsigned long generation = 5901;
	for (const bool bound : {false, true}) {
		for (const auto &kind : kinds) {
			Environment ref{generation, bias, image};
			Environment got{generation + 1, bias, image};
			generation += 2;
			ref.add_library(lower_bias, image);
			got.add_library(lower_bias, image);
			Arena ref_arena{ref.generation, arena_base(bias)};
			Arena got_arena{got.generation, arena_base(bias)};
			aot::reset_stats();
			if (bound) {
				/* The lower instance is bound first, by running its
				 * plain method. */
				const PageEdge plain{"plain", 0x1000};
				auto w = edge_case(plain, 101, lower_bias);
				aot::set_enabled(true);
				(void)drive_on(got.executor, got.memory, got.generation,
					lower_bias, got_arena, w, aot_max_blocks);
				aot::set_enabled(false);
				require(aot::stats().enters >= 1);
			}
			const auto before = aot::stats().enters;
			auto c = edge_case(kind, 103, bias);
			aot::set_enabled(false);
			const auto want = drive_on(ref.executor, ref.memory,
				ref.generation, bias, ref_arena, c, max_blocks);
			require(want.finished);
			require(want.cpu.registers[0] == 100);
			aot::set_enabled(true);
			const auto have = drive_on(got.executor, got.memory,
				got.generation, bias, got_arena, c, aot_max_blocks);
			aot::set_enabled(false);
			if (!same(want, have, c))
				std::fprintf(stderr, "  branch %s below the library "
					"(lower instance %s)\n", kind.name,
					bound ? "bound" : "not bound");
			require(same(want, have, c));
			const auto st = aot::stats();
			std::fprintf(stderr, "%s below the library (lower instance %s): "
				"x0 %llu, enters %llu\n", kind.name,
				bound ? "bound" : "not bound",
				static_cast<unsigned long long>(have.cpu.registers[0]),
				static_cast<unsigned long long>(st.enters - before));
			require(st.enters > before);
			require(st.poisoned == 0);
		}
	}
}

/* ---- scenarios on the fixture's shared-memory loops and CAS forms ------- */

/* tools/aot/fixture.py: the loops that add one to the cell at [x2] x0 times,
 * and the first of the instruction forms (cas_<size>_<ordering> is form
 * 177 + 2 * (4 * size + ordering); the tooling test pins it). */
constexpr std::uint64_t hammer_add = 0x2a00, hammer_cas = 0x2a40,
	hammer_llsc = 0x2a80;
constexpr std::uint64_t forms_at = 0x1100, form_size = 0x14;

[[nodiscard]] constexpr std::uint64_t cas_form(unsigned size, unsigned order)
{
	return forms_at + (177 + 2 * (4 * size + order)) * form_size;
}

/* The library mapped at `bias` with a stub of its own, in an address space
 * `generation` that other copies and an arena share. */
struct Copy final {
	Copy(unsigned long generation, std::uint64_t bias,
		const LibraryImage &image)
		: text{image.pages}, stub{1},
		  text_map{generation, bias, text, image.pages,
			PROT_READ | PROT_EXEC, 3, false},
		  stub_map{generation, bias + stub_offset, stub, 1,
			PROT_READ | PROT_EXEC, true}
	{
		text.put(0, image.bytes);
		stub.put32(0, system_call);
	}
	JitBacking text, stub;
	GenerationMapping text_map, stub_map;
};

/* Run `c` on `executor` through the AOT dispatcher until the guest returns to
 * its stub, whatever interrupts or other blocks come between. Memory is
 * whatever the other vCPUs and the arena hold: nothing is reset. */
std::size_t run_until_stub(GadgetExecutor &executor, StaticGuestMemory &memory,
	unsigned long generation, arm64::CpuState &cpu,
	std::size_t *interrupts = nullptr)
{
	std::size_t blocks = 0;
	for (;; ++blocks) {
		require(blocks < 20'000'000);
		arm64::discard_unmapped_code_synchronization();
		require(executor.begin_execution(generation, cpu.pc));
		std::size_t executed = 0;
		const auto event =
			executor.run_aot_block(cpu, memory, generation, executed);
		executor.end_execution();
		if (event == EngineEvent::system_call)
			break;
		require(event == EngineEvent::continued ||
			event == EngineEvent::interrupt);
		if (event == EngineEvent::interrupt && interrupts != nullptr)
			++*interrupts;
	}
	arm64::restore_native_fp_environment(cpu);
	return blocks;
}

/* A state at a loop of the fixture that rewritten code is entered under. */
[[nodiscard]] Case loop_case(std::uint64_t seed, std::uint64_t code_bias,
	std::uint64_t offset, std::uint64_t arena_at)
{
	SplitMix rng{seed};
	auto c = make_case(rng, code_bias, offset, false, true, arena_at);
	enterable(c);
	c.cpu.pc = code_bias + offset;
	c.cpu.registers[30] = code_bias + stub_offset;
	return c;
}

[[nodiscard]] std::uint64_t cell_value(const Arena &arena, std::size_t at)
{
	const auto bytes = arena.snapshot();
	std::uint64_t value;
	std::memcpy(&value, &bytes[at], 8);
	return value;
}

/* Rewritten code and the engine add to one cell from two vCPUs. The second
 * vCPU runs a copy of the library whose bytes differ from the framework's
 * (so the engine runs every instruction of it), or the same copy as the
 * first. A lost update would leave the count short. */
[[gnu::noinline]]
void atomics_are_shared_between_aot_and_engine(const LibraryImage &image)
{
	constexpr std::uint64_t bias_a = 0x7173'0000'0000ULL;
	constexpr std::uint64_t bias_b = 0x7174'0000'0000ULL;
	constexpr std::uint64_t iterations = 30000;
	constexpr unsigned long generation = 4510;
	auto changed = image;
	const std::uint32_t nop = 0xd503201fU;
	std::memcpy(&changed.bytes[0x2ff0], &nop, 4); /* filler on the loops' page */
	struct Pair {
		bool second_changed;
		std::uint64_t first, second;
		const char *name;
	};
	const Pair pairs[] = {
		{true, hammer_add, hammer_add, "ldadd / ldadd"},
		{true, hammer_add, hammer_cas, "ldadd / cas"},
		{true, hammer_cas, hammer_add, "cas / ldadd"},
		{true, hammer_cas, hammer_llsc, "cas / ldxr+stxr"},
		{true, hammer_llsc, hammer_add, "ldxr+stxr / ldadd"},
		{true, hammer_llsc, hammer_llsc, "ldxr+stxr / ldxr+stxr"},
		{false, hammer_add, hammer_add, "ldadd / ldadd (both rewritten)"},
		{false, hammer_add, hammer_cas, "ldadd / cas (both rewritten)"},
		{false, hammer_cas, hammer_cas, "cas / cas (both rewritten)"},
		{false, hammer_llsc, hammer_add, "ldxr+stxr / ldadd (both rewritten)"},
	};
	Copy first_copy{generation, bias_a, image};
	Copy second_copy{generation, bias_b, changed};
	Arena arena{generation, arena_base(bias_a)};
	StaticGuestMemory memory_a, memory_b;
	const auto cell = arena_base(bias_a) + 0x100;
	for (std::size_t i = 0; i < arena.total(); ++i)
		arena.set_kind(i, 2);
	aot::set_enabled(true);
	/* The changed copy is the engine's alone. */
	{
		GadgetExecutor executor_a, executor_b;
		aot::reset_stats();
		SplitMix fill{3};
		arena.fill(fill);
		auto c = loop_case(7, bias_b, hammer_add, arena_base(bias_a));
		c.cpu.registers[0] = 100;
		c.cpu.registers[2] = cell;
		run_until_stub(executor_b, memory_b, generation, c.cpu);
		require(aot::stats().enters == 0);
		auto d = loop_case(7, bias_a, hammer_add, arena_base(bias_a));
		d.cpu.registers[0] = 100;
		d.cpu.registers[2] = cell;
		run_until_stub(executor_a, memory_a, generation, d.cpu);
		require(aot::stats().enters > 0);
	}
	for (const auto &pair : pairs) {
		/* Fresh executors: what an executor learned about an address (the
		 * short entries it stops taking) is not a pair's to inherit; in the
		 * exact-count layout an entry retires less than it was charged and
		 * the tail entry of every budget is short. */
		GadgetExecutor executor_a, executor_b;
		SplitMix fill{11};
		arena.fill(fill);
		const auto initial = cell_value(arena, 0x100);
		aot::reset_stats();
		auto a = loop_case(21, bias_a, pair.first, arena_base(bias_a));
		auto b = loop_case(22, pair.second_changed ? bias_b : bias_a,
			pair.second, arena_base(bias_a));
		a.cpu.registers[0] = b.cpu.registers[0] = iterations;
		a.cpu.registers[2] = b.cpu.registers[2] = cell;
		std::atomic<bool> go{false};
		std::thread other{[&] {
			while (!go.load(std::memory_order_acquire))
				std::this_thread::yield();
			run_until_stub(executor_b, memory_b, generation, b.cpu);
		}};
		go.store(true, std::memory_order_release);
		run_until_stub(executor_a, memory_a, generation, a.cpu);
		other.join();
		const auto total = cell_value(arena, 0x100);
		const auto st = aot::stats();
		std::fprintf(stderr, "shared cell, %s: %llu of %llu, enters %llu "
			"steps %llu\n", pair.name,
			static_cast<unsigned long long>(total - initial),
			static_cast<unsigned long long>(2 * iterations),
			static_cast<unsigned long long>(st.enters),
			static_cast<unsigned long long>(st.steps));
		require(total - initial == 2 * iterations);
		require(a.cpu.registers[0] == 0 && b.cpu.registers[0] == 0);
		require(st.enters > 0);
		require(st.poisoned <= 1);
		/* The exclusive pair is the engine's even in rewritten code. */
		if (pair.first == hammer_llsc ||
		    (!pair.second_changed && pair.second == hammer_llsc))
			require(st.steps > 0);
	}
	aot::set_enabled(false);
}

/* CAS with the fixture's forms at every width and ordering: the compare
 * value matching and not, on a writable page, a read-only one, an unmapped
 * one and a misaligned address. Rewritten code and the engine end in the
 * same registers, memory and fault; the engine's own results are checked
 * against the architecture's too. */
[[gnu::noinline]]
void cas_outcomes_and_permissions(const LibraryImage &image)
{
	constexpr unsigned long generation_ref = 4530, generation_aot = 4531;
	const auto bias = Fixture::bias;
	Fixture ref{generation_ref, image}, got{generation_aot, image};
	const std::uint64_t masks[4] = {0xff, 0xffff, 0xffff'ffffULL, ~0ULL};
	std::size_t compared = 0, faulted = 0;
	enum Where { writable, read_only, unmapped, misaligned, count };
	aot::reset_stats();
	for (unsigned size = 0; size < 4; ++size)
		for (unsigned order = 0; order < 4; ++order)
			for (int where = 0; where < count; ++where)
				for (const bool match : {true, false}) {
					if (where == misaligned && size == 0)
						continue;
					SplitMix rng{1000 + 64 * size + 16 * order +
						4 * where + (match ? 1 : 0)};
					auto c = loop_case(rng.next(), bias,
						cas_form(size, order), arena_base(bias));
					c.kinds.assign(arena_pages + stack_pages, 2);
					c.kinds[0] = where == read_only ? 1 :
						where == unmapped ? 0 : 2;
					const auto cell = arena_base(bias) +
						(where == misaligned ? 0x103 : 0x100);
					/* What the arena holds at the cell. */
					SplitMix seeded{c.seed};
					ref.arena.fill(seeded);
					std::uint64_t old;
					const auto bytes = ref.arena.snapshot();
					std::memcpy(&old, &bytes[cell - arena_base(bias)], 8);
					old &= masks[size];
					c.cpu.registers[1] = cell;
					c.cpu.registers[2] = 0x1122'3344'5566'7788ULL;
					c.cpu.registers[3] = match ? old : (old ^ 1);
					aot::set_enabled(false);
					const auto want = drive(ref.env, ref.arena, c,
						max_blocks);
					require(want.done());
					aot::set_enabled(true);
					const auto have = drive(got.env, got.arena, c,
						aot_max_blocks);
					aot::set_enabled(false);
					require(same(want, have, c));
					++compared;
					/* The architecture's answer, from the engine's run. */
					const auto fresh = [&] {
						std::uint64_t value;
						std::memcpy(&value,
							&want.memory[cell - arena_base(bias)], 8);
						return value & masks[size];
					}();
					if (where == writable) {
						require(want.faults.empty());
						require(want.cpu.registers[3] == old);
						require(fresh == (match ?
							(c.cpu.registers[2] & masks[size]) : old));
					} else if (where == read_only || where == unmapped) {
						/* A write fault, whether or not the compare
						 * would have matched. */
						require(want.faults.size() >= 1);
						require(want.faults.front().address == cell);
						++faulted;
					}
				}
	const auto st = aot::stats();
	std::fprintf(stderr, "cas matrix: %zu cases compared, %zu faulting; "
		"enters %llu\n", compared, faulted,
		static_cast<unsigned long long>(st.enters));
	require(compared == 4 * 4 * 2 * count - 4 * 2);
	require(faulted == 4 * 4 * 2 * 2);
	require(st.enters > compared / 2);
	require(st.poisoned == 0);
}

/* A naturally misaligned CAS inside one 16-byte granule: the engine faults
 * it under the baseline profile (no FEAT_LSE2) and performs it under the
 * profile the runtime probes. Rewritten code leaves such an
 * access to the engine, so it ends as the engine does under either. */
[[gnu::noinline]]
void misaligned_cas_follows_the_profile(const LibraryImage &image)
{
	const auto bias = Fixture::bias;
	const auto probed = ::uml::platform::apple::probed_host_capabilities();
	for (const bool lse2 : {false, true}) {
		if (lse2 && !probed.has(::uml::cpu::ArmFeature::lse2))
			continue;
		const ::uml::cpu::HostCapabilitiesScope scope{
			lse2 ? probed : ::uml::cpu::baseline_host_capabilities()};
		Fixture ref{4570 + (lse2 ? 2UL : 0UL), image};
		Fixture got{4571 + (lse2 ? 2UL : 0UL), image};
		SplitMix rng{91};
		auto c = loop_case(rng.next(), bias, cas_form(3, 0),
			arena_base(bias));
		c.cpu.registers[1] = arena_base(bias) + 0x103;
		c.cpu.registers[2] = 0x1122'3344'5566'7788ULL;
		c.cpu.registers[3] = 0;
		aot::set_enabled(false);
		const auto want = drive(ref.env, ref.arena, c, max_blocks);
		aot::set_enabled(true);
		const auto have = drive(got.env, got.arena, c, aot_max_blocks);
		aot::set_enabled(false);
		require(want.done());
		require(same(want, have, c));
		std::fprintf(stderr, "misaligned cas, lse2 %d: %zu faults, "
			"alignment %d\n", lse2 ? 1 : 0, want.faults.size(),
			want.faults.empty() ? 0 : want.faults.front().alignment ? 1 : 0);
		if (lse2) {
			require(want.faults.empty());
		} else {
			require(!want.faults.empty() && want.faults.front().alignment);
		}
	}
}

/* More instances than the binder holds: the library mapped at six more
 * biases than the binder has places (2054 in the product's 2048) is run copy by
 * copy, twice. The ones it dropped are bound again, and every run ends as
 * the engine's does. */
[[gnu::noinline]]
void more_instances_than_the_binder_holds(const LibraryImage &image)
{
	const std::size_t capacity = aot::instance_capacity();
	const std::size_t copies = capacity + 6;
	constexpr unsigned long generation = 4540;
	/* Every copy stays below 2^47, the guest address limit (a copy beyond
	 * it has a key the engine cannot cache): 16,390 copies at the largest
	 * capacity reach 0x5006'0000'0000. */
	const auto first_bias = 0x1000'0000'0000ULL;
	const auto bias_of = [&](std::size_t index) {
		return first_bias + index * 0x1'0000'0000ULL;
	};
	require(bias_of(copies) < (1ULL << 47));
	std::vector<std::unique_ptr<Copy>> maps;
	for (std::size_t i = 0; i < copies; ++i)
		maps.push_back(std::make_unique<Copy>(generation, bias_of(i), image));
	Arena arena{generation, arena_base(bias_of(0))};
	StaticGuestMemory memory;
	GadgetExecutor executor;
	std::vector<Case> cases;
	for (std::size_t i = 0; i < copies; ++i) {
		/* The first loop of the fixture, counting to x3. */
		auto c = loop_case(500 + i, bias_of(i), 0x1000, arena_base(bias_of(0)));
		c.cpu.registers[3] = 50 + i;
		cases.push_back(c);
	}
	std::vector<Result> want;
	aot::set_enabled(false);
	for (std::size_t i = 0; i < copies; ++i)
		want.push_back(drive_on(executor, memory, generation, bias_of(i),
			arena, cases[i], max_blocks));
	aot::reset_stats();
	aot::set_enabled(true);
	for (int round = 0; round < 2; ++round)
		for (std::size_t i = 0; i < copies; ++i) {
			const auto have = drive_on(executor, memory, generation,
				bias_of(i), arena, cases[i], aot_max_blocks);
			require(want[i].done());
			require(same(want[i], have, cases[i]));
		}
	aot::set_enabled(false);
	const auto st = aot::stats();
	std::fprintf(stderr, "%zu instances: binds %llu enters %llu; made %llu, "
		"took a live one's place %llu, made again %llu\n", copies,
		static_cast<unsigned long long>(st.binds),
		static_cast<unsigned long long>(st.enters),
		static_cast<unsigned long long>(st.instances_created),
		static_cast<unsigned long long>(st.instance_evictions),
		static_cast<unsigned long long>(st.instance_rebinds));
	/* Each copy is bound the first time, and the ones the slots dropped
	 * again in the second round. */
	require(st.binds >= copies + (copies - capacity));
	require(st.enters >= 2 * copies);
	require(st.poisoned == 0);
	/* The slots are first in, first out, and the copies visited in turn over
	 * fewer slots miss every time after the first fill: one instance made
	 * per run, all but the first `capacity` in the place of a live one, and
	 * every one of the second round made again. */
	require(st.instances_created >= 2 * copies);
	require(st.instance_evictions >= 2 * copies - capacity);
	require(st.instance_rebinds >= copies);
}

/* The page states of a binder's instances and the page slots it filled are
 * counted while it lives and leave the count with it: executors made and
 * dropped one after another do not add up. */
[[gnu::noinline]]
void a_dropped_binder_gives_back_its_page_states(const LibraryImage &image)
{
	const auto bias = Fixture::bias;
	const auto before = aot::stats().instance_page_bytes;
	const auto slots_before = aot::stats().page_slots_filled;
	{
		Fixture got{4570, image};
		auto c = loop_case(91, bias, 0x1000, arena_base(bias));
		c.cpu.registers[3] = 50;
		aot::set_enabled(true);
		const auto have = drive(got.env, got.arena, c, aot_max_blocks);
		aot::set_enabled(false);
		require(have.finished);
		require(aot::stats().instance_page_bytes > before);
		require(aot::stats().page_slots_filled > slots_before);
	}
	require(aot::stats().instance_page_bytes == before);
	require(aot::stats().page_slots_filled == slots_before);
}

/* A shootdown (a mapping change by another thread) arrives while rewritten
 * code runs a long loop: the vCPU leaves at its next return to the
 * dispatcher, the writer is never kept waiting, and the loop ends in the
 * state the engine reaches. */
[[gnu::noinline]]
void a_shootdown_reaches_a_native_loop(const LibraryImage &image)
{
	constexpr unsigned long generation = 4560;
	constexpr std::uint64_t iterations = 20'000'000;
	const auto bias = Fixture::bias;
	Fixture got{generation, image};
	JitBacking spare{1};
	std::atomic<bool> done{false};
	std::atomic<std::size_t> shootdowns{0};
	/* A loop of three instructions per count: x0 counts up to x3. */
	auto c = loop_case(77, bias, 0x1000, arena_base(bias));
	c.cpu.registers[3] = iterations;
	for (std::size_t i = 0; i < got.arena.total(); ++i)
		got.arena.set_kind(i, 2);
	aot::reset_stats();
	aot::set_enabled(true);
	std::thread writer{[&] {
		/* Mappings elsewhere in the address space, made and removed, each
		 * of which closes the execution gate and interrupts the vCPU. */
		while (!done.load(std::memory_order_acquire) &&
		       shootdowns.load() < 20000) {
			GenerationMapping map{generation,
				got.env.bias + 0x3000'0000ULL, spare, 1,
				PROT_READ | PROT_WRITE, false};
			shootdowns.fetch_add(1);
			std::this_thread::yield();
		}
	}};
	std::size_t interrupts = 0;
	const auto began = std::chrono::steady_clock::now();
	std::size_t blocks = run_until_stub(got.env.executor, got.env.memory,
		generation, c.cpu, &interrupts);
	done.store(true, std::memory_order_release);
	writer.join();
	aot::set_enabled(false);
	const auto seconds = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - began).count();
	const auto st = aot::stats();
	std::fprintf(stderr, "shootdowns during a native loop: %zu mapping "
		"changes, %zu interrupts, %zu blocks, %.2f s, enters %llu\n",
		shootdowns.load(), interrupts, blocks, seconds,
		static_cast<unsigned long long>(st.enters));
	require(c.cpu.registers[0] == iterations);
	require(st.enters > 0);
	require(shootdowns.load() > 10);
	require(interrupts > 0);
	require(seconds < 120);
	require(st.poisoned == 0);
}

/* The 64-bit page digests find a library; they authorize nothing. Two words
 * of a page are changed (the first to something the engine will run, the
 * second to make the digest come out the same), and the page is not the one
 * the framework was built from. */
[[nodiscard]] std::uint64_t digest_of(const std::byte *page)
{
	std::uint32_t low = 0, high = 0x9e3779b9U;
	for (std::size_t offset = 0; offset < page_bytes; offset += 8) {
		std::uint64_t word;
		std::memcpy(&word, page + offset, 8);
		low = __crc32cd(low, word);
		high = __crc32cd(high, word);
	}
	return (std::uint64_t{high} << 32) | low;
}

/* The CRC of a page with zero initial state, which is linear in the page. */
[[nodiscard]] std::uint32_t crc_of(const std::byte *page)
{
	std::uint32_t crc = 0;
	for (std::size_t offset = 0; offset < page_bytes; offset += 8) {
		std::uint64_t word;
		std::memcpy(&word, page + offset, 8);
		crc = __crc32cd(crc, word);
	}
	return crc;
}

/* Set the word at `fixup` of `work` (one page) so that the page's CRC, which
 * is linear in the page, is `wanted`: the word's effect on the CRC is a
 * bijection of 32-bit values. */
void cancel_crc_with_word(std::array<std::byte, page_bytes> &work,
	std::uint32_t wanted, std::size_t fixup)
{
	std::array<std::uint32_t, 32> image_of_bit{};
	const auto base_crc = crc_of(work.data());
	for (unsigned bit = 0; bit < 32; ++bit) {
		std::array<std::byte, page_bytes> flipped = work;
		std::uint32_t value;
		std::memcpy(&value, &flipped[fixup], 4);
		value ^= 1U << bit;
		std::memcpy(&flipped[fixup], &value, 4);
		image_of_bit[bit] = crc_of(flipped.data()) ^ base_crc;
	}
	/* Solve: choose bits so that the xor of their images is base ^ wanted. */
	std::uint32_t target = base_crc ^ wanted;
	std::array<std::uint32_t, 32> rows = image_of_bit;
	std::array<std::uint32_t, 32> combo{};
	for (unsigned i = 0; i < 32; ++i)
		combo[i] = 1U << i;
	std::uint32_t chosen = 0;
	for (unsigned bit = 0; bit < 32; ++bit) {
		/* Find a row with this bit as pivot. */
		unsigned pivot = bit;
		while (pivot < 32 && ((rows[pivot] >> bit) & 1U) == 0)
			++pivot;
		require(pivot < 32);
		std::swap(rows[bit], rows[pivot]);
		std::swap(combo[bit], combo[pivot]);
		for (unsigned other_row = 0; other_row < 32; ++other_row)
			if (other_row != bit && ((rows[other_row] >> bit) & 1U) != 0) {
				rows[other_row] ^= rows[bit];
				combo[other_row] ^= combo[bit];
			}
	}
	for (unsigned bit = 0; bit < 32; ++bit)
		if ((target >> bit) & 1U) {
			/* rows[bit] is the unit vector for bit. */
			chosen ^= combo[bit];
		}
	std::uint32_t value;
	std::memcpy(&value, &work[fixup], 4);
	value ^= chosen;
	std::memcpy(&work[fixup], &value, 4);
}

/* `image` with the first word of the code at `start` changed (to
 * `mov x0, #120`) and the last word of its page chosen so that the page's
 * digest is the one the framework was built from: a page that is not the
 * stored one and that no digest tells from it. */
[[nodiscard]] LibraryImage with_equal_digest(const LibraryImage &image,
	std::uint64_t start)
{
	LibraryImage other = image;
	const auto page = start / page_bytes * page_bytes;
	const auto fixup = page + page_bytes - 4;
	require(start + 4 < fixup);
	std::array<std::byte, page_bytes> original;
	std::memcpy(original.data(), &image.bytes[page], page_bytes);
	/* mov x0, #120 in place of the function's first word. */
	const std::uint32_t mov = 0xd2800f00U;
	std::memcpy(&other.bytes[start], &mov, 4);
	std::array<std::byte, page_bytes> work;
	std::memcpy(work.data(), &other.bytes[page], page_bytes);
	cancel_crc_with_word(work, crc_of(original.data()), page_bytes - 4);
	std::memcpy(&other.bytes[fixup], &work[page_bytes - 4], 4);
	require(digest_of(work.data()) == digest_of(original.data()));
	require(std::memcmp(work.data(), original.data(), page_bytes) != 0);

	return other;
}

/* `image` with bit `bit` of the byte at `byte` of the page at `page` flipped
 * and a second word of the page, 32 bytes from the first within its 64,
 * chosen so that the page's digest is the one the framework was built from:
 * a page that differs from the stored one by that bit and the few of the
 * word that cancels it, which no digest tells from it. Where the bit is
 * neither in the first nor in the last 48 bytes, the page keeps its first and
 * last 16 bytes. */
[[nodiscard]] LibraryImage with_a_bit_changed_and_equal_digest(
	const LibraryImage &image, std::uint64_t page, std::uint64_t byte,
	unsigned bit)
{
	LibraryImage other = image;
	std::array<std::byte, page_bytes> original;
	std::memcpy(original.data(), &image.bytes[page], page_bytes);
	std::array<std::byte, page_bytes> work = original;
	work[byte] = static_cast<std::byte>(
		std::to_integer<unsigned>(work[byte]) ^ (1U << bit));
	const std::size_t fixup = (byte & ~std::uint64_t{3}) ^ 0x20U;
	require(fixup + 4 <= page_bytes);
	cancel_crc_with_word(work, crc_of(original.data()), fixup);
	require(digest_of(work.data()) == digest_of(original.data()));
	require(std::memcmp(work.data(), original.data(), page_bytes) != 0);
	std::memcpy(&other.bytes[page], work.data(), page_bytes);
	return other;
}

[[gnu::noinline]]
void equal_digest_is_not_equal_code(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	const auto start = functions[2].start;
	const auto other = with_equal_digest(image, start);
	SplitMix rng{61};
	const auto bias = Fixture::bias;
	Fixture ref{4351, other}, got{4352, other};
	auto c = make_case(rng, bias, start, false, true, arena_base(bias));
	enterable(c);
	aot::reset_stats();
	aot::set_enabled(false);
	const auto want = drive(ref.env, ref.arena, c, max_blocks);
	aot::set_enabled(true);
	const auto have = drive(got.env, got.arena, c, aot_max_blocks);
	aot::set_enabled(false);
	require(want.done());
	require(same(want, have, c));
	const auto st = aot::stats();
	std::fprintf(stderr, "equal digest, different code: %llu entries, "
		"poisoned %llu\n", static_cast<unsigned long long>(st.enters),
		static_cast<unsigned long long>(st.poisoned));
	require(st.poisoned == 1);
	require(st.enters == 0);
}

/* One changed bit anywhere in a compared page, however far from its first
 * and last bytes, is found by the hash of the page. The page's 64-bit digest
 * is made to come out the one the framework was built from (a word 32 bytes
 * from the bit cancels the bit's effect on it), so the digest finds the
 * library and only the stored hash can tell the page from the stored one; the
 * change is published and the history then drops what it held, so the binder
 * hashes the page again. The instance is the engine's from then on and the
 * guest's result is the engine's.
 *
 * The positions: the first and last byte, the byte before the middle, the
 * middle, the bytes either side of every 512-byte boundary, both bits at the
 * ends of a byte, and a seeded spread. */
[[gnu::noinline]]
void a_changed_bit_anywhere_in_a_page_is_refused(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	const auto fn = functions[5];
	const auto page = fn.start / page_bytes * page_bytes;
	require(page + page_bytes <= image.bytes.size());
	std::vector<std::pair<std::uint64_t, unsigned>> positions{
		{0, 0}, {0, 7}, {page_bytes - 1, 0}, {page_bytes - 1, 7},
		{page_bytes / 2 - 1, 3}, {page_bytes / 2, 3}, {page_bytes / 2, 0}};
	for (std::uint64_t edge = 512; edge < page_bytes; edge += 512) {
		positions.emplace_back(edge - 1, 7);
		positions.emplace_back(edge, 0);
	}
	SplitMix pick{73};
	for (int i = 0; i < 24; ++i)
		positions.emplace_back(pick.below(page_bytes),
			static_cast<unsigned>(pick.below(8)));
	const auto bias = Fixture::bias;
	SplitMix rng{31};
	const auto c = make_case(rng, bias, fn.start, false, true,
		arena_base(bias));
	std::size_t refused = 0;
	for (const auto &[byte, bit] : positions) {
		const auto changed =
			with_a_bit_changed_and_equal_digest(image, page, byte, bit);
		/* Where the bit is in the middle, the first and the last 16 bytes
		 * of the page are the stored ones. */
		if (byte >= 48 && byte < page_bytes - 48) {
			require(std::memcmp(&image.bytes[page], &changed.bytes[page],
				16) == 0);
			require(std::memcmp(&image.bytes[page + page_bytes - 16],
				&changed.bytes[page + page_bytes - 16], 16) == 0);
		}
		Fixture ref{4601, changed}, got{4602, image};
		aot::set_enabled(false);
		const auto want = drive(ref.env, ref.arena, c, max_blocks);
		aot::reset_stats();
		aot::set_enabled(true);
		const auto first = drive(got.env, got.arena, c, aot_max_blocks);
		aot::set_enabled(false);
		require(first.done());
		const auto entered = aot::stats().enters;
		require(entered > 0);
		require(aot::stats().poisoned == 0);
		/* The page becomes the other one, and its publication is
		 * followed by more than the history holds. */
		for (std::uint64_t word = page; word < page + page_bytes; word += 4) {
			std::uint32_t was, now;
			std::memcpy(&was, &image.bytes[word], 4);
			std::memcpy(&now, &changed.bytes[word], 4);
			if (was != now)
				got.env.backing.put32(word, now);
		}
		got.env.memory.invalidate_code(got.env.bias + page);
		for (std::size_t i = 0; i < 40; ++i)
			got.env.memory.invalidate_code(
				bias + 0x2000'0000ULL + (400 + i) * 0x10000);
		aot::set_enabled(true);
		const auto have = drive(got.env, got.arena, c, aot_max_blocks);
		aot::set_enabled(false);
		if (want.done())
			require(same(want, have, c));
		const auto st = aot::stats();
		require(st.poisoned == 1);
		require(st.enters == entered);
		++refused;
	}
	std::fprintf(stderr, "changed bit refused: %zu positions\n", refused);
	require(refused == positions.size());
}

/* A text page changes after it was compared, and more code changes are
 * published than the history keeps: the binder cannot tell which pages the
 * dropped ones named, so it compares them again. The change is one that a
 * page digest does not show (the page's digest is the framework's), so the
 * exact comparison is all that stands between the stored code and the
 * guest's. */
[[gnu::noinline]]
void code_change_history_overflows(const LibraryImage &image,
	const std::vector<Function> &functions)
{
	SplitMix rng{29};
	const auto bias = Fixture::bias;
	const auto fn = functions[5];
	auto c = make_case(rng, bias, fn.start, false, true, arena_base(bias));
	std::uint64_t offset = 0;
	for (const auto &f : functions)
		if ((f.start >> 12) != (fn.start >> 12) &&
		    f.start % page_bytes < page_bytes - 64) {
			offset = f.start;
			break;
		}
	require(offset != 0);
	const auto collided = with_equal_digest(image, offset);
	const auto page = offset / page_bytes * page_bytes;
	const auto fixup = page + page_bytes - 4;
	std::uint32_t patched, fixed, original, original_fixup;
	std::memcpy(&patched, &collided.bytes[offset], 4);
	std::memcpy(&fixed, &collided.bytes[fixup], 4);
	std::memcpy(&original, &image.bytes[offset], 4);
	std::memcpy(&original_fixup, &image.bytes[fixup], 4);
	const auto d = make_case(rng, bias, offset, false, true, arena_base(bias));
	Fixture ref_changed{4551, collided}, ref_original{4553, image};
	aot::set_enabled(false);
	const auto want_c = drive(ref_original.env, ref_original.arena, c,
		max_blocks);
	const auto want_changed = drive(ref_changed.env, ref_changed.arena, d,
		max_blocks);
	const auto want_original = drive(ref_original.env, ref_original.arena, d,
		max_blocks);
	require(want_changed.cpu.registers[0] != want_original.cpu.registers[0]);

	Fixture got{4552, image};
	const auto publish_elsewhere = [&](std::size_t count, std::size_t from) {
		for (std::size_t i = 0; i < count; ++i)
			got.env.memory.invalidate_code(
				bias + 0x2000'0000ULL + (from + i) * 0x10000);
	};
	const auto run = [&](const Case &which) {
		aot::set_enabled(true);
		auto result = drive(got.env, got.arena, which, aot_max_blocks);
		aot::set_enabled(false);
		return result;
	};
	aot::reset_stats();
	require(same(want_c, run(c), c));
	require(same(want_original, run(d), d));
	auto st = aot::stats();
	require(st.enters > 0);
	/* A few unrelated publications: what was compared stays compared. */
	publish_elsewhere(3, 0);
	const auto hashed = st.pages_hashed;
	require(same(want_c, run(c), c));
	require(same(want_original, run(d), d));
	st = aot::stats();
	require(st.pages_hashed == hashed);
	/* The page of d becomes the digest-equal other one, and its
	 * publication is followed by more than the history holds. */
	got.env.backing.put32(offset, patched);
	got.env.backing.put32(fixup, fixed);
	got.env.memory.invalidate_code(got.env.bias + offset);
	publish_elsewhere(40, 100);
	const auto before = aot::stats().enters;
	require(same(want_changed, run(d), d));
	st = aot::stats();
	require(st.enters == before);
	/* The exact comparison found the page different: the instance is
	 * the engine's from then on, whatever the guest does next. */
	require(st.poisoned == 1);
	require(st.pages_hashed > hashed);
	require(same(want_c, run(c), c));
	got.env.backing.put32(offset, original);
	got.env.backing.put32(fixup, original_fixup);
	got.env.memory.invalidate_code(got.env.bias + offset);
	publish_elsewhere(40, 200);
	require(same(want_original, run(d), d));
	st = aot::stats();
	require(st.enters == before);
}


/* ---- memory copies and sets ---------------------------------------------- */

/* tools/aot/fixture.py (MOPS_AT, MOPS_ROWS): the three stages of CPYF, CPY and
 * SET from registers 0, 2, 1 (destination, count, source or fill), the same
 * from 17, 16, 15, and a SET whose fill is the zero register. */
constexpr std::uint64_t mops_at = 0x2b00, mops_step = 0x20;
enum MopsRow : std::size_t {
	row_cpyf, row_cpy, row_set, row_cpyf_high, row_cpy_high, row_set_high,
	row_set_zero, mops_rows,
};

[[nodiscard]] bool mops_is_set(std::size_t row)
{
	return row == row_set || row == row_set_high || row == row_set_zero;
}

/* tools/aot/fixture.py (SIMD_WARM_AT): byte load from x2, then the structure
 * access from x1. */
constexpr std::uint64_t simd_warm_at = 0x2f00, simd_warm_step = 0x10;
constexpr std::size_t simd_warm_rows = 4;

struct MopsRun final {
	std::size_t compared{}, faulted{};
};

/* Where in the row a start is made (0 the prologue, 8 the epilogue) and, if
 * set, the flags it starts with. */
struct MopsStart final {
	std::uint64_t at{0};
	std::optional<std::uint32_t> nzcv{};
};

/* One start of a row: destination, source and count, and the kind of each
 * page of the arena (0 unmapped, 1 read-only, 2 writable). The engine runs
 * it alone and then with rewritten code; the end state and the state at every
 * fault (registers, flags, memory) must be the same. */
void mops_row(Fixture &ref, Fixture &got, SplitMix &rng, MopsRun &run,
	std::size_t row, std::uint64_t destination, std::uint64_t source,
	std::uint64_t count, const std::vector<std::uint8_t> &kinds,
	std::uint64_t fill = 0x1234'5678'90ab'cdefULL, MopsStart start = {})
{
	const auto bias = Fixture::bias;
	auto c = make_case_from_seed(rng.next(), bias,
		mops_at + row * mops_step + start.at, true, true, arena_base(bias));
	enterable(c);
	if (start.nzcv)
		c.cpu.nzcv = *start.nzcv;
	c.kinds = kinds;
	const bool high = row == row_cpyf_high || row == row_cpy_high ||
		row == row_set_high;
	const unsigned d = high ? 17 : 0, n = high ? 16 : 2, s = high ? 15 : 1;
	c.cpu.registers[d] = destination;
	c.cpu.registers[n] = count;
	if (row != row_set_zero)
		c.cpu.registers[s] = mops_is_set(row) ? fill : source;
	aot::set_enabled(false);
	const auto want = drive(ref.env, ref.arena, c, max_blocks);
	aot::set_enabled(true);
	const auto have = drive(got.env, got.arena, c, aot_max_blocks);
	aot::set_enabled(false);
	if (!want.done() || !same(want, have, c)) {
		std::fprintf(stderr, "  memory copy/set row %zu at +%llu flags %x: "
			"destination %llx source %llx count %llx\n", row,
			static_cast<unsigned long long>(start.at),
			start.nzcv ? *start.nzcv : 0xffU,
			static_cast<unsigned long long>(destination),
			static_cast<unsigned long long>(source),
			static_cast<unsigned long long>(count));
		require(false);
	}
	++run.compared;
	run.faulted += want.faults.empty() ? 0 : 1;
}

/* The engine alone and then rewritten code, both from this case. */
void compare_case(Fixture &ref, Fixture &got, const Case &c, const char *what)
{
	aot::set_enabled(false);
	const auto want = drive(ref.env, ref.arena, c, max_blocks);
	aot::set_enabled(true);
	const auto have = drive(got.env, got.arena, c, aot_max_blocks);
	aot::set_enabled(false);
	if (!want.done() || !same(want, have, c)) {
		std::fprintf(stderr, "  %s\n", what);
		require(false);
	}
}

[[gnu::noinline]]
void memory_copy_and_set_match_the_engine(const LibraryImage &image)
{
	const auto bias = Fixture::bias;
	Fixture ref{4601, image}, got{4602, image};
	SplitMix rng{31};
	MopsRun run;
	aot::reset_stats();
	const auto base = arena_base(bias);
	const auto page_at = [&](std::size_t page) {
		return base + page * page_bytes;
	};
	const std::vector<std::uint8_t> plain(arena_pages + stack_pages, 2);
	const auto with = [&](std::initializer_list<
		std::pair<std::size_t, std::uint8_t>> changes) {
		auto kinds = plain;
		for (const auto &[page, kind] : changes)
			kinds[page] = kind;
		return kinds;
	};
	const std::uint64_t counts[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17,
		31, 32, 33, 63, 64, 65, 100, 128, 255, 1000, 4095, 4096, 4097,
		5000, 9000, 1ULL << 20};
	const std::uint64_t tags[] = {0, 0x5aULL << 56, 0xa3ULL << 56};
	for (std::size_t row = 0; row < mops_rows; ++row) {
		const bool set = mops_is_set(row);
		const bool high = row == row_cpyf_high || row == row_cpy_high ||
			row == row_set_high;
		for (const auto n : counts) {
			if (high && n > 129 && n != 4097)
				continue;
			for (const auto tag : tags) {
				const auto tagged = [&](std::uint64_t address) {
					return address | tag;
				};
				/* Inside one page each, at several alignments. */
				for (const auto &[so, dof] : std::vector<
				     std::pair<std::uint64_t, std::uint64_t>>{
				     {0x100, 0x300}, {0x103, 0x7fd}, {0, 0},
				     {0x4f, 0x60}}) {
					if (n > 4096 && so != 0x100)
						continue;
					mops_row(ref, got, rng, run, row,
						tagged(page_at(4) + dof), tagged(page_at(2) + so),
						n, plain);
				}
				if (n == 0)
					continue;
				/* The destination, the source or both end in a page that
				 * is unmapped or read-only: the fault comes at the first,
				 * a middle and the last byte. */
				std::vector<std::uint64_t> leads{0, n / 2, n - 1};
				if (n == 1)
					leads = {0};
				if (n > 4096)
					leads = {0, 1, 4095};
				for (const auto lead : leads) {
					for (const std::uint8_t kind : {0, 1, 2}) {
						/* The destination: `lead` bytes before the
						 * boundary of page 5, the rest in page 5. */
						mops_row(ref, got, rng, run, row,
							tagged(page_at(5) - lead), tagged(page_at(2) + 0x40),
							n, with({{5, kind}}));
						if (set || kind == 1)
							continue;
						/* The source likewise. */
						mops_row(ref, got, rng, run, row,
							tagged(page_at(8) + 0x30), tagged(page_at(5) - lead),
							n, with({{5, kind}}));
						/* Both cross, at different places. */
						for (const auto other : leads)
							mops_row(ref, got, rng, run, row,
								tagged(page_at(8) - other),
								tagged(page_at(5) - lead), n,
								with({{5, kind}, {8, 0}}));
					}
					/* Past the first boundary the next page is unmapped
					 * too: progress, then the fault. */
					if (n > 4096)
						mops_row(ref, got, rng, run, row,
							tagged(page_at(5) - lead),
							tagged(page_at(2) + 0x40), n,
							with({{6, 0}}));
				}
				if (set || n > 200)
					continue;
				/* Source and destination overlap, the destination above or
				 * below, in one page and across a boundary. */
				for (const std::uint64_t delta : {0, 1, 2, 7, 8, 15, 16, 17,
				     63, 64, 65}) {
					mops_row(ref, got, rng, run, row,
						tagged(page_at(3) + 0x400 + delta),
						tagged(page_at(3) + 0x400), n, plain);
					mops_row(ref, got, rng, run, row,
						tagged(page_at(3) + 0x400),
						tagged(page_at(3) + 0x400 + delta), n, plain);
					mops_row(ref, got, rng, run, row,
						tagged(page_at(4) - n / 2 + delta),
						tagged(page_at(4) - n / 2), n, plain);
					mops_row(ref, got, rng, run, row,
						tagged(page_at(4) - n / 2),
						tagged(page_at(4) - n / 2 + delta), n,
						with({{4, 0}}));
				}
			}
		}
	}
	/* A set whose fill value has other bits than its low byte. */
	for (const std::uint64_t fill : {0x00ULL, 0xffULL, ~0ULL, 0x100ULL,
	     0xdead'beef'0000'0042ULL})
		for (const std::uint64_t n : {1, 9, 33, 64, 65})
			mops_row(ref, got, rng, run, row_set, page_at(4) + 0x21,
				0, n, plain, fill);
	const auto st = aot::stats();
	std::fprintf(stderr, "memory copy and set: %zu starts compared, %zu "
		"faulted; enters %llu steps %llu slow %llu\n", run.compared,
		run.faulted, static_cast<unsigned long long>(st.enters),
		static_cast<unsigned long long>(st.steps),
		static_cast<unsigned long long>(st.mops_slow));
	require(run.compared > 5000);
	require(run.faulted > 500);
	require(st.enters > run.compared / 2);
	require(st.mops_slow > 0);

	/* The thunk's own path: a copy and a set of up to 64 bytes inside one
	 * page each, with the pages in the TLB, leave neither a step nor a call
	 * to the host routine; a larger one calls it once and does not step. */
	MopsRun small;
	for (const auto n : {1ULL, 7ULL, 16ULL, 33ULL, 64ULL}) {
		for (const auto row : {row_cpyf, row_cpy, row_set}) {
			/* Twice: the first start fills the TLB entries. */
			mops_row(ref, got, rng, small, row, page_at(4) + 0x300,
				page_at(2) + 0x100, n, plain);
			aot::reset_stats();
			mops_row(ref, got, rng, small, row, page_at(4) + 0x300,
				page_at(2) + 0x100, n, plain);
			const auto once = aot::stats();
			require(once.enters > 0);
			require(once.mops_slow == 0);
			require(once.steps == 0);
		}
	}
	aot::reset_stats();
	mops_row(ref, got, rng, small, row_cpy, page_at(4) + 0x300,
		page_at(2) + 0x100, 100, plain);
	require(aot::stats().mops_slow == 1);
	require(aot::stats().steps == 0);

	/* A copy that faults on every page: the engine runs the instruction
	 * again at each resume, and rewritten code is entered for it once. */
	{
		auto kinds = plain;
		for (std::size_t page = 6; page < 11; ++page)
			kinds[page] = 0;
		aot::reset_stats();
		mops_row(ref, got, rng, small, row_cpyf, page_at(6), page_at(0),
			5 * page_bytes, kinds);
		const auto resumed = aot::stats();
		require(resumed.mops_fault >= 1);
		require(resumed.mops_fault <= 2);
	}
}

/* A signal handler can leave a copy in the middle of its epilogue with the
 * flags or the count edited, so the stage is entered in states the prologue
 * never formats: the flags clear with bytes left (an exception), N set on a
 * forward copy (ignored), a count above the limit. The engine decides each;
 * rewritten code must agree, including the exception syndrome. */
[[gnu::noinline]]
void memory_copy_and_set_started_at_the_epilogue(const LibraryImage &image)
{
	constexpr std::uint32_t flag_n = 0x8000'0000U, flag_z = 0x4000'0000U,
		flag_c = 0x2000'0000U, flag_v = 0x1000'0000U;
	constexpr std::uint64_t copy_limit = 0x007f'ffff'ffff'ffffULL,
		set_limit = 0x7fff'ffff'ffff'ffffULL;
	const auto bias = Fixture::bias;
	Fixture ref{4611, image}, got{4612, image};
	SplitMix rng{37};
	MopsRun run;
	aot::reset_stats();
	const auto base = arena_base(bias);
	const auto page_at = [&](std::size_t page) {
		return base + page * page_bytes;
	};
	const std::vector<std::uint8_t> plain(arena_pages + stack_pages, 2);
	auto broken = plain;
	broken[5] = 0;
	const std::uint32_t flags[] = {0, flag_c, flag_n | flag_c,
		flag_z | flag_c, flag_v | flag_c, flag_n, flag_z,
		flag_n | flag_z | flag_c | flag_v};
	for (std::size_t row = 0; row < mops_rows; ++row) {
		const auto limit = mops_is_set(row) ? set_limit : copy_limit;
		const std::uint64_t counts[] = {0, 1, 64, 65, 4097, limit,
			limit + 1, ~0ULL};
		for (const auto nzcv : flags)
			for (const auto n : counts) {
				const MopsStart start{8, nzcv};
				/* Inside one page each. */
				mops_row(ref, got, rng, run, row, page_at(4) + 0x300,
					page_at(2) + 0x100, n, plain, 0x1234, start);
				/* The destination ends in an unmapped page. */
				mops_row(ref, got, rng, run, row, page_at(5) - 20,
					page_at(2) + 0x40, n, broken, 0x1234, start);
				/* The source below an overlapping destination. */
				mops_row(ref, got, rng, run, row, page_at(3) + 0x410,
					page_at(3) + 0x400, n, plain, 0x1234, start);
			}
	}
	const auto st = aot::stats();
	std::fprintf(stderr, "epilogue starts: %zu compared, %zu faulted; "
		"enters %llu steps %llu\n", run.compared, run.faulted,
		static_cast<unsigned long long>(st.enters),
		static_cast<unsigned long long>(st.steps));
	require(run.compared > 1000);
	require(run.faulted > 100);
	require(st.enters > run.compared / 2);
}

/* Advanced SIMD structure accesses that span two pages, with the TLB entry
 * of one of them already filled by an earlier byte load: the first page
 * unmapped and the last cached, and the reverse, for loads and stores, at
 * every split of the access. */
[[gnu::noinline]]
void structure_accesses_over_a_boundary_match_the_engine(
	const LibraryImage &image)
{
	const auto bias = Fixture::bias;
	Fixture ref{4621, image}, got{4622, image};
	SplitMix rng{43};
	const auto base = arena_base(bias);
	const auto page_at = [&](std::size_t page) {
		return base + page * page_bytes;
	};
	std::size_t compared = 0;
	aot::reset_stats();
	for (std::size_t row = 0; row < simd_warm_rows; ++row)
		for (const std::uint64_t lead : {1ULL, 8ULL, 15ULL, 16ULL, 17ULL,
		     32ULL, 48ULL, 63ULL, 64ULL})
			for (const std::uint8_t first : {0, 1, 2})
				for (const std::uint8_t last : {0, 1, 2})
					for (const bool cache_first : {false, true})
						for (const std::uint64_t tag : {0ULL, 0x5aULL << 56}) {
							auto c = make_case_from_seed(rng.next(), bias,
								simd_warm_at + row * simd_warm_step, true,
								true, base);
							enterable(c);
							c.kinds.assign(arena_pages + stack_pages, 2);
							c.kinds[5] = first;
							c.kinds[6] = last;
							c.cpu.registers[1] = (page_at(6) - lead) | tag;
							c.cpu.registers[2] = cache_first ?
								page_at(5) + 0x10 : page_at(6) + 0x20;
							compare_case(ref, got, c,
								"structure access over a boundary");
							++compared;
						}
	const auto st = aot::stats();
	std::fprintf(stderr, "structure accesses over a boundary: %zu compared; "
		"enters %llu steps %llu\n", compared,
		static_cast<unsigned long long>(st.enters),
		static_cast<unsigned long long>(st.steps));
	require(st.enters > compared / 2);
}

/* A function that retires a handful of instructions costs more to enter than
 * the engine takes to run it: after two such entries in a row the engine runs
 * it, and rewritten code is entered again one time in 64. */
[[gnu::noinline]]
void short_entries_are_left_to_the_engine(const LibraryImage &image)
{
	const auto bias = Fixture::bias;
	Fixture ref{4701, image}, got{4702, image};
	SplitMix rng{37};
	auto c = make_case_from_seed(rng.next(), bias, mops_at + row_cpyf * mops_step,
		true, true, arena_base(bias));
	enterable(c);
	const auto base = arena_base(bias);
	c.cpu.registers[0] = base + 4 * page_bytes + 0x300;
	c.cpu.registers[2] = 7;
	c.cpu.registers[1] = base + 2 * page_bytes + 0x100;
	c.kinds.assign(arena_pages + stack_pages, 2);
	aot::set_enabled(false);
	const auto want = drive(ref.env, ref.arena, c, max_blocks);
	require(want.done());
	aot::set_enabled(true);
	aot::reset_stats();
	for (int i = 0; i < 200; ++i)
		require(same(want, drive(got.env, got.arena, c, aot_max_blocks), c));
	aot::set_enabled(false);
	const auto st = aot::stats();
	std::fprintf(stderr, "short entries: 200 runs, enters %llu, left to the "
		"engine %llu\n", static_cast<unsigned long long>(st.enters),
		static_cast<unsigned long long>(st.refusals[4]));
	require(st.enters >= 2);
	require(st.enters < 100);
	require(st.refusals[4] > 100);
}

/* The history the engine keeps for admission (the address a stepped
 * instruction faulted at, and the addresses whose entries were short) is a
 * hint keyed by the address alone. Nothing resets it when the text under the
 * address changes, a mapping goes or the executor runs in another address
 * space, so the rows below warm it without ever toggling diagnostics (which
 * resets it) and then change each of those: the run must be the engine's,
 * and entering must resume once the text is the framework's again. */
[[gnu::noinline]]
void admission_history_survives_changes_as_a_hint_only(
	const LibraryImage &image)
{
	const auto bias = Fixture::bias;
	const auto base = arena_base(bias);
	const auto row_pc = mops_at + row_cpyf * mops_step;
	const auto changed_at = row_pc + 8;
	constexpr std::uint32_t nop = 0xd503201fU;
	SplitMix rng{53};
	auto c = make_case_from_seed(rng.next(), bias, row_pc, true, true, base);
	enterable(c);
	c.kinds.assign(arena_pages + stack_pages, 2);
	c.cpu.registers[0] = base + 4 * page_bytes + 0x300;
	c.cpu.registers[1] = base + 2 * page_bytes + 0x100;
	c.cpu.registers[2] = 16;
	std::uint32_t original;
	std::memcpy(&original, &image.bytes[changed_at], 4);
	LibraryImage changed = image;
	std::memcpy(&changed.bytes[changed_at], &nop, 4);
	/* What the engine does with each text, computed before AOT is on. */
	aot::set_enabled(false);
	Fixture ref_original{4901, image}, ref_changed{4902, changed};
	const auto want_original = drive(ref_original.env, ref_original.arena, c,
		max_blocks);
	const auto want_changed = drive(ref_changed.env, ref_changed.arena, c,
		max_blocks);
	require(want_original.done() && want_changed.done());
	require(!identical(want_original, want_changed));
	/* The page that holds the row is not mapped at all. */
	const std::size_t row_page = row_pc / page_bytes;
	Environment hole_ref_env{4903, bias, image, row_page};
	Arena hole_ref_arena{4903, base};
	const auto want_hole = drive(hole_ref_env, hole_ref_arena, c, max_blocks);
	require(want_hole.done());
	require(!identical(want_original, want_hole));

	Fixture got{4904, image};
	Fixture other{4905, changed};
	Environment hole_env{4906, bias, image, row_page};
	Arena hole_arena{4906, base};
	aot::reset_stats();
	aot::set_enabled(true);
	const auto run = [&](Environment &env, Arena &arena,
	                     const Result &want, std::size_t times,
	                     const char *what) {
		for (std::size_t i = 0; i < times; ++i) {
			/* The executor is got's in every row; the address space is
			 * the one named. */
			const auto have = drive_on(got.env.executor, env.memory,
				env.generation, env.bias, arena, c, aot_max_blocks);
			if (!same(want, have, c)) {
				std::fprintf(stderr, "  %s, run %zu\n", what, i);
				require(false);
			}
		}
	};
	/* The change is published in got's own address space by name: the
	 * address space the publication reaches by default is the one the
	 * thread touched last, which is not got's here. */
	const auto publish = [&](std::uint32_t word) {
		got.env.backing.put32(changed_at, word);
		require(darwin_uml_static_vm_invalidate_code_generation(
			got.env.generation, got.env.bias + changed_at, 4) == 0);
	};
	/* The history is warm: the entries are refused and the verdicts live. */
	run(got.env, got.arena, want_original, 200, "warming");
	const auto warm = aot::stats();
	require(warm.enters >= 2);
	require(warm.refusals[4] > 100);
	/* The text under the address changes (and is published). A refused entry
	 * is the engine's; the one in 64 that is tried finds the page changed. */
	publish(nop);
	run(got.env, got.arena, want_changed, 200, "text changed");
	const auto after_change = aot::stats();
	require(after_change.refusals[4] > warm.refusals[4]);
	/* The bytes are back: rewritten code is entered again. */
	publish(original);
	run(got.env, got.arena, want_original, 200, "text restored");
	require(aot::stats().enters > after_change.enters);
	/* Another address space whose text differs at the address. */
	const auto before_other = aot::stats();
	run(other.env, other.arena, want_changed, 200, "other address space");
	require(aot::stats().refusals[4] > before_other.refusals[4]);
	/* Another address space in which the page is not mapped. */
	run(hole_env, hole_arena, want_hole, 200, "unmapped text");
	/* And back in the first one. */
	run(got.env, got.arena, want_original, 200, "first address space again");
	aot::set_enabled(false);
	const auto st = aot::stats();
	std::fprintf(stderr, "admission history: enters %llu, left to the "
		"engine %llu\n", static_cast<unsigned long long>(st.enters),
		static_cast<unsigned long long>(st.refusals[4]));
	require(st.poisoned == 0);
}

/* The address a stepped instruction faulted at is kept as a hint too: the
 * guest resumes at it once the fault is handled, and the engine runs it
 * again rather than entering rewritten code. Nothing resets it when the text
 * at that address changes, the page is replaced, or the executor goes on in
 * another address space, so each row stops on a stepped memory fault, changes
 * what is under the faulting address in one of those ways, and resumes at the
 * same address on the same executor without toggling diagnostics (which
 * resets the hint). The run must be what the engine alone does, whatever the
 * hint says, and rewritten code must be entered again once the guest has
 * moved on. */
[[gnu::noinline]]
void a_stepped_fault_hint_survives_changes_as_a_hint_only(
	const LibraryImage &image)
{
	const auto bias = Fixture::bias;
	const auto base = arena_base(bias);
	const auto row_pc = mops_at + row_cpyf * mops_step;
	/* ldr x3, [x0]: x0 is the destination the row's first instruction
	 * faults at, so the address faults again, now as a plain load. */
	constexpr std::uint32_t load = 0xf9400003U;
	const std::size_t row_page = row_pc / page_bytes;
	SplitMix rng{59};
	auto c = make_case_from_seed(rng.next(), bias, row_pc, true, true, base);
	enterable(c);
	c.kinds.assign(arena_pages + stack_pages, 2);
	c.kinds[4] = 0;
	c.cpu.registers[0] = base + 4 * page_bytes + 0x300;
	c.cpu.registers[1] = base + 2 * page_bytes + 0x100;
	c.cpu.registers[2] = 16;
	/* Where the engine stops: the instruction of the row that faults. */
	std::uint64_t fault_at = 0;
	{
		aot::set_enabled(false);
		Fixture probe{4999, image};
		const auto first = drive_on(probe.env.executor, probe.env.memory,
			probe.env.generation, bias, probe.arena, c, max_blocks, {}, true);
		require(first.last == EngineEvent::memory_fault);
		require(first.faults.size() == 1);
		fault_at = first.faults[0].pc - bias;
		require(fault_at >= row_pc && fault_at < row_pc + mops_step);
	}
	LibraryImage changed = image;
	std::memcpy(&changed.bytes[fault_at], &load, 4);

	enum Change : std::size_t {
		nothing, text_patched, page_replaced, other_space, page_unmapped,
		changes,
	};
	static constexpr const char *what[changes] = {"nothing changed",
		"text patched and published", "page replaced",
		"another address space with other text",
		"another address space without the page"};
	struct Ending final {
		Result stopped, ended;
	};
	/* Stop at the fault, change what is under it, resume there. */
	const auto sequence = [&](Change change,
	                          gadgets::GadgetExecutor &executor,
	                          unsigned long id, std::size_t blocks) {
		Environment env{id, bias, image};
		Arena arena{id, base};
		JitBacking replacement{1};
		replacement.put(0, std::span{changed.bytes}.subspan(
			row_page * page_bytes, page_bytes));
		Environment other{id + 1, bias, changed};
		Arena other_arena{id + 1, base};
		Environment hole{id + 2, bias, image, row_page};
		Arena hole_arena{id + 2, base};
		Ending out;
		out.stopped = drive_on(executor, env.memory, env.generation, bias,
			arena, c, blocks, {}, true);
		auto *memory = &env.memory;
		auto generation = env.generation;
		auto *where = &arena;
		switch (change) {
		case nothing:
			break;
		case text_patched:
			env.backing.put32(fault_at, load);
			require(darwin_uml_static_vm_invalidate_code_generation(
				env.generation, bias + fault_at, 4) == 0);
			break;
		case page_replaced:
			env.map.replace_page(bias + row_page * page_bytes,
				replacement);
			break;
		case other_space:
			other_arena.adopt(arena);
			memory = &other.memory;
			generation = other.generation;
			where = &other_arena;
			break;
		case page_unmapped:
			hole_arena.adopt(arena);
			memory = &hole.memory;
			generation = hole.generation;
			where = &hole_arena;
			break;
		case changes:
			break;
		}
		out.ended = run_from(executor, *memory, generation, *where,
			out.stopped.cpu, blocks);
		return out;
	};
	/* What the engine does, on fresh guests, before AOT is on. */
	aot::set_enabled(false);
	gadgets::GadgetExecutor ref_executor;
	std::array<Ending, changes> want;
	for (std::size_t i = 0; i < changes; ++i)
		want[i] = sequence(static_cast<Change>(i), ref_executor,
			5000 + 10 * i, max_blocks);
	for (const auto &w : want) {
		require(w.stopped.last == EngineEvent::memory_fault);
		require(w.stopped.faults.size() == 1);
		require(w.stopped.faults[0].pc == bias + fault_at);
		require(w.ended.done());
	}
	require(!identical(want[nothing].ended, want[text_patched].ended));
	require(!identical(want[nothing].ended, want[page_unmapped].ended));

	gadgets::GadgetExecutor executor;
	aot::reset_stats();
	aot::set_enabled(true);
	for (std::size_t i = 0; i < changes; ++i) {
		const auto steps = aot::stats().steps;
		const auto have = sequence(static_cast<Change>(i), executor,
			5100 + 10 * i, aot_max_blocks);
		/* The fault was a stepped instruction's, which is what arms the
		 * hint. */
		if (aot::stats().steps <= steps) {
			std::fprintf(stderr, "  %s: no stepped instruction\n", what[i]);
			require(false);
		}
		if (!same(want[i].stopped, have.stopped, c) ||
		    !same(want[i].ended, have.ended, c)) {
			std::fprintf(stderr, "  %s\n", what[i]);
			require(false);
		}
	}
	const auto after_changes = aot::stats();
	require(after_changes.poisoned == 0);
	/* Back in an address space with the framework's text, with every page
	 * mapped: the guest has moved on, and rewritten code is entered. */
	auto ok = c;
	ok.kinds.assign(arena_pages + stack_pages, 2);
	Fixture again{5200, image};
	aot::set_enabled(true);
	for (int i = 0; i < 200; ++i)
		(void)drive_on(executor, again.env.memory, again.env.generation,
			bias, again.arena, ok, aot_max_blocks);
	aot::set_enabled(false);
	const auto st = aot::stats();
	std::fprintf(stderr, "stepped fault hint: %zu changes, enters %llu, "
		"steps %llu\n", static_cast<std::size_t>(changes),
		static_cast<unsigned long long>(st.enters),
		static_cast<unsigned long long>(st.steps));
	require(st.enters > after_changes.enters);
	require(st.poisoned == 0);
}

/* The guest kernel's threads run the executor on a task stack (the size the
 * kernel configuration gives), with its own frames and a signal frame above
 * ours: nothing on the way into rewritten code may need much of it (a 64 KiB
 * temporary in the executor killed the first boot of a guest). The scenarios below run on the last such stack of a
 * mapping whose first megabyte is inaccessible, painted beforehand, so an
 * overflow faults and the high-water mark is measured: a first entry (the
 * verification of a page and a refill), a native copy, the host routine
 * for a longer one, a fault that is resolved and resumed, a step the
 * engine takes (an exception), and structure accesses that span a
 * boundary. */
[[gnu::noinline]]
void rewritten_code_stays_within_a_stack_budget(const LibraryImage &image)
{
	constexpr std::size_t stack_bytes = AOT_TEST_KERNEL_STACK_BYTES;
	constexpr std::size_t moat_bytes = 1024 * 1024;
	/* What the executor, the harness and the host routines may use beyond
	 * the thread's own start-up. */
	constexpr std::size_t budget = 16 * 1024;
	const auto bias = Fixture::bias;
	const auto base = arena_base(bias);
	const auto page_at = [&](std::size_t page) {
		return base + page * page_bytes;
	};
	struct Scenario final {
		std::unique_ptr<Fixture> fixture;
		Case c;
	};
	std::vector<Scenario> scenarios;
	unsigned long generation = 4800;
	SplitMix rng{47};
	const auto add = [&](std::uint64_t pc_offset) -> Case & {
		scenarios.push_back({std::make_unique<Fixture>(++generation, image),
			make_case_from_seed(rng.next(), bias, pc_offset, true, true,
				base)});
		auto &c = scenarios.back().c;
		enterable(c);
		c.kinds.assign(arena_pages + stack_pages, 2);
		return c;
	};
	const auto row_at = [&](std::size_t row) {
		return mops_at + row * mops_step;
	};
	{	/* A first entry: verification, refill, the native copy. */
		auto &c = add(row_at(row_cpyf));
		c.cpu.registers[0] = page_at(4) + 0x300;
		c.cpu.registers[1] = page_at(2) + 0x100;
		c.cpu.registers[2] = 16;
	}
	{	/* The host routine: a longer copy across a boundary. */
		auto &c = add(row_at(row_cpy));
		c.cpu.registers[0] = page_at(4) + 0xff0;
		c.cpu.registers[1] = page_at(2) + 0x100;
		c.cpu.registers[2] = 3 * page_bytes;
	}
	{	/* A fault, resolved and resumed. */
		auto &c = add(row_at(row_cpyf));
		c.kinds[5] = 0;
		c.cpu.registers[0] = page_at(5) - 20;
		c.cpu.registers[1] = page_at(2) + 0x40;
		c.cpu.registers[2] = 5000;
	}
	{	/* The engine takes an exception: bytes left with the flags clear. */
		auto &c = add(row_at(row_set) + 8);
		c.cpu.nzcv = 0;
		c.cpu.registers[0] = page_at(4) + 0x300;
		c.cpu.registers[2] = 65;
	}
	{	/* A structure access over a boundary, first page unmapped. */
		auto &c = add(simd_warm_at);
		c.kinds[5] = 0;
		c.cpu.registers[1] = page_at(6) - 16;
		c.cpu.registers[2] = page_at(6) + 0x20;
	}
	{	/* A structure store over a boundary, last page read-only. */
		auto &c = add(simd_warm_at + simd_warm_step);
		c.kinds[6] = 1;
		c.cpu.registers[1] = page_at(6) - 48;
		c.cpu.registers[2] = page_at(5) + 0x20;
	}
	aot::set_enabled(true);
	aot::reset_stats();
	const auto run = [&](std::size_t count) -> std::size_t {
		void *const mapping = mmap(nullptr, moat_bytes + stack_bytes,
			PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		require(mapping != MAP_FAILED);
		require(mprotect(mapping, moat_bytes, PROT_NONE) == 0);
		auto *const stack = static_cast<std::uint64_t *>(
			static_cast<void *>(static_cast<char *>(mapping) + moat_bytes));
		constexpr std::uint64_t paint = 0xa5a5'5a5a'c3c3'3c3cULL;
		for (std::size_t i = 0; i < stack_bytes / 8; ++i)
			stack[i] = paint;
		struct Job final {
			std::vector<Scenario> *scenarios;
			std::size_t count;
		} job{&scenarios, count};
		pthread_attr_t attributes;
		require(pthread_attr_init(&attributes) == 0);
		require(pthread_attr_setstack(&attributes, stack, stack_bytes) == 0);
		pthread_t thread;
		require(pthread_create(&thread, &attributes, [](void *argument) -> void * {
			auto *work = static_cast<Job *>(argument);
			for (std::size_t i = 0; i < work->count; ++i) {
				auto &one = (*work->scenarios)[i];
				(void)drive(one.fixture->env, one.fixture->arena, one.c,
					aot_max_blocks);
			}
			return nullptr;
		}, &job) == 0);
		require(pthread_join(thread, nullptr) == 0);
		pthread_attr_destroy(&attributes);
		std::size_t untouched = 0;
		while (untouched < stack_bytes / 8 && stack[untouched] == paint)
			++untouched;
		require(munmap(mapping, moat_bytes + stack_bytes) == 0);
		return stack_bytes - untouched * 8;
	};
	/* An empty run measures what the thread itself uses. The scenarios then
	 * run one after another on a fresh stack, each on its own fixture, so
	 * each one verifies its pages on that stack; the mark is the deepest. */
	const auto start_up = run(0);
	const auto high_water = run(scenarios.size());
	aot::set_enabled(false);
	std::fprintf(stderr, "stack: thread start-up %zu bytes, high water "
		"with the executor %zu bytes (budget %zu above start-up)\n",
		start_up, high_water, budget);
	require(aot::stats().enters > 0);
	require(high_water <= start_up + budget);
	/* The runtime's own measure agrees with the painted stack: the sampler
	 * assumes the kernel's task stack size, finds the pthread's stack to be
	 * the host stack it is, never reports more free than the stack held at
	 * its deepest, saw the thread's own start-up above it, and stayed
	 * inside the growth budget over the start of each run. */
	const auto seen = aot::stats();
	std::fprintf(stderr, "stack: sampled %llu left of %llu (kind %llu); "
		"engine %llu at run start; deepest sample %llu below it\n",
		static_cast<unsigned long long>(seen.stack_min_free),
		static_cast<unsigned long long>(seen.stack_size_at_min),
		static_cast<unsigned long long>(seen.stack_kind_at_min),
		static_cast<unsigned long long>(seen.stack_engine_free),
		static_cast<unsigned long long>(seen.stack_max_growth));
	require(seen.stack_task_bytes == stack_bytes);
	require(seen.stack_kind_at_min == 1);
	require(seen.stack_size_at_min == stack_bytes);
	require(seen.stack_min_free <= stack_bytes);
	require(seen.stack_engine_free <= stack_bytes);
	require(stack_bytes - seen.stack_min_free >= start_up);
	require(stack_bytes - seen.stack_min_free <= high_water);
	require(seen.stack_engine_free >= seen.stack_min_free);
	/* A first entry verifies a page into a buffer of its own: deeper than the
	 * start of the run, by something, and by no more than the budget. */
	require(seen.stack_max_growth > 0);
	require(seen.stack_max_growth <= budget);
}

/* ---- Every exit lies on the stepped trace ---------------------------------
 *
 * The engine run one instruction at a time visits, in order, every state a
 * guest can observe of a vCPU. A run through rewritten code returns the
 * engine at some of them and never at another: whatever ends a run (the
 * budget, an interrupt, a fault, a breakpoint, a system call) leaves the
 * registers, flags, FPCR and FPSR, thread pointer, vector registers and
 * memory of one state of that trace, and later exits come later on it. The
 * trace holds the state after each instruction and the state at each fault the
 * run repairs and resumes, which a memory copy or set that stops between its
 * pages leaves in the middle of the instruction; a fault exit lies on the
 * second kind and every other exit on the first (Trace). A debugger, a signal
 * frame, ptrace and a crash report see only such states. (The exclusive
 * monitor is not guest-visible state and is not part of the comparison.) */

void print_difference(const char *tag, const Snapshot &want,
	const Snapshot &have)
{
	const auto word = [&](const char *name, std::uint64_t a,
	                      std::uint64_t b) {
		if (a != b)
			std::fprintf(stderr, "    %s %s: trace %llx, exit %llx\n", tag,
				name, static_cast<unsigned long long>(a),
				static_cast<unsigned long long>(b));
	};
	word("pc", want.pc, have.pc);
	word("sp", want.sp, have.sp);
	for (std::size_t i = 0; i < 31; ++i) {
		char name[8];
		std::snprintf(name, sizeof(name), "x%zu", i);
		word(name, want.x[i], have.x[i]);
	}
	word("nzcv", want.nzcv, have.nzcv);
	word("fpcr", want.fpcr, have.fpcr);
	word("fpsr", want.fpsr, have.fpsr);
	word("tls", want.tls, have.tls);
	word("vectors", want.vectors, have.vectors);
	word("memory", want.memory, have.memory);
}

void prepare_arena(Arena &arena, const Case &c)
{
	for (std::size_t i = 0; i < arena.total(); ++i)
		if (i < c.kinds.size())
			arena.set_kind(i, c.kinds[i]);
	SplitMix fill{c.seed};
	arena.fill(fill);
}

/* `c` run one instruction at a time by the engine: the state at the start,
 * after each instruction and at each fault the run repairs, with the arena
 * hashed where `hash_memory`. */
struct Stepped final {
	Trace states;
	Result result;
};

[[nodiscard, gnu::noinline]]
Stepped run_stepped(Environment &env, Arena &arena, const Case &c, std::size_t cap, bool hash_memory)
{
	Stepped out;
	aot::set_enabled(false);
	auto start = c;
	start.cpu.pc = env.bias + c.pc_offset;
	prepare_arena(arena, start);
	const auto memory = [&]() -> std::uint64_t {
		return hash_memory ? hash_bytes(arena.snapshot()) : 0;
	};
	const auto record = [&](const arm64::CpuState &cpu, bool fault) {
		out.states.push(snapshot_of(cpu, memory()), fault);
	};
	record(start.cpu, false);
	env.executor.set_single_step(true);
	/* A fault the run repairs (a page of the arena it gets) is a state of
	 * the trace too: a memory copy or set commits the bytes before the page
	 * that faults, and the state it stops in is neither before nor after the
	 * instruction. */
	out.result = run_from(env.executor, env.memory, env.generation, arena,
		start.cpu, cap, [&](const arm64::CpuState &cpu) {
		record(cpu, false);
	}, false, [&](const arm64::CpuState &cpu) { record(cpu, true); });
	env.executor.set_single_step(false);
	/* A run that ends at a fault ends in the state recorded for it, which is
	 * not also an instruction boundary. */
	if (out.result.last != EngineEvent::memory_fault)
		out.states.push(snapshot_of(out.result.cpu,
			hash_memory ? hash_bytes(out.result.memory) : 0), false);
	return out;
}

/* The number of instructions after which `c`, run one at a time on the
 * engine, is in `target` (arena included), or SIZE_MAX if it is not within
 * `cap` of them. The arena is hashed only where the rest matches. A state at
 * a fault is not looked at: the targets are interrupt and breakpoint stops,
 * which leave a guest at an instruction boundary. */
[[nodiscard, gnu::noinline]] std::size_t step_to(Environment &env,
	Arena &arena, const Case &c, std::size_t cap, const Snapshot &target)
{
	aot::set_enabled(false);
	auto start = c;
	start.cpu.pc = env.bias + c.pc_offset;
	prepare_arena(arena, start);
	std::size_t index = SIZE_MAX, steps = 0;
	auto without_memory = target;
	without_memory.memory = 0;
	const auto look = [&](const arm64::CpuState &cpu) {
		if (index != SIZE_MAX || snapshot_of(cpu) != without_memory)
			return;
		auto now = snapshot_of(cpu, hash_bytes(arena.snapshot()));
		if (now == target)
			index = steps;
	};
	look(start.cpu);
	env.executor.set_single_step(true);
	(void)run_from(env.executor, env.memory, env.generation, arena,
		start.cpu, cap, [&](const arm64::CpuState &cpu) {
		++steps;
		look(cpu);
	});
	env.executor.set_single_step(false);
	return index;
}

/* The comparison must see a difference in any one field. Each of the fields
 * of a state in the middle of a trace is changed in turn, and the changed
 * state must be on no part of the trace. */
[[gnu::noinline]]
void the_matcher_notices_every_field(const Trace &trace)
{
	require(trace.size() >= 8);
	const auto middle = trace[trace.size() / 2];
	std::size_t fields = 0;
	const auto mutate = [&](const char *what, auto &&change) {
		auto now = middle;
		change(now);
		++fields;
		if (trace.find(0, now, false) != trace.size() ||
		    trace.find(0, now, true) != trace.size()) {
			std::fprintf(stderr, "the trace comparison does not see a "
				"changed %s\n", what);
			require(false);
		}
	};
	constexpr std::uint64_t flip = 0x5a5a'5a5a'5a5a'5a5aULL;
	mutate("pc", [](Snapshot &s) { s.pc ^= flip; });
	mutate("sp", [](Snapshot &s) { s.sp ^= flip; });
	mutate("x0", [](Snapshot &s) { s.x[0] ^= flip; });
	mutate("x15", [](Snapshot &s) { s.x[15] ^= flip; });
	mutate("x30", [](Snapshot &s) { s.x[30] ^= flip; });
	mutate("nzcv", [](Snapshot &s) { s.nzcv ^= 0xf000'0000U; });
	mutate("fpcr", [](Snapshot &s) { s.fpcr ^= 0x00c0'0000U; });
	mutate("fpsr", [](Snapshot &s) { s.fpsr ^= 0x0000'0010U; });
	mutate("tls", [](Snapshot &s) { s.tls ^= flip; });
	mutate("vectors", [](Snapshot &s) { s.vectors ^= flip; });
	mutate("memory", [](Snapshot &s) { s.memory ^= flip; });
	require(fields == 11);
}

struct TraceTotals final {
	std::size_t candidates{}, tried{}, traced{}, unbounded{},
		unsteppable{}, exits{}, steps{};
};

/* A sample of the starts the differential uses, each run on the engine one
 * instruction at a time (recording every state, arena included) and then
 * through rewritten code, where every state the engine is returned at, each
 * fault, and the end must be on the recorded path in order. A case whose
 * stepped run is not the engine's ordinary run (it ends differently) says
 * nothing about the path and is left out; the count is reported. */
[[gnu::noinline]]
TraceTotals exits_lie_on_the_stepped_trace(
	const Config &config, const LibraryImage &image,
	const std::vector<Function> &functions, const std::vector<Site> &sites)
{
	constexpr std::uint64_t bias = 0x7123'0000'0000ULL;
	Environment ref{4111, bias, image};
	Environment step{4112, bias, image};
	Environment test{4113, bias, image};
	Arena ref_arena{ref.generation, arena_base(bias)};
	Arena step_arena{step.generation, arena_base(bias)};
	Arena test_arena{test.generation, arena_base(bias)};
	SplitMix rng{config.master ^ 0x57e9ULL};
	std::vector<std::uint64_t> starts;
	for (const auto &f : functions) {
		starts.push_back(f.start);
		starts.push_back(f.start + 4 * rng.below((f.end - f.start) / 4));
	}
	for (const auto &site : sites)
		starts.push_back(site.offset);
	for (std::size_t i = starts.size(); i > 1; --i)
		std::swap(starts[i - 1], starts[rng.below(i)]);
	TraceTotals t;
	t.candidates = starts.size();
	bool matcher_tried = false;
	for (const auto start : starts) {
		if (t.traced >= config.trace_cases)
			break;
		++t.tried;
		const bool faulting = rng.below(3) == 0;
		const bool tbi = rng.below(4) != 0;
		auto c = make_case(rng, bias, start, faulting, tbi,
			arena_base(bias));
		randomize_fp_state(c);
		aot::set_enabled(false);
		const auto want = drive(ref, ref_arena, c, max_blocks);
		if (!want.done()) {
			++t.unbounded;
			continue;
		}
		const auto stepped = run_stepped(step, step_arena, c,
			config.trace_cap, true);
		if (!stepped.result.done()) {
			++t.unbounded;
			continue;
		}
		if (!identical(want, stepped.result)) {
			++t.unsteppable;
			continue;
		}
		const auto &trace = stepped.states;
		std::size_t at = 0, exits = 0;
		bool off = false;
		const auto check = [&](const arm64::CpuState &cpu, const char *what,
		                       bool fault) {
			if (off)
				return;
			++exits;
			const auto now = snapshot_of(cpu,
				hash_bytes(test_arena.snapshot()));
			const auto found = trace.find(at, now, fault);
			if (found != trace.size()) {
				at = found;
				return;
			}
			off = true;
			describe("an exit is off the stepped trace", c);
			std::fprintf(stderr, "  %s %zu, after trace state %zu of %zu, "
				"start %llx\n", what, exits, at, trace.size(),
				static_cast<unsigned long long>(start));
			print_difference("trace state", trace[at], now);
			if (at + 1 < trace.size())
				print_difference("next trace state", trace[at + 1], now);
			const auto earlier = trace.find(0, now, fault);
			if (earlier != trace.size())
				std::fprintf(stderr, "  the state is on the trace, "
					"earlier, at %zu\n", earlier);
		};
		aot::set_enabled(true);
		const auto got = drive(test, test_arena, c, aot_max_blocks,
			[&](const arm64::CpuState &cpu) {
				check(cpu, "block exit", false);
			},
			[&](const arm64::CpuState &cpu) { check(cpu, "fault", true); });
		aot::set_enabled(false);
		/* A run that ends at a fault ends in the state of that fault. */
		check(got.cpu, "end", got.last == EngineEvent::memory_fault);
		require(!off);
		require(same(want, got, c));
		t.exits += exits;
		t.steps += trace.size();
		++t.traced;
		if (!matcher_tried && trace.size() >= 8) {
			the_matcher_notices_every_field(trace);
			matcher_tried = true;
		}
	}
	return t;
}

/* A memory copy or set that faults in the middle of its epilogue leaves the
 * engine in a state between the start and the end of the instruction: the
 * stage has committed the bytes before the page that faults and the registers
 * hold the progress. The single-stepped run stops in the same state at the
 * same fault, so it is a state of the stepped trace, and rewritten code
 * returns the engine there as well. Every row of the fixture runs from its
 * prologue, untagged and with tagged pointers: forward into a destination
 * page that is unmapped or read-only and then into a second unmapped one, a
 * source that runs into an unmapped page, and backward over an overlapping
 * destination whose next page below faults. Each exit of rewritten code, each
 * fault and the end must lie on the stepped trace; a trace without the
 * states at faults fails this row on its first case, whatever sample the
 * differential draws. The last cases return the first fault as an ordinary
 * exit, which the check must refuse: the state is one only the fault reaches,
 * and the fault is still to be taken. */
[[gnu::noinline]]
void memory_copy_and_set_faults_lie_on_the_stepped_trace(
	const LibraryImage &image)
{
	const auto bias = Fixture::bias;
	Fixture step{4615, image}, got{4616, image};
	SplitMix rng{41};
	const auto base = arena_base(bias);
	const auto page_at = [&](std::size_t page) {
		return base + page * page_bytes;
	};
	const std::vector<std::uint8_t> plain(arena_pages + stack_pages, 2);
	const auto with = [&](std::initializer_list<
		std::pair<std::size_t, std::uint8_t>> changes) {
		auto kinds = plain;
		for (const auto &[page, kind] : changes)
			kinds[page] = kind;
		return kinds;
	};
	std::size_t cases = 0, fault_exits = 0;
	aot::reset_stats();
	enum class Lie { none, once, every };
	std::size_t terminal_cases = 0;
	const auto check_case = [&](std::size_t row, std::uint64_t destination,
	                            std::uint64_t source, std::uint64_t count,
	                            const std::vector<std::uint8_t> &kinds,
	                            bool terminal = false, Lie lie = Lie::none) {
		auto c = make_case_from_seed(rng.next(), bias,
			mops_at + row * mops_step, true, true, base);
		enterable(c);
		c.kinds = kinds;
		const bool high = row == row_cpyf_high || row == row_cpy_high ||
			row == row_set_high;
		const unsigned d = high ? 17 : 0, n = high ? 16 : 2, s = high ? 15 : 1;
		c.cpu.registers[d] = destination;
		c.cpu.registers[n] = count;
		if (row != row_set_zero)
			c.cpu.registers[s] = mops_is_set(row) ? 0x5aULL : source;
		const auto stepped = run_stepped(step.env, step.arena, c, 256, true);
		/* A terminal case ends at a fault no page repairs. */
		require(terminal ? !stepped.result.finished &&
			stepped.result.last == EngineEvent::memory_fault :
			stepped.result.finished);
		require(!stepped.result.faults.empty());
		const auto &trace = stepped.states;
		std::size_t at = 0;
		bool off = false;
		const char *blamed = "";
		const auto check = [&](const arm64::CpuState &cpu, const char *what,
		                       bool fault) {
			if (off)
				return;
			const auto now = snapshot_of(cpu,
				hash_bytes(got.arena.snapshot()));
			const auto found = trace.find(at, now, fault);
			if (found != trace.size()) {
				at = found;
				return;
			}
			off = true;
			blamed = what;
			if (lie != Lie::none)
				return;
			describe("a MOPS exit is off the stepped trace", c);
			std::fprintf(stderr, "  row %zu, %s, after trace state %zu of "
				"%zu\n", row, what, at, trace.size());
			print_difference("trace state", trace[at], now);
		};
		aot::set_enabled(true);
		g_faults_to_report_as_continued = lie == Lie::once ? 1 :
			lie == Lie::every ? SIZE_MAX : 0;
		g_faults_reported_as_continued = 0;
		const auto before = aot::stats();
		const auto have = drive(got.env, got.arena, c,
			lie == Lie::none ? aot_max_blocks : 4000,
			[&](const arm64::CpuState &cpu) { check(cpu, "block exit", false); },
			[&](const arm64::CpuState &cpu) {
				++fault_exits;
				check(cpu, "fault", true);
			});
		aot::set_enabled(false);
		/* A run that ends at a fault ends in the state of that fault. */
		check(have.cpu, "end", have.last == EngineEvent::memory_fault);
		const auto after = aot::stats();
		const auto replaced = g_faults_reported_as_continued;
		g_faults_to_report_as_continued = 0;
		if (lie != Lie::none) {
			/* The check must refuse the wrong exit, at that exit and not at
			 * the fault the retry takes: a state only a fault reaches is not
			 * one a run is left in by an ordinary exit. */
			require(replaced == 1 || (lie == Lie::every && replaced > 1));
			if (!off || std::strcmp(blamed, "block exit") != 0) {
				describe("an ordinary exit at a state only a fault reaches "
					"was not refused", c);
				std::fprintf(stderr, "  row %zu, %s, %s\n", row,
					lie == Lie::once ? "the first fault" : "every fault",
					off ? blamed : "accepted");
			}
			require(off);
			require(std::strcmp(blamed, "block exit") == 0);
			return;
		}
		require(!off);
		require(same(stepped.result, have, c));
		/* This case, and not another, ran in rewritten code and took the
		 * fault in the host routine of its epilogue. */
		require(after.enters == before.enters + 1);
		require(after.mops_fault == before.mops_fault + 1);
		++(terminal ? terminal_cases : cases);
	};
	for (const std::uint64_t tag : {0ULL, 0x5aULL << 56}) {
		for (const std::uint8_t kind : {std::uint8_t{0}, std::uint8_t{1}}) {
			for (std::size_t row = 0; row < mops_rows; ++row) {
				/* Forward: 20 bytes in page 4, then page 5 (unmapped or
				 * read-only) faults, then page 6 (unmapped) does. */
				check_case(row, (page_at(4) + page_bytes - 20) | tag,
					(page_at(1) + 0x40) | tag, page_bytes + 60,
					with({{5, kind}, {6, 0}}));
				/* The source: 20 bytes in page 2, then page 3 faults. A
				 * read-only page reads, so only the unmapped one does. */
				if (!mops_is_set(row) && kind == 0)
					check_case(row, (page_at(7) + 0x20) | tag,
						(page_at(2) + page_bytes - 20) | tag, 80,
						with({{3, 0}}));
			}
			/* Backward: the destination is above the source, the bytes
			 * of page 4 go first and page 3 faults. */
			for (const auto row : {row_cpy, row_cpy_high})
				check_case(row, (page_at(4) - 24) | tag,
					(page_at(4) - 40) | tag, 80, with({{3, kind}}));
		}
	}
	const auto st = aot::stats();
	std::fprintf(stderr, "memory copy and set faults on the stepped trace: "
		"%zu cases, %zu fault exits; enters %llu mops_fault %llu\n", cases,
		fault_exits, static_cast<unsigned long long>(st.enters),
		static_cast<unsigned long long>(st.mops_fault));
	require(cases == 44);
	require(fault_exits >= cases);
	/* Each case is entered once, at its prologue, and faults once in the
	 * host routine of the epilogue (checked case by case); a case the engine
	 * ran alone would pass the trace check and say nothing about rewritten
	 * code. */
	require(st.enters == cases);
	require(st.mops_fault == cases);
	/* Runs that end at a fault no page repairs: the destination or the source
	 * runs past the last page of the arena. The state of that fault is the
	 * end of the run and not also an instruction boundary. */
	const auto top = page_at(arena_pages + stack_pages);
	for (const std::uint64_t tag : {0ULL, 0x5aULL << 56}) {
		for (std::size_t row = 0; row < mops_rows; ++row) {
			check_case(row, (top - 100) | tag, (page_at(1) + 0x40) | tag,
				page_bytes, plain, true);
			if (!mops_is_set(row))
				check_case(row, (page_at(4) + 0x40) | tag, (top - 50) | tag,
					page_bytes, plain, true);
		}
	}
	require(terminal_cases == 22);
	/* The wrong exit: the first fault, and every fault, returned as an
	 * ordinary exit, for faults a page repairs and for the one that ends a
	 * run. */
	for (const auto lie : {Lie::once, Lie::every}) {
		check_case(row_cpyf, page_at(4) + page_bytes - 20, page_at(1) + 0x40,
			page_bytes + 60, with({{5, 0}, {6, 0}}), false, lie);
		check_case(row_set, page_at(4) + page_bytes - 20, 0, page_bytes + 60,
			with({{5, 0}, {6, 0}}), false, lie);
		check_case(row_cpyf, page_at(7) + 0x20, page_at(2) + page_bytes - 20,
			80, with({{3, 0}}), false, lie);
		check_case(row_cpy, page_at(4) - 24, page_at(4) - 40, 80,
			with({{3, 0}}), false, lie);
		check_case(row_set, top - 100, 0, page_bytes, plain, true, lie);
		check_case(row_cpyf, top - 100, page_at(1) + 0x40, page_bytes, plain,
			true, lie);
		check_case(row_cpyf, page_at(4) + 0x40, top - 50, page_bytes, plain,
			true, lie);
	}
}

/* ---- An interrupt token flipped during a native loop -----------------------
 *
 * The production entry (the engine adapter) runs a long loop of rewritten code
 * while another thread flips the token that interrupts the vCPU, either the
 * stop flag or the sequence word, after the vCPU has run `after` blocks and a
 * delay that lands it anywhere in the one that follows. The vCPU waits for
 * the flip only once it has run `within` more blocks, so the exit is
 * bounded whatever the host does with the flipping thread; the race is real
 * for as long as the flipper is quicker than that. */

struct FlipPlan final {
	bool sequence;
	std::size_t after, within;
	unsigned delay_ns;
};

struct FlipState final {
	std::atomic_bool stop{false};
	unsigned int sequence{0};
	std::atomic<std::size_t> polls{0};
	std::atomic_bool go{false}, flipped{false};
	std::atomic<std::size_t> polls_at_flip{0};
	std::size_t release_at{}, park_at{};
};

[[nodiscard]] bool wait_for(const std::atomic_bool &flag,
	std::chrono::milliseconds limit) noexcept
{
	const auto deadline = std::chrono::steady_clock::now() + limit;
	while (!flag.load(std::memory_order_acquire))
		if (std::chrono::steady_clock::now() >= deadline)
			return false;
	return true;
}

/* The interrupt callback of the engine's loop: called once at the top of each
 * round of it, while no token is pending. */
[[nodiscard]] bool flip_poll(void *context) noexcept
{
	auto &state = *static_cast<FlipState *>(context);
	const auto n = state.polls.fetch_add(1, std::memory_order_relaxed) + 1;
	if (n == state.release_at)
		state.go.store(true, std::memory_order_release);
	if (n == state.park_at)
		(void)wait_for(state.flipped, std::chrono::milliseconds{5000});
	return false;
}

enum class FlipLoop { alu, add };

/* The two loops of the fixture: four instructions to a lap of registers
 * alone, and an atomic add to a cell of the arena. */
[[nodiscard]] Case flip_case(FlipLoop loop, std::uint64_t bias,
	std::uint64_t iterations)
{
	if (loop == FlipLoop::alu) {
		auto c = loop_case(77, bias, 0x1000, arena_base(bias));
		c.cpu.registers[3] = iterations;
		return c;
	}
	auto c = loop_case(78, bias, hammer_add, arena_base(bias));
	c.cpu.registers[0] = iterations;
	c.cpu.registers[2] = arena_base(bias) + 0x100;
	return c;
}

struct Flipped final {
	bool flipped{};
	uml::cpu::ExecutionResult first, last;
	arm64::CpuState stopped, ended;
	std::vector<std::byte> stopped_memory, ended_memory;
	std::size_t polls_at_flip{}, polls_total{};
	std::uint64_t longest_entry{}, enters{};
};

/* `c` on the gadget engine's own entry with rewritten code on. With `resume`,
 * the token is then cleared and the run goes on to the guest's system call. */
[[nodiscard, gnu::noinline]] Flipped flip_through_the_engine(
	const LibraryImage &image, unsigned long generation, const Case &c,
	const FlipPlan &plan, bool resume)
{
	Flipped out;
	Fixture got{generation, image};
	prepare_arena(got.arena, c);
	FlipState state;
	state.release_at = plan.after;
	state.park_at = plan.after + plan.within;
	aot::reset_stats();
	aot::set_enabled(true);
	/* The entry is chosen when the engine is made. */
	auto engine = arm64::detail::make_gadget_engine({});
	auto cpu = c.cpu;
	uml::cpu::ExecutionRequest request{
		.cpu = cpu,
		.memory = got.env.memory,
		.translated_memory = &got.env.memory,
		.generation = generation,
		.instruction_budget = std::size_t{1} << 60,
		.interrupt = {
			.sequence = plan.sequence ? &state.sequence : nullptr,
			.snapshot = 0,
			.stop = plan.sequence ? nullptr : &state.stop,
			.poll = flip_poll,
			.poll_context = &state,
		},
	};
	std::thread flipper{[&] {
		if (!wait_for(state.go, std::chrono::milliseconds{5000}))
			return;
		const auto until = std::chrono::steady_clock::now() +
			std::chrono::nanoseconds{plan.delay_ns};
		while (std::chrono::steady_clock::now() < until) {
		}
		if (plan.sequence)
			__atomic_add_fetch(&state.sequence, 1U, __ATOMIC_RELEASE);
		else
			state.stop.store(true, std::memory_order_release);
		state.polls_at_flip.store(state.polls.load(
			std::memory_order_relaxed), std::memory_order_relaxed);
		state.flipped.store(true, std::memory_order_release);
	}};
	out.first = engine->run(request);
	flipper.join();
	out.flipped = state.flipped.load();
	out.stopped = cpu;
	out.stopped_memory = got.arena.snapshot();
	out.polls_at_flip = state.polls_at_flip.load();
	out.polls_total = state.polls.load();
	const auto st = aot::stats();
	out.longest_entry = st.max_retired;
	out.enters = st.enters;
	if (resume) {
		if (plan.sequence)
			request.interrupt.snapshot = state.sequence;
		else
			state.stop.store(false);
		request.interrupt.poll = nullptr;
		request.interrupt.poll_context = nullptr;
		out.last = engine->run(request);
		out.ended = cpu;
		out.ended_memory = got.arena.snapshot();
	}
	aot::set_enabled(false);
	return out;
}

/* The flipped token ends the run in a state on the engine's own path, and the
 * run then goes on to the same end as the engine alone reaches. */
[[gnu::noinline]]
void interrupt_flips_leave_the_engine_on_the_stepped_trace(
	const LibraryImage &image)
{
	constexpr std::uint64_t iterations = 400'000;
	constexpr std::size_t within = 6;
	const auto bias = Fixture::bias;
	unsigned long generation = 6000;
	std::size_t cases = 0, longest = 0;
	for (const auto loop : {FlipLoop::alu, FlipLoop::add}) {
		const auto c = flip_case(loop, bias, iterations);
		aot::set_enabled(false);
		Fixture ref{++generation, image};
		const auto want = drive(ref.env, ref.arena, c, 4'000'000);
		require(want.finished);
		const auto want_end = snapshot_of(want.cpu, hash_bytes(want.memory));
		for (const bool sequence : {false, true})
		for (const std::size_t after : {std::size_t{1}, std::size_t{4},
		     std::size_t{16}})
		for (const unsigned delay : {0U, 1500U}) {
			const FlipPlan plan{sequence, after, within, delay};
			const auto out = flip_through_the_engine(image, ++generation,
				c, plan, true);
			require(out.flipped);
			require(out.first.reason == uml::cpu::ExitReason::interrupt);
			require(out.enters > 0);
			const auto target = snapshot_of(out.stopped,
				hash_bytes(out.stopped_memory));
			Fixture step{++generation, image};
			const auto cap = (after + within + 2) * entry_bound + 64;
			const auto at = step_to(step.env, step.arena, c, cap, target);
			if (at == SIZE_MAX) {
				describe("an interrupt exit is off the stepped trace", c);
				std::fprintf(stderr, "  %s token, after %zu, delay %u ns, "
					"loop %d, pc %llx\n", sequence ? "sequence" : "stop",
					after, delay, static_cast<int>(loop),
					static_cast<unsigned long long>(target.pc));
				require(false);
			}
			require(out.last.reason == uml::cpu::ExitReason::system_call);
			const auto end = snapshot_of(out.ended,
				hash_bytes(out.ended_memory));
			if (!(end == want_end)) {
				describe("a run resumed after the interrupt ends elsewhere",
					c);
				print_difference("resumed", want_end, end);
				require(false);
			}
			longest = std::max(longest, at);
			++cases;
		}
	}
	std::fprintf(stderr, "interrupt flips: %zu on the stepped trace "
		"(longest %zu instructions)\n", cases, longest);
	require(cases == 24);
}

/* A stop takes effect within one block: the token is looked at at the top of
 * each round of the engine's loop, a round runs one block, and a block of
 * rewritten code is bounded by its budget. So after the flip at most the
 * round in flight (its poll counted before or after the flipper's read) can
 * come before the exit, no entry is longer than the budget allows, and the
 * run ends where it was waiting for the flip or earlier. */
[[gnu::noinline]]
void a_stop_takes_effect_within_one_run_block(
	const LibraryImage &image)
{
	constexpr std::uint64_t iterations = 400'000;
	constexpr std::size_t flips = 96, within = 10;
	const auto bias = Fixture::bias;
	SplitMix rng{0x5709};
	unsigned long generation = 6500;
	std::size_t latest = 0, waited = 0;
	for (std::size_t i = 0; i < flips; ++i) {
		const auto loop = rng.below(2) != 0 ? FlipLoop::alu : FlipLoop::add;
		const FlipPlan plan{rng.below(2) != 0, 1 + rng.below(40), within,
			static_cast<unsigned>(rng.below(4000))};
		const auto c = flip_case(loop, bias, iterations);
		const auto out = flip_through_the_engine(image, ++generation, c,
			plan, false);
		require(out.flipped);
		require(out.first.reason == uml::cpu::ExitReason::interrupt);
		require(out.enters > 0);
		require(out.polls_total >= plan.after);
		require(out.polls_total <= plan.after + plan.within);
		require(out.polls_total >= out.polls_at_flip);
		if (out.polls_total - out.polls_at_flip > 1) {
			std::fprintf(stderr, "a stop took %zu rounds: %s token, after "
				"%zu, delay %u ns\n",
				out.polls_total - out.polls_at_flip,
				plan.sequence ? "sequence" : "stop", plan.after,
				plan.delay_ns);
			require(false);
		}
		require(out.longest_entry <= entry_bound);
		latest = std::max(latest, out.polls_total - plan.after);
		waited += out.polls_total == plan.after + plan.within ? 1 : 0;
	}
	std::fprintf(stderr, "stop latency: %zu flips, every stop within one "
		"round of the flip; furthest run after the release %zu blocks, %zu "
		"flips arrived only after the vCPU waited\n", flips, latest, waited);
}

/* The loop of the fixture runs on this vCPU, rewritten code on, while another
 * thread writes a breakpoint into its text and publishes it by the bridge
 * once the vCPU has run `publish_at` blocks. The vCPU waits for the
 * publication at block `park_at`, so the run is short whatever the other
 * thread does. The vCPU must reach the breakpoint within the block in
 * flight and one lap of the loop, having started rewritten code at most once
 * after the publication, in a state on the engine's own path, and once the
 * word is back the loop goes on, in rewritten code, to the end the engine
 * alone reaches from the same state. The vCPU must have entered rewritten
 * code before the publication began. Returns whether the vCPU ran a block
 * while the publication was in progress. */
[[gnu::noinline]]
bool breakpoint_on_a_running_loop(const LibraryImage &image,
	unsigned long generation, std::size_t publish_at, std::size_t park_at)
{
	constexpr std::uint32_t brk = 0xd4200000U;
	constexpr std::uint64_t spot = 0x1004;
	const auto bias = Fixture::bias;
	std::uint32_t original;
	std::memcpy(&original, &image.bytes[spot], 4);
	auto race = loop_case(93, bias, 0x1000, arena_base(bias));
	race.cpu.registers[3] = std::uint64_t{1} << 40;
	Fixture got{generation, image};
	prepare_arena(got.arena, race);
	aot::reset_stats();
	aot::set_enabled(true);
	std::atomic<std::size_t> blocks{0}, blocks_before_call{0},
		blocks_at_publish{0}, enters_at_publish{0};
	std::atomic<std::uint64_t> enters_before_call{0};
	std::atomic_bool published{false};
	std::atomic<int> published_rc{-1};
	std::thread patcher{[&] {
		const auto deadline = std::chrono::steady_clock::now() +
			std::chrono::seconds{5};
		while (blocks.load(std::memory_order_acquire) < publish_at)
			if (std::chrono::steady_clock::now() >= deadline)
				return;
		got.env.backing.put32(spot, brk);
		enters_before_call.store(aot::stats().enters);
		blocks_before_call.store(blocks.load(std::memory_order_acquire));
		published_rc.store(darwin_uml_static_vm_invalidate_code_generation(
			got.env.generation, bias + spot, 4));
		enters_at_publish.store(aot::stats().enters);
		blocks_at_publish.store(blocks.load(std::memory_order_acquire));
		published.store(true, std::memory_order_release);
	}};
	auto cpu = race.cpu;
	EngineEvent last = EngineEvent::continued;
	for (;;) {
		const auto n = blocks.fetch_add(1, std::memory_order_acq_rel) + 1;
		if (n == park_at)
			(void)wait_for(published, std::chrono::milliseconds{5000});
		require(n < 100000);
		last = run_selected(got.env.executor, got.env.memory,
			got.env.generation, cpu).event;
		if (last != EngineEvent::continued && last != EngineEvent::interrupt)
			break;
	}
	patcher.join();
	arm64::restore_native_fp_environment(cpu);
	require(published_rc.load() == 0);
	require(last == EngineEvent::breakpoint);
	require(cpu.pc == bias + spot);
	/* A publication that finds the vCPU still outside rewritten code proves
	 * nothing about entering it. */
	require(enters_before_call.load() > 0);
	const auto after = blocks.load() - blocks_at_publish.load();
	const auto entered_after =
		aot::stats().enters - enters_at_publish.load();
	const bool raced = blocks_at_publish.load() > blocks_before_call.load();
	require(after <= 8);
	require(entered_after <= 1);
	const auto target = snapshot_of(cpu, hash_bytes(got.arena.snapshot()));
	Fixture step{generation + 1000, image};
	const auto cap = (park_at + 2) * entry_bound + 64;
	if (step_to(step.env, step.arena, race, cap, target) == SIZE_MAX) {
		describe("the breakpoint stop is off the stepped trace", race);
		std::fprintf(stderr, "  pc %llx x0 %llx, published after %zu blocks\n",
			static_cast<unsigned long long>(target.pc),
			static_cast<unsigned long long>(target.x[0]), publish_at);
		require(false);
	}
	/* The word is back; the vCPU goes on to the loop's end, a little further
	 * on, and so does the engine alone from the same state. */
	got.env.backing.put32(spot, original);
	require(darwin_uml_static_vm_invalidate_code_generation(
		got.env.generation, bias + spot, 4) == 0);
	auto resumed = cpu;
	resumed.registers[3] = resumed.registers[0] + 20000;
	aot::set_enabled(false);
	Fixture ref{generation + 2000, image};
	prepare_arena(ref.arena, race);
	const auto want = run_from(ref.env.executor, ref.env.memory,
		ref.env.generation, ref.arena, resumed, 2'000'000);
	require(want.finished);
	aot::set_enabled(true);
	const auto entered_before_resume = aot::stats().enters;
	(void)run_until_stub(got.env.executor, got.env.memory,
		got.env.generation, resumed);
	aot::set_enabled(false);
	require(aot::stats().enters > entered_before_resume);
	require(aot::stats().poisoned == 0);
	const auto end = snapshot_of(resumed, hash_bytes(got.arena.snapshot()));
	const auto want_end = snapshot_of(want.cpu, hash_bytes(want.memory));
	if (!(end == want_end)) {
		describe("a loop resumed after the breakpoint ends elsewhere", race);
		print_difference("resumed", want_end, end);
		require(false);
	}
	return raced;
}

/* ---- A breakpoint published through the generation bridge -------------------
 *
 * What ptrace does to the guest's text: write the instruction, then publish
 * the change by `darwin_uml_static_vm_invalidate_code_generation`. The page
 * must not be entered while the breakpoint is in it, the run must stop at the
 * breakpoint with the state of the engine alone (an exit event, pc at the
 * instruction), and rewritten code must be entered again once the original
 * word is back. And with the loop running on a vCPU when the breakpoint is
 * published, the vCPU must reach it within a bounded number of blocks,
 * having started rewritten code at most once more, in a state on the
 * engine's own path. */
[[gnu::noinline]]
void a_breakpoint_published_through_the_bridge_stops_rewritten_code(
	const LibraryImage &image)
{
	constexpr std::uint32_t brk = 0xd4200000U;
	const auto bias = Fixture::bias;
	const auto publish = [&](Fixture &f, std::uint64_t at,
	                         std::uint32_t word) {
		f.env.backing.put32(at, word);
		return darwin_uml_static_vm_invalidate_code_generation(
			f.env.generation, bias + at, 4);
	};
	const auto original_at = [&](std::uint64_t at) {
		std::uint32_t word;
		std::memcpy(&word, &image.bytes[at], 4);
		return word;
	};
	unsigned long generation = 7000;
	/* The four instructions of the loop: add, b, cmp, b.lo. */
	constexpr std::uint64_t spots[] = {0x1004, 0x1008, 0x1024, 0x1028};
	auto c = loop_case(91, bias, 0x1004, arena_base(bias));
	c.cpu.registers[0] = 300;
	c.cpu.registers[3] = 2000;
	for (const auto spot : spots) {
		aot::set_enabled(false);
		Fixture ref_original{++generation, image};
		Fixture ref_patched{++generation, image};
		const auto want_original = drive(ref_original.env,
			ref_original.arena, c, 2'000'000);
		require(want_original.finished);
		require(publish(ref_patched, spot, brk) == 0);
		const auto want_stopped = drive(ref_patched.env, ref_patched.arena,
			c, 2'000'000);
		require(want_stopped.last == EngineEvent::breakpoint);
		require(want_stopped.cpu.pc == bias + spot);

		Fixture got{++generation, image};
		aot::reset_stats();
		aot::set_enabled(true);
		require(same(want_original, drive(got.env, got.arena, c,
			aot_max_blocks), c));
		const auto entered = aot::stats().enters;
		require(entered > 0);
		/* The breakpoint is in the page. */
		require(publish(got, spot, brk) == 0);
		const auto stopped = drive(got.env, got.arena, c, aot_max_blocks);
		require(same(want_stopped, stopped, c));
		require(stopped.last == EngineEvent::breakpoint);
		require(stopped.cpu.pc == bias + spot);
		require(aot::stats().enters == entered);
		/* The original word is back. */
		require(publish(got, spot, original_at(spot)) == 0);
		require(same(want_original, drive(got.env, got.arena, c,
			aot_max_blocks), c));
		require(aot::stats().enters > entered);
		aot::set_enabled(false);
	}

	/* The loop runs on this vCPU while another thread publishes. Overlap is
	 * a matter of scheduling, so the schedule is a bounded one and it must
	 * observe the overlap at least once: the publication points go on past
	 * the first sweep (up to 96 runs in all) until the vCPU was seen running
	 * a block while the publication was in progress, and a schedule that
	 * never sees it fails. */
	std::size_t raced = 0, attempts = 0;
	constexpr std::size_t max_attempts = 96;
	for (const std::size_t publish_at : {std::size_t{3}, std::size_t{8},
	     std::size_t{16}, std::size_t{5}, std::size_t{12}, std::size_t{2}}) {
		for (int again = 0; again < 4 && raced < 3; ++again) {
			++attempts;
			raced += breakpoint_on_a_running_loop(image, ++generation,
				publish_at, publish_at + 24) ? 1 : 0;
		}
	}
	for (std::size_t publish_at = 2; raced == 0 && attempts < max_attempts;
	     publish_at = publish_at % 29 + 2) {
		++attempts;
		raced += breakpoint_on_a_running_loop(image, ++generation,
			publish_at, publish_at + 24) ? 1 : 0;
	}
	std::fprintf(stderr, "breakpoint on a running loop: %zu runs, %zu with the "
		"vCPU running during the publication\n", attempts, raced);
	require(raced >= 1);
}

/* The kernel declares a page immutable (`guest_code` 3) or it does not (1).
 * Rewritten code is entered only for pages that have the fact. Another
 * address space can map the frame of a mutable page writable and publish new
 * bytes, and the address space that compared the page is told nothing: its
 * verdict about the page holds for the bytes at the time, and rewritten code
 * would go on running them for every function of the page, one that was never
 * entered before included, where the engine reads the new bytes when it first
 * translates it. Two address spaces share one frame. The first runs a loop of
 * the page, which compares the page; the second publishes a changed word of
 * another function through the bridge; the first then runs that function and
 * must do what the engine does, whether its page never had the fact or lost it
 * after rewritten code was entered. */
[[gnu::noinline]]
void a_page_without_the_immutable_fact_is_not_run_natively(
	const LibraryImage &image)
{
	const auto bias = Fixture::bias;
	const auto base = arena_base(bias);
	const auto row_pc = mops_at + row_cpyf * mops_step;
	const auto changed_at = row_pc + 8;
	constexpr std::uint32_t nop = 0xd503201fU;
	SplitMix rng{61};
	auto c = make_case_from_seed(rng.next(), bias, row_pc, true, true, base);
	enterable(c);
	c.kinds.assign(arena_pages + stack_pages, 2);
	c.cpu.registers[0] = base + 4 * page_bytes + 0x300;
	c.cpu.registers[1] = base + 2 * page_bytes + 0x100;
	c.cpu.registers[2] = 16;
	/* The loop that is run first, on the page of the row. */
	auto warm = flip_case(FlipLoop::add, bias, 3000);
	warm.kinds.assign(arena_pages + stack_pages, 2);
	require(hammer_add / page_bytes == row_pc / page_bytes);
	LibraryImage changed = image;
	std::memcpy(&changed.bytes[changed_at], &nop, 4);
	aot::set_enabled(false);
	Fixture ref_original{5801, image}, ref_changed{5802, changed};
	const auto want_warm = drive(ref_original.env, ref_original.arena, warm,
		max_blocks);
	const auto want_original = drive(ref_original.env, ref_original.arena, c,
		max_blocks);
	const auto want_changed = drive(ref_changed.env, ref_changed.arena, c,
		max_blocks);
	require(want_warm.done() && want_original.done() && want_changed.done());
	require(!identical(want_original, want_changed));

	enum Arm : int { mutable_page, revoked_after_entry, arms };
	static constexpr const char *what[arms] = {
		"a mutable page", "a page that lost the fact"};
	unsigned long generation = 5810;
	for (int arm = 0; arm < arms; ++arm) {
		JitBacking frame{image.pages};
		frame.put(0, image.bytes);
		Environment env{generation++, bias, image, SIZE_MAX, &frame,
			arm == mutable_page ? 1 : 3};
		Arena arena{env.generation, base};
		/* The second address space: the frame, writable. */
		const auto writer_generation = generation++;
		GenerationMapping writer{writer_generation, bias, frame,
			image.pages, PROT_READ | PROT_WRITE | PROT_EXEC, true};
		const auto run = [&](const Case &which, const Result &want,
		                     std::size_t times, const char *phase) {
			for (std::size_t i = 0; i < times; ++i) {
				const auto have = drive(env, arena, which, aot_max_blocks);
				if (!same(want, have, which)) {
					const auto st = aot::stats();
					std::fprintf(stderr, "  %s, %s, run %zu: enters %llu "
						"mutable pages %llu\n", what[arm], phase, i,
						static_cast<unsigned long long>(st.enters),
						static_cast<unsigned long long>(st.mutable_pages));
					require(false);
				}
			}
		};
		aot::reset_stats();
		aot::set_enabled(true);
		run(warm, want_warm, 8, "warming");
		const auto warmed = aot::stats();
		if (arm == revoked_after_entry)
			require(warmed.enters >= 2);
		if (arm == revoked_after_entry) {
			/* The kernel maps the range again without the fact. */
			require(darwin_uml_static_vm_activate(env.generation) == 0);
			require(darwin_uml_static_vm_map_generation(env.generation,
				reinterpret_cast<void *>(bias),
				frame.descriptor(), 0, image.pages * page_bytes,
				PROT_READ | PROT_EXEC, 1, 0) == 0);
		}
		frame.put32(changed_at, nop);
		require(darwin_uml_static_vm_invalidate_code_generation(
			writer_generation, bias + changed_at, 4) == 0);
		run(c, want_changed, 200, "after the publication");
		aot::set_enabled(false);
		/* Nothing was entered without the fact, nor once it was gone. */
		require(aot::stats().enters == warmed.enters);
		require(arm != mutable_page || warmed.enters == 0);
		require(aot::stats().poisoned == 0);
	}
}

/* The engine adapter chooses between the plain block loop and the one with
 * rewritten code when its executor is made, once, and neither loop tests it
 * again. So an engine made while rewritten code is off never enters it, even
 * once it is switched on, and one made while it is on keeps entering it
 * after it is switched off; the diagnostics switch decides only for
 * executors made later (and for the harness's own dispatch). */
[[gnu::noinline]]
void the_dispatch_choice_is_made_when_the_executor_is_made(
	const LibraryImage &image)
{
	const auto bias = Fixture::bias;
	auto c = loop_case(77, bias, 0x1000, arena_base(bias));
	c.cpu.registers[3] = 20000;
	unsigned long generation = 7500;
	const auto entries = [&](uml::cpu::CpuEngine &engine) {
		Fixture f{++generation, image};
		prepare_arena(f.arena, c);
		auto cpu = c.cpu;
		uml::cpu::ExecutionRequest request{
			.cpu = cpu,
			.memory = f.env.memory,
			.translated_memory = &f.env.memory,
			.generation = generation,
			.instruction_budget = std::size_t{1} << 40,
		};
		aot::reset_stats();
		const auto result = engine.run(request);
		require(result.reason == uml::cpu::ExitReason::system_call);
		require(cpu.registers[0] == 20000);
		return aot::stats().enters;
	};
	aot::set_enabled(false);
	GadgetExecutor made_off;
	require(!made_off.aot_selected());
	auto off = arm64::detail::make_gadget_engine({});
	aot::set_enabled(true);
	GadgetExecutor made_on;
	require(made_on.aot_selected());
	auto on = arm64::detail::make_gadget_engine({});
	const auto off_while_on = entries(*off);
	const auto on_while_on = entries(*on);
	aot::set_enabled(false);
	const auto off_while_off = entries(*off);
	const auto on_while_off = entries(*on);
	aot::set_enabled(false);
	std::fprintf(stderr, "dispatch choice: made off %llu entries while on, "
		"%llu while off; made on %llu while on, %llu while off\n",
		static_cast<unsigned long long>(off_while_on),
		static_cast<unsigned long long>(off_while_off),
		static_cast<unsigned long long>(on_while_on),
		static_cast<unsigned long long>(on_while_off));
	require(off_while_on == 0 && off_while_off == 0);
	require(on_while_on > 0 && on_while_off > 0);
}


/* The IC IVAU instructions of the library that rewritten code leaves to the
 * engine, from the sites the rewriter reported as steps: SYS #3, C7, C5, #1
 * with a register that is not the zero register. */
[[nodiscard]] std::vector<std::uint64_t> code_synchronization_sites(
	const LibraryImage &image, const std::set<std::uint64_t> &steps)
{
	std::vector<std::uint64_t> sites;
	for (const auto offset : steps) {
		if (offset + 4 > image.bytes.size())
			continue;
		std::uint32_t word;
		std::memcpy(&word, &image.bytes[offset], 4);
		if ((word & 0xffff'ffe0U) == 0xd50b'7520U && (word & 31U) != 31U)
			sites.push_back(offset);
	}
	return sites;
}

/* An IC IVAU that rewritten code leaves to the engine, run through the
 * engine's entry as the product runs it, is the guest's own: it is made in
 * the generation the engine runs, whichever generation the thread last
 * activated (the kernel selects one for its own calls, and a vCPU that ran
 * another process's task last still holds that one). An address that
 * generation maps nothing at faults, at the instruction, as hardware
 * faults it for a load, and the native code that
 * follows it does not run: the entry loop reports the fault at the
 * instruction that armed it. Where the address is mapped the instruction
 * retires and publishes the change in the running generation alone. The
 * active generation maps the address in one arm and not in the others, so
 * an instruction resolved there takes the wrong turn in two of the three.
 *
 * What the row proves about the executor's early exit, run_aot leaving at an
 * instruction whose helper armed the note: the fault is at the IC IVAU and the
 * retry hint is left there, so the retry (rewritten code still on) is the
 * ordinary engine's one instruction. Without the exit that run went on, the
 * engine's own check of the guest's code sequence ended it at the next
 * instruction, and the retry entered rewritten code and stepped the IC IVAU
 * again; the row fails on that.
 * What it cannot prove: the exit also matters where the bridge answers
 * non-zero and moves no code sequence of the running generation (no address
 * space for the thread, an exception inside the call, an entered context of
 * another address space's lifetime), and that answer cannot be made from a
 * test without a hook that fails the bridge in the middle of a run. That
 * case is covered by reading the exit, not by this row.
 * Returns how many sites the library has. */
[[gnu::noinline]]
std::size_t code_synchronization_in_rewritten_code(const LibraryImage &image,
	const std::set<std::uint64_t> &steps)
{
	const auto sites = code_synchronization_sites(image, steps);
	constexpr std::size_t most = 3;
	const auto bias = Fixture::bias;
	const auto base = arena_base(bias);
	constexpr std::size_t unmapped_page = 3;
	static_assert(unmapped_page < arena_pages);
	const auto target = base + unmapped_page * page_bytes + 0x40;
	enum Arm : int { nowhere, guest_only, active_only, arms };
	static constexpr const char *what[arms] = {
		"the page is mapped in neither generation",
		"the page is mapped in the guest's generation only",
		"the page is mapped in the active generation only"};
	unsigned long generation = 8100;
	const auto chosen = std::min(most, sites.size());
	for (std::size_t n = 0; n < chosen; ++n) {
		const auto site = sites[n * sites.size() / chosen];
		std::uint32_t word;
		std::memcpy(&word, &image.bytes[site], 4);
		const auto reg = word & 31U;
		for (int arm = 0; arm < arms; ++arm) {
			const auto guest = generation++;
			const auto active = generation++;
			Fixture f{guest, image};
			Arena active_arena{active, base};
			if (arm == guest_only)
				f.arena.set_kind(unmapped_page, 1);
			if (arm == active_only)
				active_arena.set_kind(unmapped_page, 1);
			arm64::CpuState cpu{};
			cpu.tbi_enabled = true;
			cpu.pc = bias + site;
			cpu.sp = base + (arena_pages + stack_pages) * page_bytes - 0x40;
			cpu.registers[30] = bias + stub_offset;
			cpu.registers[reg] = target;
			const auto guest_before = darwin_uml_static_vm_code_changes(guest);
			const auto active_before =
				darwin_uml_static_vm_code_changes(active);
			/* The kernel's last selection on this thread. */
			require(darwin_uml_static_vm_activate(active) == 0);
			aot::reset_stats();
			aot::set_enabled(true);
			auto engine = arm64::detail::make_gadget_engine({});
			uml::cpu::ExecutionRequest request{
				.cpu = cpu,
				.memory = f.env.memory,
				.translated_memory = &f.env.memory,
				.generation = guest,
				.instruction_budget = std::size_t{1} << 20,
			};
			const auto result = engine->run(request);
			const auto steps_taken = aot::stats().steps;
			const auto faulted_at_target =
				result.reason == uml::cpu::ExitReason::memory_fault &&
				result.fault.address == target;
			const auto say = [&](const char *failure) {
				std::fprintf(stderr, "  IC IVAU at %llx, %s: %s (exit %d, "
					"pc %llx, address %llx, retired %zu, steps %llu)\n",
					static_cast<unsigned long long>(site), what[arm], failure,
					static_cast<int>(result.reason),
					static_cast<unsigned long long>(cpu.pc),
					static_cast<unsigned long long>(result.fault.address),
					result.retired_instructions,
					static_cast<unsigned long long>(steps_taken));
				require(false);
			};
			if (steps_taken == 0)
				say("the instruction was not stepped by rewritten code");
			if (darwin_uml_static_vm_code_changes(active) != active_before)
				say("the active generation heard of the guest's IC IVAU");
			if (arm == guest_only) {
				aot::set_enabled(false);
				if (faulted_at_target || cpu.pc == bias + site)
					say("an IC IVAU at a mapped address faulted");
				if (darwin_uml_static_vm_code_changes(guest) == guest_before)
					say("the guest's generation did not hear of its IC IVAU");
				continue;
			}
			if (!faulted_at_target ||
			    result.fault.access != uml::memory::MemoryAccess::read)
				say("an IC IVAU at an unmapped address did not fault");
			/* The count of instructions an entry of rewritten code retired is
			 * the budget it paid for, not what it ran: the fault's place is
			 * what the guest sees. */
			if (cpu.pc != bias + site)
				say("the fault is not at the instruction");
			/* The kernel maps the page for the guest, and the instruction
			 * runs again, with rewritten code still on: the fault left the
			 * PC at the instruction as the place the engine runs it (the
			 * retry hint a stepped fault leaves), so the retry is the
			 * ordinary engine's one instruction, and neither enters
			 * rewritten code nor steps in it. A run that went on after the
			 * instruction in rewritten code (the MMU's own advance of the
			 * guest's code sequence ends it at the next check even so) has
			 * no hint, and the retry enters rewritten code at the
			 * instruction and steps it again. */
			f.arena.set_kind(unmapped_page, 1);
			request.instruction_budget = 1;
			const auto guest_at_retry = darwin_uml_static_vm_code_changes(guest);
			const auto active_at_retry =
				darwin_uml_static_vm_code_changes(active);
			const auto before_retry = aot::stats();
			const auto again = engine->run(request);
			const auto after_retry = aot::stats();
			aot::set_enabled(false);
			const auto retry_say = [&](const char *failure) {
				std::fprintf(stderr, "  IC IVAU at %llx, %s: %s (exit %d, "
					"pc %llx, retired %zu, enters %llu, steps %llu)\n",
					static_cast<unsigned long long>(site), what[arm], failure,
					static_cast<int>(again.reason),
					static_cast<unsigned long long>(cpu.pc),
					again.retired_instructions,
					static_cast<unsigned long long>(
						after_retry.enters - before_retry.enters),
					static_cast<unsigned long long>(
						after_retry.steps - before_retry.steps));
				require(false);
			};
			if (again.reason != uml::cpu::ExitReason::instruction_budget ||
			    cpu.pc != bias + site + 4 || again.retired_instructions != 1)
				retry_say("the instruction did not run once its page was "
					"mapped");
			if (after_retry.enters != before_retry.enters ||
			    after_retry.steps != before_retry.steps)
				retry_say("the retry entered rewritten code");
			if (darwin_uml_static_vm_code_changes(guest) == guest_at_retry)
				retry_say("the guest's generation did not hear of its IC IVAU");
			if (darwin_uml_static_vm_code_changes(active) != active_at_retry)
				retry_say("the active generation heard of the guest's IC IVAU");
		}
	}
	if (chosen != 0)
		std::fprintf(stderr, "IC IVAU in rewritten code: %zu of %zu sites, "
			"%d arms each\n", chosen, sites.size(), static_cast<int>(arms));
	return sites.size();
}

} // namespace

/* A host fault inside rewritten code is a bug; say where before dying. */
void report_host_fault(int signal, siginfo_t *info, void *context)
{
	const auto *uc = static_cast<const ucontext_t *>(context);
	char text[512];
	const auto &ss = uc->uc_mcontext->__ss;
	const int n = std::snprintf(text, sizeof(text),
		"HOST FAULT signal %d address %p pc %llx x0 %llx x24 %llx x25 %llx "
		"x26 %llx x27 %llx x28 %llx lr %llx\n", signal, info->si_addr,
		static_cast<unsigned long long>(ss.__pc),
		static_cast<unsigned long long>(ss.__x[0]),
		static_cast<unsigned long long>(ss.__x[24]),
		static_cast<unsigned long long>(ss.__x[25]),
		static_cast<unsigned long long>(ss.__x[26]),
		static_cast<unsigned long long>(ss.__x[27]),
		static_cast<unsigned long long>(ss.__x[28]),
		static_cast<unsigned long long>(ss.__lr));
	(void)!write(2, text, static_cast<std::size_t>(n));
	char more[400];
	const int m = std::snprintf(more, sizeof(more),
		"x8 %llx x9 %llx x10 %llx x11 %llx x12 %llx x13 %llx x14 %llx x15 %llx x29 %llx\n",
		(unsigned long long)ss.__x[8], (unsigned long long)ss.__x[9], (unsigned long long)ss.__x[10], (unsigned long long)ss.__x[11], (unsigned long long)ss.__x[12], (unsigned long long)ss.__x[13], (unsigned long long)ss.__x[14], (unsigned long long)ss.__x[15], (unsigned long long)ss.__x[29]);
	(void)!write(2, more, static_cast<std::size_t>(m));
	void *frames[24];
	const int depth = backtrace(frames, 24);
	backtrace_symbols_fd(frames, depth, 2);
	_exit(99);
}

int main(int argc, char **argv)
{
	if (std::getenv("AOT_TRACE") != nullptr) {
		struct sigaction action {};
		action.sa_sigaction = report_host_fault;
		action.sa_flags = SA_SIGINFO;
		sigemptyset(&action.sa_mask);
		sigaction(SIGBUS, &action, nullptr);
		sigaction(SIGSEGV, &action, nullptr);
	}
	const uml::testing::RuntimeTestRoute runtime_route;
	const auto cfg = config(argc, argv);
	const auto libraries = load_libraries(cfg.libraries);
	/* The diagnostics report keeps its per-library counters in tables of 64:
	 * a 65th unit would run uncounted, and a library the guest runs but the
	 * report shows as never entered is the result this build exists to
	 * catch. */
	require(libraries.size() <= 64);
	/* The component is on by default; the harness turns it on and off row by
	 * row (set_enabled) and starts each scenario from the engine alone. */
	require(aot_runtime::enabled());
	aot::set_enabled(false);
	if (!cfg.only.empty()) {
		/* One case from a failure report, under the diagnosis: the exit
		 * status says whether every exit lay on the reference path. */
		unsigned long long seed = 0, pc = 0;
		int faulting = 0, tbi = 0;
		require(std::sscanf(cfg.only.c_str(), "%llx:%llx:%d:%d", &seed,
			&pc, &faulting, &tbi) == 4);
		constexpr std::uint64_t bias = 0x7123'0000'0000ULL;
		const auto image = load_library(
			libraries[cfg.library < 0 ? 0 : cfg.library].host);
		g_sites_image = &image;
		auto c = make_case_from_seed(seed, bias, pc, faulting != 0,
			tbi != 0, arena_base(bias));
		randomize_fp_state(c);
		return diagnose(c, image, bias) ? 0 : 1;
	}
	bool scenarios_done = false;
	std::size_t code_synchronization_sites_seen = 0;
	/* libz when the build holds it (some scenarios name its functions),
	 * otherwise the first library. */
	std::size_t scenario_index = libraries.front().index;
	for (const auto &file : libraries) {
		if (file.guest.ends_with("/libz.so")) {
			scenario_index = file.index;
			break;
		}
	}
	for (const auto &file : libraries) {
		if (cfg.library >= 0 &&
		    static_cast<long>(file.index) != cfg.library)
			continue;
		const auto image = load_library(file.host);
		g_sites_image = &image;
		const auto functions = load_functions(cfg.functions, file.index);
		std::fprintf(stderr, "library %zu %s: %zu functions\n", file.index,
			file.guest.c_str(), functions.size());
		/* A sample of a large library: evenly spaced functions. */
		std::vector<Function> sample;
		const auto stride = std::max<std::size_t>(
			1, functions.size() / cfg.sample);
		for (std::size_t i = 0; i < functions.size(); i += stride)
			sample.push_back(functions[i]);
		if (cfg.words_only) {
			words_the_engine_rejects_are_left_to_it(image, functions,
				load_steps(cfg.steps, file.index));
			continue;
		}
		if (cfg.code_synchronize_only) {
			code_synchronization_sites_seen +=
				code_synchronization_in_rewritten_code(image,
					load_steps(cfg.steps, file.index));
			continue;
		}
		if (!scenarios_done) {
			default_off_and_single_step(image, sample);
			scenarios_done = true;
		}
		aot::reset_stats();
		const auto sites = load_sites(cfg.sites, file.index);
		const auto t = differential(cfg, image, sample, sites);
		const auto st = aot::stats();
		std::fprintf(stderr,
			"differential: %zu cases, %zu compared, %zu unbounded "
			"(%zu not repeatable, %zu too slow), %zu finished, %zu faulting, "
			"%zu faults; %zu started with FP flags or an unloaded FP state, "
			"%zu of them unloaded; enters %llu steps %llu "
			"binds %llu verifications %llu retired %llu refills %llu "
			"refusals sp %llu fp %llu poisoned %llu unreadable %llu\n",
			t.cases, t.compared, t.skipped, t.nondeterministic, t.slow,
			t.finished, t.faulted, t.fault_events, t.flagged, t.deferred,
			static_cast<unsigned long long>(st.enters),
			static_cast<unsigned long long>(st.steps),
			static_cast<unsigned long long>(st.binds),
			static_cast<unsigned long long>(st.verifications),
			static_cast<unsigned long long>(st.retired),
			static_cast<unsigned long long>(st.refills),
			static_cast<unsigned long long>(st.refusals[1]),
			static_cast<unsigned long long>(st.refusals[3]),
			static_cast<unsigned long long>(st.poisoned),
			static_cast<unsigned long long>(st.unreadable));
		require(t.compared > t.cases / 2);
		require(st.enters > t.compared / 2);
		require(st.poisoned == 0);
		/* No entry ran longer than the budget allows (counted one
		 * instruction at a time with UML_AOT_COUNT_RETIRED). */
		require(st.max_retired <= entry_bound);
		/* Both entry refusals are exercised by every library's cases. */
		require(st.refusals[1] > 0);
		require(st.refusals[3] > 0);
		/* Every state rewritten code returns the engine at is on the
		 * engine's own path. */
		aot::reset_stats();
		const auto traced =
			exits_lie_on_the_stepped_trace(cfg, image, sample, sites);
		const auto trace_stats = aot::stats();
		std::fprintf(stderr, "stepped trace: %zu of %zu starts followed "
			"(%zu tried, %zu unbounded, %zu not steppable), %zu exits on "
			"%zu recorded states; enters %llu\n", traced.traced,
			traced.candidates, traced.tried, traced.unbounded,
			traced.unsteppable, traced.exits, traced.steps,
			static_cast<unsigned long long>(trace_stats.enters));
		require(traced.traced * 3 >=
			std::min(cfg.trace_cases, traced.candidates));
		require(traced.exits >= traced.traced);
		require(trace_stats.enters * 4 >= traced.traced);
		require(trace_stats.poisoned == 0);
		if (file.guest.ends_with("/libaotfixture.so")) {
			/* The same cases under the profile the runtime probes, named
			 * here whatever default the test binary installs (LSE2 among
			 * its features: unaligned atomics inside a granule do not
			 * fault). */
			const auto probed =
				::uml::platform::apple::probed_host_capabilities();
			const ::uml::cpu::HostCapabilitiesScope scope{probed};
			aot::reset_stats();
			const auto p = differential(cfg, image, sample, sites);
			const auto ps = aot::stats();
			std::fprintf(stderr, "probed profile (lse2 %d): %zu cases, "
				"%zu compared, %zu finished, %zu faults; enters %llu\n",
				probed.has(::uml::cpu::ArmFeature::lse2) ? 1 : 0, p.cases,
				p.compared, p.finished, p.fault_events,
				static_cast<unsigned long long>(ps.enters));
			require(p.compared > p.cases / 2);
			require(ps.enters > p.compared / 2);
			require(ps.poisoned == 0);
			/* Every library of the tree has its entry table: none was left
			 * to the engine (the blob header's count). */
			require(ps.libraries_refused == 0);
			loop_through_another_range(image, functions);
			code_change_in_the_dispatch_window(image, functions, false);
			code_change_in_the_dispatch_window(image, functions, true);
			edges_into_a_page_that_differs(image);
			edges_into_a_page_that_matches(image);
			next_page_changes_after_its_neighbour_was_compared(image);
			edge_into_another_instance(image);
			atomics_are_shared_between_aot_and_engine(image);
			cas_outcomes_and_permissions(image);
			misaligned_cas_follows_the_profile(image);
			more_instances_than_the_binder_holds(image);
			a_dropped_binder_gives_back_its_page_states(image);
			code_change_history_overflows(image, functions);
			a_changed_bit_anywhere_in_a_page_is_refused(image, functions);
			a_shootdown_reaches_a_native_loop(image);
			memory_copy_and_set_match_the_engine(image);
			memory_copy_and_set_started_at_the_epilogue(image);
			memory_copy_and_set_faults_lie_on_the_stepped_trace(image);
			structure_accesses_over_a_boundary_match_the_engine(image);
			short_entries_are_left_to_the_engine(image);
			admission_history_survives_changes_as_a_hint_only(image);
			a_stepped_fault_hint_survives_changes_as_a_hint_only(image);
			rewritten_code_stays_within_a_stack_budget(image);
			interrupt_flips_leave_the_engine_on_the_stepped_trace(image);
			a_stop_takes_effect_within_one_run_block(image);
			a_breakpoint_published_through_the_bridge_stops_rewritten_code(
				image);
			a_page_without_the_immutable_fact_is_not_run_natively(image);
			the_dispatch_choice_is_made_when_the_executor_is_made(image);
		}
		words_the_engine_rejects_are_left_to_it(image, functions,
			load_steps(cfg.steps, file.index));
		code_synchronization_sites_seen +=
			code_synchronization_in_rewritten_code(image,
				load_steps(cfg.steps, file.index));
		if (file.guest.ends_with("/boot-fixture.oat"))
			direct_branches_below_the_library(image);
		/* The scenarios are about the mechanism, not about the code:
		 * they run on one library, whatever order the build lists. */
		if (file.index == scenario_index) {
			if (file.guest.ends_with("/libz.so")) {
				inline_hit_path_is_taken(image, functions);
				return_loop_is_bounded(image);
				misaligned_sp_at_a_pair_store(image);
				open_exclusive_is_not_entered(image, functions);
				deferred_fp_state_is_judged_by_its_guest_fpcr(image,
					functions);
				equal_digest_is_not_equal_code(image, functions);
			}
			preemption_is_bounded(image, sample);
			two_load_biases(image, sample);
			other_bytes_fall_back(image, sample);
			changed_text_falls_back(image, functions);
			verdicts_are_shared_between_address_spaces(image, functions);
			another_generations_change_reaches_native_code(image, functions);
			refill_keeps_state(image, sample,
				file.guest.ends_with("/libaotfixture.so"));
			partially_mapped_library(image, functions);
			two_vcpus(image, sample);
		}
	}
	/* The release's own libraries hold IC IVAU (the instruction cache flush
	 * of the runtime's JIT); a set with none leaves the row nothing to judge. */
	if (cfg.library < 0 && !cfg.words_only)
		require(code_synchronization_sites_seen != 0);
	if (cfg.code_synchronize_only)
		return 0;
	if (libraries.size() > 1 && cfg.library < 0) {
		/* The scenario library and the one listed after it. */
		std::size_t at = 0;
		while (libraries[at].index != scenario_index)
			++at;
		const auto &first = libraries[at];
		const auto &second = libraries[(at + 1) % libraries.size()];
		const auto image = load_library(first.host);
		const auto other = load_library(second.host);
		cross_library_branches(image, load_functions(cfg.functions,
			first.index), other, load_functions(cfg.functions,
			second.index));
	}
	/* Nothing in the whole run counted a departure that had nothing to count
	 * out. */
	require(::uml::memory::count_anomalies() == 0);
	return 0;
}
