// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/gadgets/a32_layout.h"
#include "cpu/arm64/gadgets/execution_histogram.hpp"
#include "cpu/arm64/gadgets/guest_sampler.hpp"
#include "cpu/arm64/gadgets/program.hpp"
#include "cpu/arm64/gadgets/tlb.hpp"
#include "memory/static_guest_memory.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace uml::cpu::gadget_cache {

/* Defined in cpu/gadget_shared_space.hpp: what an executor says about whether
 * it may be running translated code, which executors sharing an address space
 * read before they free a body. */
struct ExecutorAnnouncement;

} // namespace uml::cpu::gadget_cache

namespace uml::cpu::arm64::gadgets {

struct GadgetCacheDiagnostics final {
	/* Programs this executor had to produce. */
	std::uint64_t translations{};
	std::uint64_t validation_attempts{};
	std::uint64_t validation_hits{};
	std::uint64_t validation_mismatches{};
	std::uint64_t validation_faults{};
	std::uint64_t validation_sequence_races{};
	/* Translations another executor of the address space stored, which
	 * this one took after validating them instead of translating. */
	std::uint64_t adoptions{};
	/* Sweeps of an address space's link records and indirect sites, and
	 * the records and sites they released. */
	std::uint64_t link_sweeps{};
	std::uint64_t released_links{};
	std::uint64_t released_sites{};
#if UML_GADGET_EXECUTION_HISTOGRAM
	/* Why translated execution returned to this executor, per block entry:
	 * the chain reached its linked-instruction limit, an indirect site missed
	 * both of its cells, an unlinked or exit gadget left with the PC set, or
	 * a guest-visible event (syscall, fault, interrupt, other) ended it. */
	std::uint64_t block_entries{};
	std::uint64_t exits_chain_limit{};
	std::uint64_t exits_indirect_miss{};
	std::uint64_t exits_indirect_miss_return{};
	/* Misses at sites with 1, 2, 3, 4, 5-8, 9-16, 17-64, >64 distinct
	 * targets, refreshed by the executor on each miss. */
	std::array<std::uint64_t, 8> miss_by_site_targets{};
	std::uint64_t exits_unlinked{};
	std::uint64_t exits_syscall{};
	std::uint64_t exits_fault{};
	std::uint64_t exits_interrupt{};
	std::uint64_t exits_other{};
	std::uint64_t cache_misses{};
#endif
};

class GadgetExecutor final : diagnostics::SampledExecutor {
public:
	explicit GadgetExecutor(
		BuildOptions build_options = {});
	~GadgetExecutor();
	GadgetExecutor(const GadgetExecutor &) = delete;
	GadgetExecutor &operator=(const GadgetExecutor &) = delete;
	[[nodiscard]] bool begin_execution(unsigned long generation,
					   std::uint64_t guest_pc);
	void end_execution() noexcept;

	[[nodiscard]] ::uml::cpu::EngineEvent run_block(
		CpuState &cpu, ::uml::memory::StaticGuestMemory &memory,
		unsigned long generation, std::size_t &executed);
	/* run_block with the ahead-of-time step (cpu/arm64/aot) in front, for
	 * an engine whose aot_selected() is true; it is the engine that picks
	 * the one or the other, once per entry. Defined only with UML_AOT. */
	[[nodiscard]] ::uml::cpu::EngineEvent run_aot_block(
		CpuState &cpu, ::uml::memory::StaticGuestMemory &memory,
		unsigned long generation, std::size_t &executed);
	/* Whether the ahead-of-time component was on when this executor was
	 * made (DARWIN_UML_AOT): fixed for its life, false in a build without
	 * the component. */
	[[nodiscard]] bool aot_selected() const noexcept
	{
		return aot_selected_;
	}
	/* run_block for an AArch32 task (a32/a32_builder.hpp): cpu.pc is an
	 * A32 or T32 key (aarch32/guest_key.hpp) and every exit leaves it one.
	 * Retirement, fault positions and ITSTATE follow the contract in
	 * aarch32/semantics.hpp; a breakpoint exit leaves BKPT's immediate in
	 * breakpoint_immediate(). */
	[[nodiscard]] ::uml::cpu::EngineEvent run_a32_block(
		CpuState &cpu, ::uml::memory::StaticGuestMemory &memory,
		unsigned long generation, std::size_t &executed);
	/* The start of an AArch32 entry, before its first run_a32_block: the
	 * scratch slots the streams borrow hold what the virtual CPU ran
	 * before, so they start at zero (a32_layout.h). */
	static void enter_aarch32(CpuState &cpu) noexcept
	{
		cpu.registers[UML_GADGET_A32_SCRATCH0_SLOT] = 0;
		cpu.registers[UML_GADGET_A32_SCRATCH1_SLOT] = 0;
	}
	[[nodiscard]] std::uint32_t breakpoint_immediate() const noexcept
	{
		return breakpoint_immediate_;
	}
	[[nodiscard]] bool preempted() const noexcept { return preempted_; }
	/* Whether the gadget-cache budget or another executor asked this one to
	 * let go of an address space it holds (Space::request_renewal) since it
	 * last did. It does at the start of its next run; a run in progress ends
	 * at its next return from translated code so that the next starts now,
	 * and the space is not held, or grown, for the rest of the dispatch.
	 * False while a let-go ran out of memory: the ask stands, and runs go on
	 * as they did, so that none ends after every block. One relaxed load of
	 * a flag on this executor's own announcement. */
	[[nodiscard]] bool renewal_pending() const noexcept
	{
		return renew_ != nullptr && !renewal_stuck_ &&
			renew_->load(std::memory_order_relaxed);
	}
	/* While set, every run_block retires at most one guest instruction. */
	void set_single_step(bool enabled) noexcept { single_step_ = enabled; }

	[[nodiscard]] std::uint64_t fault_address() const noexcept;
	[[nodiscard]] ::uml::memory::Access fault_access() const noexcept;
	[[nodiscard]] bool fault_alignment() const noexcept;
	[[nodiscard]] std::uint32_t floating_point_syndrome() const noexcept;
	[[nodiscard]] const GadgetCacheDiagnostics &cache_diagnostics() const noexcept
	{
		return cache_diagnostics_;
	}

private:
	[[nodiscard]] bool sample_block(const void *stream,
		diagnostics::SampledBlock &block) const noexcept override;

	/* A cached translation is handed out as a BuildResult whose program
	 * borrows the cache's words, so it is valid until the next store. */
	void prepare_cache(unsigned long generation, std::uint64_t changes);
	/* Before entering translated code: let go of the address spaces
	 * another executor closed, and of those whose generation the MMU has
	 * destroyed (`retirements` is its count of destroyed generations). */
	void leave_gone_spaces(std::uint64_t retirements) noexcept;
	/* Tell the address space `generation` is bound to, in each cache that
	 * holds it, that an executor entered it, for the gadget-cache budget to
	 * find the spaces the guest runs least (gadget_cache_budget.hpp). Once
	 * per entry. */
	void note_run(unsigned long generation) noexcept;
	void select_cpu(CpuState &cpu) noexcept;
	[[nodiscard]] std::optional<BuildResult> find(
		std::uint64_t pc, std::uint64_t changes,
		::uml::memory::StaticGuestMemory &memory);
	[[nodiscard]] BuildResult translate(
		std::uint64_t pc, std::span<const std::uint32_t> window);
	[[nodiscard]] void *execution_context(unsigned long generation);
	[[nodiscard]] ::uml::cpu::EngineEvent run_trap_enabled_instruction(
		CpuState &cpu, ::uml::memory::StaticGuestMemory &memory,
		std::size_t &executed);

	class ProgramCache;

	/* The ahead-of-time component's state (cpu/arm64/aot), present only in
	 * a build with an AOT framework; the member exists in every build so
	 * that the layout does not depend on the option. */
	struct AotState;
	/* The body of run_block and run_aot_block: with Aot, the AOT step runs
	 * first. */
	template<bool Aot>
	[[nodiscard]] ::uml::cpu::EngineEvent run_block_body(
		CpuState &cpu, ::uml::memory::StaticGuestMemory &memory,
		unsigned long generation, std::size_t &executed);
	/* run_block_body's AOT step: when cpu.pc has rewritten code, runs it and
	 * returns the event the run ends with, else nothing and the engine
	 * runs the block as usual. Defined only with UML_AOT. */
	[[nodiscard]] std::optional<::uml::cpu::EngineEvent> run_aot(
		CpuState &cpu, ::uml::memory::StaticGuestMemory &memory,
		unsigned long generation, std::uint64_t code_changes,
		std::uint64_t mapping_changes, std::size_t &executed);
	/* run_aot's engine step: the one instruction rewritten code left to the
	 * engine, as run_trap_enabled_instruction runs it, from a cached
	 * one-instruction program. Defined only with UML_AOT. */
	[[nodiscard]] ::uml::cpu::EngineEvent step_aot(CpuState &cpu,
		::uml::memory::StaticGuestMemory &memory, std::size_t &executed);

	/* run_block for a guest SVCR other than 0, out of line so that the
	 * mode-0 path stays as it was. */
	[[nodiscard]] ::uml::cpu::EngineEvent run_sme_block(
		CpuState &cpu, ::uml::memory::StaticGuestMemory &memory,
		unsigned long generation, std::uint64_t code_changes,
		std::size_t &executed);
	/* What both paths do once a program has run: correct a terminal
	 * exit's retirement count and train the indirect site it missed. */
	[[nodiscard]] ::uml::cpu::EngineEvent finish_block(
		ProgramCache &programs, const BuildResult &built,
		::uml::cpu::EngineEvent event, CpuState &cpu,
		std::size_t &executed);

	/* What both AArch32 paths do once a program has run: the A64 correction
	 * of a terminal exit's retirement, made in guest units a T32 block's
	 * instructions do not fill, ITSTATE for a T32 exit, and the indirect
	 * site's training. */
	[[nodiscard]] ::uml::cpu::EngineEvent finish_a32_block(
		ProgramCache &programs, ::uml::cpu::EngineEvent event,
		CpuState &cpu, ::uml::memory::StaticGuestMemory &memory,
		std::size_t &executed);
	[[nodiscard]] ::uml::cpu::EngineEvent run_a32_instruction(
		CpuState &cpu, ::uml::memory::StaticGuestMemory &memory,
		std::size_t &executed);
	[[nodiscard]] ::uml::cpu::EngineEvent run_a32_translation(
		CpuState &cpu, ::uml::memory::StaticGuestMemory &memory,
		unsigned long generation, std::uint64_t code_changes,
		std::size_t &executed);

	unsigned long cache_generation_{};
	std::uint64_t cache_changes_{};
	unsigned long mapping_generation_{};
	std::uint64_t mapping_changes_{};
	std::unique_ptr<ProgramCache> programs_;
	::uml::memory::Tlb tlb_;
	CpuState *current_cpu_{};
	void *execution_context_{};
	unsigned long execution_generation_{};
	/* The execution context's code-change sequence, which every run
	 * publishes in CpuState::code_sequence for the ISB to check. */
	const std::uint64_t *code_sequence_{&arm64::no_code_changes};
	bool preempted_{};
	bool single_step_{};
	std::uint32_t floating_point_syndrome_{};
	BuildOptions build_options_;
	GadgetCacheDiagnostics cache_diagnostics_{};
	/* Null unless DARWIN_UML_GUEST_SAMPLES is set. */
	diagnostics::GuestSamplingSlot *sampling_{};
	/* The translations of SVCR modes 1-3 (SM, ZA, both), each mode in a
	 * cache of its own made on first use; programs_ holds mode 0. A
	 * translation's legality depends on its mode, and keeping each mode's
	 * links, indirect sites and target table apart means no program can
	 * reach another mode's code. */
	struct SmeModeCache final {
		std::unique_ptr<ProgramCache> programs;
		unsigned long generation{};
		std::uint64_t changes{};
	};
	std::array<SmeModeCache, 3> sme_caches_{};
	/* BKPT's immediate, after the last A64 member so that none moves. */
	std::uint32_t breakpoint_immediate_{};
	/* Null when the executor keeps its translations to itself. */
	gadget_cache::ExecutorAnnouncement *announcement_{};
	/* The announcement's renewal flag, and whether the let-go it asks for
	 * failed for want of memory and stands (renewal_pending()). */
	const std::atomic<bool> *renew_{};
	bool renewal_stuck_{};
	bool share_translations_{};
	/* The MMU's count of destroyed generations when this executor last
	 * looked for address spaces to let go of. */
	std::uint64_t retirements_seen_{};
	/* Whether any SVCR mode cache was made, which note_run() then visits. */
	bool sme_caches_made_{};
	/* The ahead-of-time component's state and whether this executor runs
	 * blocks through it (cpu/arm64/aot), after every member of the ordinary
	 * path so that none moves; present in every build so that the layout
	 * does not depend on the option. */
	std::unique_ptr<AotState> aot_;
	bool aot_selected_{};
};

#if defined(UML_GADGET_SWEEP_STRESS) && UML_GADGET_SWEEP_STRESS
/* Hooks of the unit-test variant of the gadget executor
 * (uml_arm64_gadget_sweep_stress_executor), which tests link in place of the
 * production one. They are defined in executor.cpp, under the same macro. */
namespace stress {

/* Address spaces alive now, private and closed ones included, and how many
 * shared ones have been closed. */
[[nodiscard]] std::size_t live_spaces() noexcept;
[[nodiscard]] std::uint64_t closes() noexcept;
/* Bodies a slot's release left in no slot's hands, so that a space queued
 * them to free: what a space's last holder frees after it. A holder that lets
 * go of a space nobody will run again does not add to it. */
[[nodiscard]] std::uint64_t limbo_pushes() noexcept;
/* Renew a shared address space on every Nth body stored in it, 0 for never;
 * the variant starts at every 64th. */
void set_renewal_period(unsigned stores) noexcept;
/* Freed arena bytes a shared address space needs before it is renewed for
 * holding more of them than its bodies use; production uses two mebibytes. */
void set_renewal_floor(std::size_t bytes) noexcept;
/* Close the open shared address spaces of one guest address space, as a
 * store that found them mostly freed arena would; how many. The caller is
 * the only thread that can destroy them. */
std::size_t close_spaces(unsigned long generation);
struct SpaceBytes final {
	std::size_t arena;
	std::size_t held;
	std::size_t spaces;
};
/* What the open shared spaces of a guest address space hold: arena bytes,
 * and the bytes of the bodies some slot holds. */
[[nodiscard]] SpaceBytes open_space_bytes(unsigned long generation);

} // namespace stress
#endif

} // namespace uml::cpu::arm64::gadgets
