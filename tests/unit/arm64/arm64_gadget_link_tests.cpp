// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_gadget_test_support.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/gadgets/a32/a32_builder.hpp"
#include "cpu/arm64/gadgets/program.hpp"
#include "cpu/gadget_shared_space.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;

extern "C" void uml_arm64_gadget_link_exit();
extern "C" void uml_arm64_gadget_fp_link_entry();

namespace {

using uml::testing::require;
using uml::emulator::tests::address;
using uml::emulator::tests::LinkedBlock;

gadgets::BuildResult build_target(std::uint64_t pc, std::uint16_t value)
{
	const std::array instructions{
		0xd280'0000U | (static_cast<std::uint32_t>(value) << 5),
		0xd400'0001U,
	};
	return gadgets::Builder{}.build(pc, instructions);
}

/* Laid out as the executor's link record: unlinked, it names link_exit,
 * which takes the PC from the link itself, and linked, a copy of a program
 * behind a block header, as the executor stores a block. */
struct LinkTarget final {
	explicit LinkTarget(std::uint64_t pc) noexcept : link{.guest_pc = pc}
	{
		uml::emulator::tests::unlink(link);
	}

	LinkTarget(const LinkTarget &) = delete;
	LinkTarget &operator=(const LinkTarget &) = delete;

	void resolve(const gadgets::BuildResult &target)
	{
		blocks.emplace_back(target.program);
		blocks.back().link(link);
	}

	/* As the executor stores an FP/SIMD block: its stream behind one
	 * fp_link_entry word, which the link names. */
	void resolve_fp(const gadgets::BuildResult &target)
	{
		require(target.program.accesses_fp_state());
		std::vector<std::uintptr_t> words{
			address(uml_arm64_gadget_fp_link_entry)};
		const auto stream = target.program.words();
		words.insert(words.end(), stream.begin(), stream.end());
		blocks.emplace_back(target.program.guest_address(),
			target.program.instruction_count(), words);
		blocks.back().link(link);
	}

	void reset() noexcept { uml::emulator::tests::unlink(link); }

	gadgets::BlockLink link;
	std::vector<LinkedBlock> blocks;
};

std::uint32_t direct_branch(std::uint64_t pc, std::uint64_t target,
			    bool link = false)
{
	const auto displacement =
		static_cast<std::int64_t>(target) - static_cast<std::int64_t>(pc);
	require((displacement & 3) == 0);
	const auto immediate = static_cast<std::uint32_t>(
		static_cast<std::uint64_t>(displacement >> 2) & 0x03ff'ffffU);
	return (link ? 0x9400'0000U : 0x1400'0000U) | immediate;
}

std::uint32_t conditional_branch(std::uint64_t pc, std::uint64_t target,
				 unsigned condition)
{
	const auto displacement =
		static_cast<std::int64_t>(target) - static_cast<std::int64_t>(pc);
	require((displacement & 3) == 0);
	const auto immediate = static_cast<std::uint32_t>(
		static_cast<std::uint64_t>(displacement >> 2) & 0x7ffffU);
	return 0x5400'0000U | (immediate << 5) | (condition & 15U);
}

std::uint32_t compare_branch(std::uint64_t pc, std::uint64_t target,
			     bool nonzero)
{
	const auto displacement =
		static_cast<std::int64_t>(target) - static_cast<std::int64_t>(pc);
	require((displacement & 3) == 0);
	const auto immediate = static_cast<std::uint32_t>(
		static_cast<std::uint64_t>(displacement >> 2) & 0x7ffffU);
	/* CBZ/CBNZ X0. */
	return (nonzero ? 0xb500'0000U : 0xb400'0000U) | (immediate << 5);
}

std::uint32_t test_branch(std::uint64_t pc, std::uint64_t target,
			  bool nonzero, unsigned bit)
{
	const auto displacement =
		static_cast<std::int64_t>(target) - static_cast<std::int64_t>(pc);
	require((displacement & 3) == 0);
	require(bit < 64);
	const auto immediate = static_cast<std::uint32_t>(
		static_cast<std::uint64_t>(displacement >> 2) & 0x3fffU);
	return 0x3600'0000U |
		(static_cast<std::uint32_t>(bit >> 5) << 31) |
		(static_cast<std::uint32_t>(bit & 31U) << 19) |
		(static_cast<std::uint32_t>(nonzero) << 24) |
		(immediate << 5);
}

/* A cell laid out as the executor's LinkRecord, retargeted as it is: its
 * pair written whole with gadget_cache::store_pair, behind a release fence
 * when it names a block, while translated code follows it. */
struct RetargetedRecord final {
	explicit RetargetedRecord(std::uint64_t pc) noexcept : link{.guest_pc = pc}
	{
		uml::emulator::tests::unlink(link);
	}

	RetargetedRecord(const RetargetedRecord &) = delete;
	RetargetedRecord &operator=(const RetargetedRecord &) = delete;

	void reset() noexcept
	{
		uml::cpu::gadget_cache::store_pair(&link.entry,
			address(uml_arm64_gadget_link_exit),
			uml::cpu::gadget_cache::word_of(
				gadgets::unlinked_stream(link)));
	}

	void link_to(const gadgets::BlockLink &block) noexcept
	{
		std::atomic_thread_fence(std::memory_order_release);
		uml::cpu::gadget_cache::store_pair(&link.entry, block.entry,
			uml::cpu::gadget_cache::word_of(block.stream));
	}

	gadgets::BlockLink link;
};

/* The blocks a retargeted record names at its PC, each with a count of its
 * own, and the pairs a writer stores whole: one of theirs, or link_exit with
 * the record's own stream word. */
struct RetargetedBlocks final {
	explicit RetargetedBlocks(std::uint64_t pc)
	{
		for (std::uint32_t index = 0; index < count; ++index) {
			/* `index` NOPs, MOVZ X0, #(100 + index), SVC #0. */
			std::vector<std::uint32_t> instructions(index,
				0xd503'201fU);
			instructions.push_back(0xd280'0000U |
				((100U + index) << 5));
			instructions.push_back(0xd400'0001U);
			const auto built = gadgets::Builder{}.build(pc, instructions);
			blocks.emplace_back(built.program);
			blocks.back().link(views[index]);
			counts[index] = built.program.instruction_count();
		}
	}

	/* Whether `entry` and `stream` are a pair one store wrote, with the
	 * header in front of the stream giving `link`'s PC and the count of the
	 * block it names. */
	[[nodiscard]] bool whole(const gadgets::BlockLink &link,
		std::uint64_t entry, std::uint64_t stream) const noexcept
	{
		const auto header = [&](std::uint64_t &pc, std::uint8_t &count) {
			const auto *bytes = reinterpret_cast<const unsigned char *>(
				stream) - gadgets::block_header_bytes -
				sizeof(std::uintptr_t);
			std::memcpy(&pc, bytes, sizeof(pc));
			count = bytes[gadgets::block_header_count];
		};
		std::uint64_t pc = 0;
		std::uint8_t header_count = 0;
		if (stream == uml::cpu::gadget_cache::word_of(
				gadgets::unlinked_stream(link))) {
			header(pc, header_count);
			return entry == address(uml_arm64_gadget_link_exit) &&
				pc == link.guest_pc && header_count == 0;
		}
		for (std::size_t index = 0; index < count; ++index) {
			if (stream != uml::cpu::gadget_cache::word_of(
					views[index].stream))
				continue;
			header(pc, header_count);
			return entry == views[index].entry && pc == link.guest_pc &&
				header_count == counts[index];
		}
		return false;
	}

	static constexpr std::size_t count = 4;
	std::vector<LinkedBlock> blocks;
	std::array<gadgets::BlockLink, count> views{};
	std::array<std::size_t, count> counts{};
};

/* Translated code follows a record that other threads retarget between the
 * blocks at its PC and its exit, through the production pair writer: every
 * pair a reader loads is one a writer stored, and every run of a branch to
 * the record ends as one of the blocks or the exit says. */
void retargeted_record_follow()
{
	constexpr std::uint64_t source = 0x4000;
	constexpr std::uint64_t pc = 0x4040;
	RetargetedRecord record{pc};
	const RetargetedBlocks targets{pc};

	/* The check tells a pair torn between two stores from a whole one. */
	require(targets.views[0].entry != targets.views[1].entry);
	for (std::size_t index = 0; index < RetargetedBlocks::count; ++index)
		require(targets.whole(record.link, targets.views[index].entry,
			uml::cpu::gadget_cache::word_of(
				targets.views[index].stream)));
	require(targets.whole(record.link, address(uml_arm64_gadget_link_exit),
		uml::cpu::gadget_cache::word_of(
			gadgets::unlinked_stream(record.link))));
	require(!targets.whole(record.link, targets.views[0].entry,
		uml::cpu::gadget_cache::word_of(targets.views[1].stream)));
	require(!targets.whole(record.link, address(uml_arm64_gadget_link_exit),
		uml::cpu::gadget_cache::word_of(targets.views[0].stream)));
	require(!targets.whole(record.link, targets.views[0].entry,
		uml::cpu::gadget_cache::word_of(
			gadgets::unlinked_stream(record.link))));

	const std::array instructions{direct_branch(source, pc)};
	const auto branch = gadgets::Builder{}.build(source, instructions,
		[&](std::uint64_t) -> const gadgets::BlockLink * {
			return &record.link;
		});

	std::atomic<bool> stop{false};
	std::atomic<unsigned> writing{0};
	std::atomic<std::uint64_t> torn{0};
	std::atomic<std::uint64_t> wrong{0};
	std::vector<std::thread> threads;
	for (unsigned writer = 0; writer < 2; ++writer)
		threads.emplace_back([&, writer] {
			writing.fetch_add(1);
			for (std::size_t step = writer; !stop.load(
				     std::memory_order_relaxed); step += 3) {
				const auto state = step % (RetargetedBlocks::count + 1);
				if (state == RetargetedBlocks::count)
					record.reset();
				else
					record.link_to(targets.views[state]);
			}
		});
	for (unsigned reader = 0; reader < 2; ++reader)
		threads.emplace_back([&] {
			while (!stop.load(std::memory_order_relaxed)) {
				std::uint64_t entry = 0;
				std::uint64_t stream = 0;
				uml::cpu::gadget_cache::load_pair(&record.link.entry,
					entry, stream);
				if (!targets.whole(record.link, entry, stream))
					torn.fetch_add(1);
			}
		});
	while (writing.load() != 2)
		std::this_thread::yield();

	/* What a run of the branch ends as: the exit at the record's PC, or
	 * block `index`'s system call. */
	std::array<std::uint64_t, RetargetedBlocks::count + 1> seen{};
	::uml::memory::Tlb tlb;
	std::uint64_t runs = 0;
	constexpr std::uint64_t enough = 200'000;
	constexpr std::uint64_t most = 20'000'000;
	const auto outcomes = [&] {
		return static_cast<std::size_t>(std::ranges::count_if(seen,
			[](std::uint64_t times) { return times != 0; }));
	};
	for (; runs < most && (runs < enough || outcomes() < 2); ++runs) {
		arm64::CpuState cpu{};
		std::size_t executed = 0;
		const auto event = gadgets::execute(branch.program, cpu, tlb,
			executed);
		if (event == ::uml::cpu::EngineEvent::continued) {
			if (cpu.pc == pc && executed == 1)
				++seen[RetargetedBlocks::count];
			else
				wrong.fetch_add(1);
			continue;
		}
		const auto index = cpu.registers[0] - 100;
		if (event == ::uml::cpu::EngineEvent::system_call &&
		    index < RetargetedBlocks::count &&
		    cpu.pc == pc + 4 * targets.counts[index] &&
		    executed == 1 + targets.counts[index])
			++seen[index];
		else
			wrong.fetch_add(1);
	}
	stop.store(true);
	for (auto &thread : threads)
		thread.join();
	require(torn.load() == 0);
	require(wrong.load() == 0);
	require(runs < most);
}

/* An AArch32 or T32 block at `key`, whose direct branch, if any, names
 * `link`. */
gadgets::BuildResult build_aarch32(std::uint64_t key,
	std::span<const std::byte> code, const gadgets::BlockLink *link)
{
	gadgets::Builder::LinkResolver resolve;
	if (link != nullptr)
		resolve = [link](std::uint64_t pc) -> const gadgets::BlockLink * {
			require(pc == link->guest_pc);
			return link;
		};
	const gadgets::BuildOptions options{};
	auto output = gadgets::a32::build({.key = key, .code = code}, resolve,
		std::pmr::get_default_resource(), {}, options,
		gadgets::a32::decode_profile(options), gadgets::a32::block_limit);
	require(output.units != 0);
	return std::move(output.result);
}

/* `program` with a count past the chain limit, as a caller that precharges
 * it enters it: it leaves at its first linked edge. */
gadgets::Program precharged(const gadgets::Program &program)
{
	gadgets::ProgramWords words{std::pmr::get_default_resource()};
	for (const auto word : program.words())
		words.push_back(word);
	return gadgets::Program{program.guest_address(), 8192, std::move(words),
		false};
}

/* A chain-limit exit leaves at the body a link names without running it:
 * it stores the PC in that body's header, an AArch32 or T32 key whole, and
 * charges the body's count, and behind an FP/SIMD body's entry word it
 * reads them as behind any other's and leaves the FP/SIMD state alone. */
void chain_limit_exits()
{
	namespace aarch32 = arm64::aarch32;
	const auto leave = [](const gadgets::Program &source,
				   arm64::CpuState &cpu) {
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(precharged(source), cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::continued);
		return executed;
	};
	{
		/* B to MOV R0, #1; SVC #0. */
		const std::array<std::uint32_t, 2> body{0xe3a0'0001U, 0xef00'0000U};
		const std::array<std::uint32_t, 1> branch{0xea00'000eU};
		LinkTarget target{aarch32::a32_key(0x1040)};
		const auto built = build_aarch32(target.link.guest_pc,
			std::as_bytes(std::span{body}), nullptr);
		require(built.program.instruction_count() == 2);
		target.resolve(built);
		const auto source = build_aarch32(aarch32::a32_key(0x1000),
			std::as_bytes(std::span{branch}), &target.link);
		arm64::CpuState cpu{};
		cpu.a32 = 1;
		cpu.a32_cp15ben = 1;
		cpu.pc = aarch32::a32_key(0x1000);
		require(leave(source.program, cpu) == 8192 + 2);
		require(cpu.pc == aarch32::a32_key(0x1040));
		require(cpu.registers[0] == 0);
	}
	{
		/* B to MOVS R0, #1; SVC #0. */
		const std::array<std::uint16_t, 2> body{0x2001U, 0xdf00U};
		const std::array<std::uint16_t, 1> branch{0xe01eU};
		LinkTarget target{aarch32::t32_key(0x2040)};
		const auto built = build_aarch32(target.link.guest_pc,
			std::as_bytes(std::span{body}), nullptr);
		require(built.program.instruction_count() == 2);
		target.resolve(built);
		const auto source = build_aarch32(aarch32::t32_key(0x2000),
			std::as_bytes(std::span{branch}), &target.link);
		arm64::CpuState cpu{};
		cpu.a32 = 1;
		cpu.a32_cp15ben = 1;
		cpu.pc = aarch32::t32_key(0x2000);
		require(leave(source.program, cpu) == 8192 + 2);
		require(cpu.pc == aarch32::t32_key(0x2040));
		require(cpu.registers[0] == 0);
	}
	{
		/* B to FMOV X0, D0; SVC #0, with the FP/SIMD state loaded or
		 * not. */
		const std::array body{0x9e66'0000U, 0xd400'0001U};
		LinkTarget target{0x4040};
		const auto built = gadgets::Builder{}.build(0x4040, body);
		require(built.program.instruction_count() == 2);
		target.resolve_fp(built);
		const std::array branch{direct_branch(0x4000, 0x4040)};
		const auto source = gadgets::Builder{}.build(0x4000, branch,
			[&](std::uint64_t) { return &target.link; });
		for (const bool loaded : {true, false}) {
			arm64::CpuState cpu{};
			cpu.fp_loaded = loaded;
			require(leave(source.program, cpu) == 8192 + 2);
			require(cpu.pc == 0x4040);
			require(cpu.fp_loaded == loaded && !cpu.fp_dirty);
			require(cpu.registers[0] == 0);
		}
	}
}

} // namespace

int main()
{
	/* An unresolved direct edge selects the data-only fallback stream and
	 * returns the target PC without executing arbitrary memory. */
	{
		LinkTarget target{0x4040};
		const std::array instructions{
			direct_branch(0x4000, target.link.guest_pc)};
		auto built = gadgets::Builder{}.build(
			0x4000, instructions,
			[&](std::uint64_t pc) -> const gadgets::BlockLink * {
				require(pc == target.link.guest_pc);
				return &target.link;
			});
		arm64::CpuState cpu{};
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(built.program, cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::continued);
		require(cpu.pc == 0x4040);
		require(executed == 1);
	}

	/* A resolved edge enters the successor stream without returning through
	 * C++; its terminal event and combined instruction count are preserved. */
	{
		auto successor = build_target(0x4040, 42);
		LinkTarget target{0x4040};
		target.resolve(successor);
		const std::array instructions{
			direct_branch(0x4000, target.link.guest_pc)};
		auto first = gadgets::Builder{}.build(
			0x4000, instructions,
			[&](std::uint64_t) { return &target.link; });
		arm64::CpuState cpu{};
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(first.program, cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::system_call);
		require(cpu.registers[0] == 42);
		require(cpu.pc == 0x4048);
		require(executed == 3);
	}

	/* An FP/SIMD target owns a translation-selected entry prefix. The hot
	 * loaded case marks the frame dirty without C++, while a fresh deferred
	 * kernel frame is materialized exactly before the linked target. */
	{
		const std::array target_instructions{
			0x9e66'0000U, // fmov x0, d0
			0xd400'0001U, // svc #0
		};
		auto successor = gadgets::Builder{}.build(
			0x4040, target_instructions);
		require(successor.program.accesses_fp_state());
		LinkTarget target{0x4040};
		target.resolve_fp(successor);
		const std::array instructions{
			direct_branch(0x4000, target.link.guest_pc)};
		auto first = gadgets::Builder{}.build(
			0x4000, instructions,
			[&](std::uint64_t) { return &target.link; });
		::uml::memory::Tlb tlb;

		const std::uint64_t loaded_value =
			0x0123'4567'89ab'cdefULL;
		arm64::CpuState loaded{};
		std::memcpy(loaded.vectors[0].bytes.data(), &loaded_value,
			sizeof(loaded_value));
		std::size_t executed = 0;
		require(gadgets::execute(first.program, loaded, tlb, executed) ==
			::uml::cpu::EngineEvent::system_call);
		require(loaded.registers[0] == loaded_value);
		require(loaded.fp_loaded && loaded.fp_dirty);
		require(loaded.pc == 0x4048);
		require(executed == 3);

		const std::uint64_t deferred_value =
			0xfedc'ba98'7654'3210ULL;
		std::array<arm64::VectorRegister, 32> deferred{};
		std::memcpy(deferred[0].bytes.data(), &deferred_value,
			sizeof(deferred_value));
		arm64::CpuState lazy{};
		lazy.defer_fp_load(deferred.data(), 0x0040'0000U, 0x0800'0010U,
			arm64::fpcr_writable_mask);
		executed = 0;
		require(gadgets::execute(first.program, lazy, tlb, executed) ==
			::uml::cpu::EngineEvent::system_call);
		require(lazy.registers[0] == deferred_value);
		require(lazy.fpcr == 0x0040'0000U);
		require(lazy.fpsr == 0x0800'0010U);
		require(lazy.fp_loaded && lazy.fp_dirty);
		require(lazy.pc == 0x4048);
		require(executed == 3);
	}

	/* Materializing a lazy frame does not hide a fault in the linked FP/SIMD
	 * target. The architectural PC remains at the faulting vector load. */
	{
		const std::array target_instructions{
			0x3dc0'0000U, // ldr q0, [x0]
			0xd400'0001U, // svc #0
		};
		auto successor = gadgets::Builder{}.build(
			0x4040, target_instructions);
		require(successor.program.accesses_fp_state());
		LinkTarget target{0x4040};
		target.resolve_fp(successor);
		const std::array instructions{
			direct_branch(0x4000, target.link.guest_pc)};
		auto first = gadgets::Builder{}.build(
			0x4000, instructions,
			[&](std::uint64_t) { return &target.link; });

		std::array<arm64::VectorRegister, 32> deferred{};
		arm64::CpuState cpu{};
		cpu.defer_fp_load(deferred.data(), 0, 0,
			arm64::fpcr_writable_mask);
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(first.program, cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::memory_fault);
		require(cpu.fp_loaded && cpu.fp_dirty);
		/* The vector load reports its position in the successor, as every
		 * transfer without a PC word does. */
		gadgets::resolve_fault_position(successor.program, cpu.pc);
		require(cpu.pc == target.link.guest_pc);
		require(executed == 3);
	}

	/* B.cond selects target links directly. Exercise both NZCV outcomes so a
	 * raw guest address can never be mistaken for a host stream pointer. */
	for (const bool taken : {false, true}) {
		auto branch_target = build_target(0x4040, 7);
		auto fallthrough = build_target(0x4004, 9);
		LinkTarget target{0x4040};
		LinkTarget next{0x4004};
		target.resolve(branch_target);
		next.resolve(fallthrough);
		const std::array instructions{
			conditional_branch(0x4000, target.link.guest_pc, 0)};
		auto first = gadgets::Builder{}.build(
			0x4000, instructions,
			[&](std::uint64_t pc) {
				return pc == target.link.guest_pc ?
					&target.link : &next.link;
			});
		arm64::CpuState cpu{};
		cpu.nzcv = taken ? 0x4000'0000U : 0;
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(first.program, cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::system_call);
		require(cpu.registers[0] == (taken ? 7U : 9U));
		require(executed == 3);
	}

	/* BL preserves its fixed return address while using the same deferred-PC
	 * linked edge as B. The return address is the return block's link's own,
	 * so that address resolves to a record of its own, as in the executor. */
	{
		auto successor = build_target(0x4040, 17);
		LinkTarget target{0x4040};
		target.resolve(successor);
		LinkTarget return_site{0x4004};
		const std::array instructions{
			direct_branch(0x4000, target.link.guest_pc, true)};
		auto first = gadgets::Builder{}.build(
			0x4000, instructions,
			[&](std::uint64_t pc) {
				return pc == target.link.guest_pc ?
					&target.link : &return_site.link;
			});
		arm64::CpuState cpu{};
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(first.program, cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::system_call);
		require(cpu.registers[30] == 0x4004);
		require(cpu.registers[0] == 17);
		require(cpu.pc == 0x4048);
		require(executed == 3);
	}

	/* Both architecturally-always B.cond encodings take the target without
	 * reading NZCV. */
	for (const unsigned condition : {14U, 15U}) {
		auto successor = build_target(0x4040, 18 + condition);
		LinkTarget target{0x4040};
		LinkTarget fallthrough{0x4004};
		target.resolve(successor);
		const std::array instructions{
			conditional_branch(0x4000, target.link.guest_pc, condition)};
		auto first = gadgets::Builder{}.build(
			0x4000, instructions,
			[&](std::uint64_t pc) {
				return pc == target.link.guest_pc ?
					&target.link : &fallthrough.link;
			});
		arm64::CpuState cpu{};
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(first.program, cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::system_call);
		require(cpu.registers[0] == 18 + condition);
		require(executed == 3);
	}

	/* CBZ/CBNZ and TBZ/TBNZ each select the correct linked successor for both
	 * outcomes. */
	for (const bool nonzero : {false, true}) {
		for (const bool condition_true : {false, true}) {
			auto taken_target = build_target(0x4040, 31);
			auto not_taken_target = build_target(0x4004, 32);
			LinkTarget target{0x4040};
			LinkTarget fallthrough{0x4004};
			target.resolve(taken_target);
			fallthrough.resolve(not_taken_target);
			const std::array instructions{
				compare_branch(0x4000, target.link.guest_pc,
					nonzero)};
			auto first = gadgets::Builder{}.build(
				0x4000, instructions,
				[&](std::uint64_t pc) {
					return pc == target.link.guest_pc ?
						&target.link : &fallthrough.link;
				});
			arm64::CpuState cpu{};
			cpu.registers[0] =
				condition_true == nonzero ? 1U : 0U;
			::uml::memory::Tlb tlb;
			std::size_t executed = 0;
			require(gadgets::execute(first.program, cpu, tlb, executed) ==
				::uml::cpu::EngineEvent::system_call);
			require(cpu.registers[0] ==
				(condition_true ? 31U : 32U));
			require(executed == 3);
		}

		for (const bool condition_true : {false, true}) {
			auto taken_target = build_target(0x4040, 41);
			auto not_taken_target = build_target(0x4004, 42);
			LinkTarget target{0x4040};
			LinkTarget fallthrough{0x4004};
			target.resolve(taken_target);
			fallthrough.resolve(not_taken_target);
			const std::array instructions{
				test_branch(0x4000, target.link.guest_pc,
					nonzero, 5)};
			auto first = gadgets::Builder{}.build(
				0x4000, instructions,
				[&](std::uint64_t pc) {
					return pc == target.link.guest_pc ?
						&target.link : &fallthrough.link;
				});
			arm64::CpuState cpu{};
			cpu.registers[0] =
				condition_true == nonzero ? (1U << 5) : 0U;
			::uml::memory::Tlb tlb;
			std::size_t executed = 0;
			require(gadgets::execute(first.program, cpu, tlb, executed) ==
				::uml::cpu::EngineEvent::system_call);
			require(cpu.registers[0] ==
				(condition_true ? 41U : 42U));
			require(executed == 3);
		}
	}

	/* A sequential block trailer links to its translated fallthrough too. */
	{
		auto successor = build_target(0x4004, 51);
		LinkTarget fallthrough{0x4004};
		fallthrough.resolve(successor);
		const std::array instructions{0xd503'201fU}; // nop
		auto first = gadgets::Builder{}.build(
			0x4000, instructions,
			[&](std::uint64_t pc) {
				require(pc == fallthrough.link.guest_pc);
				return &fallthrough.link;
			});
		arm64::CpuState cpu{};
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(first.program, cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::system_call);
		require(cpu.registers[0] == 51);
		require(cpu.pc == 0x400c);
		require(executed == 3);
	}

	/* A live link can be retargeted and then reset to its private fallback
	 * stream without rebuilding its predecessor. */
	{
		auto first_target = build_target(0x4040, 61);
		auto second_target = build_target(0x4080, 62);
		LinkTarget target{0x4040};
		target.resolve(first_target);
		const std::array instructions{
			direct_branch(0x4000, target.link.guest_pc)};
		auto predecessor = gadgets::Builder{}.build(
			0x4000, instructions,
			[&](std::uint64_t) { return &target.link; });
		::uml::memory::Tlb tlb;

		for (const auto expected : {61U, 62U}) {
			arm64::CpuState cpu{};
			std::size_t executed = 0;
			require(gadgets::execute(
				predecessor.program, cpu, tlb, executed) ==
				::uml::cpu::EngineEvent::system_call);
			require(cpu.registers[0] == expected);
			require(executed == 3);
			target.resolve(second_target);
		}

		target.reset();
		arm64::CpuState cpu{};
		std::size_t executed = 0;
		require(gadgets::execute(
			predecessor.program, cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::continued);
		require(cpu.pc == target.link.guest_pc);
		require(executed == 1);
	}

	/* A self-linked hot loop is forced out at the 4K chain boundary so the
	 * executor can observe interrupts and enforce its 64K budget. */
	{
		LinkTarget self{0x4000};
		const std::array instructions{0x1400'0000U}; // b .
		auto loop = gadgets::Builder{}.build(
			0x4000, instructions,
			[&](std::uint64_t) { return &self.link; });
		self.resolve(loop);
		arm64::CpuState cpu{};
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(loop.program, cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::continued);
		require(cpu.pc == 0x4000);
		require(executed == 4096);
	}

	/* A caller that precharges the counter above the chain limit must leave at
	 * the first linked edge and retain its exact count. This also covers values
	 * above 8,191, where testing bit 12 alone would wrap back to "below". */
	{
		LinkTarget self{0x4000};
		const std::array instructions{0x1400'0000U}; // b .
		auto built = gadgets::Builder{}.build(
			0x4000, instructions,
			[&](std::uint64_t) { return &self.link; });
		gadgets::ProgramWords words{std::pmr::get_default_resource()};
		for (const auto word : built.program.words())
			words.push_back(word);
		gadgets::Program precharged{
			0x4000, 8192, std::move(words), false};
		self.resolve(built);
		arm64::CpuState cpu{};
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		require(gadgets::execute(precharged, cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::continued);
		require(cpu.pc == 0x4000);
		require(executed == 8193);
	}

	chain_limit_exits();
	retargeted_record_follow();

	return 0;
}
