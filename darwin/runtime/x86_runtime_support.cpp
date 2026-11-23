// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/x86_runtime_support.hpp"

#if DARWIN_UML_X86_64_GUEST

#include "cpu/x86_64/code_page_registry.hpp"
#include "cpu/x86_64/decoder.hpp"
#include "memory/mmu/frame_accounting.hpp"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

namespace uml::runtime {

namespace x86_64 = ::uml::cpu::x86_64;
using ::uml::memory::StaticGuestMemory;

namespace {

class MmSharedLog final {
public:
	MmSharedLog()
	{
		const char *path = std::getenv("UML_X86_MM_SHARED_LOG");
		if (path != nullptr && path[0] != '\0')
			file_ = std::fopen(path, "ae");
	}

	[[nodiscard]] bool enabled() const noexcept { return file_ != nullptr; }

	~MmSharedLog()
	{
		if (file_ != nullptr)
			std::fclose(file_);
	}

	MmSharedLog(const MmSharedLog &) = delete;
	MmSharedLog &operator=(const MmSharedLog &) = delete;

	void note(unsigned long generation, bool shared, std::uint64_t rip) noexcept
	{
		if (file_ == nullptr)
			return;
		const std::lock_guard lock{mutex_};
		try {
			/* A generation starts unshared. */
			auto [entry, added] = shared_.try_emplace(generation, false);
			if (entry->second == shared)
				return;
			entry->second = shared;
			(void)added;
		} catch (...) {
			return;
		}
		std::fprintf(file_, "uml-x86 mm-shared generation=%lu shared=%d rip=%#llx\n",
			generation, shared ? 1 : 0, static_cast<unsigned long long>(rip));
		std::fflush(file_);
	}

	void note_jit(unsigned long generation, bool marked, bool ordered,
		std::uint64_t rip) noexcept
	{
		if (file_ == nullptr)
			return;
		const std::lock_guard lock{mutex_};
		if (marked && !ordered) {
			/* Unordered code while the kernel marks the space shared:
			 * always logged, and always a failure. */
			std::fprintf(file_,
				"uml-x86 jit-unordered-shared generation=%lu rip=%#llx\n",
				generation, static_cast<unsigned long long>(rip));
			std::fflush(file_);
		}
		try {
			auto [entry, added] = ordered_.try_emplace(generation, ordered);
			if (!added && entry->second == ordered)
				return;
			entry->second = ordered;
		} catch (...) {
			return;
		}
		std::fprintf(file_,
			"uml-x86 jit-space generation=%lu ordered=%d rip=%#llx\n",
			generation, ordered ? 1 : 0,
			static_cast<unsigned long long>(rip));
		std::fflush(file_);
	}

private:
	std::FILE *file_{};
	std::mutex mutex_;
	/* Each generation's mark and ordering at its last dispatch. */
	std::unordered_map<unsigned long, bool> shared_;
	std::unordered_map<unsigned long, bool> ordered_;
};

MmSharedLog &mm_shared_log()
{
	static MmSharedLog log;
	return log;
}

} // namespace

void note_x86_mm_shared(unsigned long generation, bool shared,
	std::uint64_t rip) noexcept
{
	mm_shared_log().note(generation, shared, rip);
}

bool x86_mm_shared_logging() noexcept
{
	return mm_shared_log().enabled();
}

void note_x86_jit_ordering(unsigned long generation, bool marked,
	bool ordered, std::uint64_t rip) noexcept
{
	mm_shared_log().note_jit(generation, marked, ordered, rip);
}

void report_x86_run_counters() noexcept
{
	const char *path = std::getenv("UML_X86_ACCOUNTING_LOG");
	if (path == nullptr || path[0] == '\0')
		return;
	std::FILE *file = std::fopen(path, "ae");
	if (file == nullptr)
		return;
	const auto accounting = ::uml::memory::FrameAccounting::diagnostics();
	const auto registrations = x86_64::registration_totals();
	std::fprintf(file,
		"uml-x86 accounting unresolved_kernel_writes=%llu "
		"direct_range_drops=%llu global_holds=%llu "
		"registration_failures=%llu holder_registrations=%llu "
		"flush_serial_misses=%llu registration_refusals=%llu "
		"registration_unsettled=%llu kernel_frame_writes=%llu\n",
		static_cast<unsigned long long>(accounting.unresolved_kernel_writes),
		static_cast<unsigned long long>(accounting.direct_range_drops),
		static_cast<unsigned long long>(accounting.global_holds),
		static_cast<unsigned long long>(registrations.failures),
		static_cast<unsigned long long>(registrations.registrations),
		static_cast<unsigned long long>(registrations.flush_serial_misses),
		static_cast<unsigned long long>(registrations.refusals),
		static_cast<unsigned long long>(registrations.unsettled),
		static_cast<unsigned long long>(accounting.kernel_frame_writes));
	std::fclose(file);
}

std::uint32_t x86_noncanonical_vector(StaticGuestMemory &memory,
	const x86_64::CpuState &cpu) noexcept
{
	const auto decoded = x86_64::decode(memory, cpu.rip, cpu.execution_mode);
	if (decoded.status != x86_64::DecodeStatus::decoded)
		return x86_general_protection;
	const auto &instruction = decoded.instruction;
	using x86_64::Operation;
	switch (instruction.operation) {
	case Operation::push:
	case Operation::pop:
	case Operation::push_all:
	case Operation::pop_all:
	case Operation::push_flags:
	case Operation::pop_flags:
	case Operation::enter:
	case Operation::call:
	case Operation::call_indirect:
	case Operation::far_call:
	case Operation::ret:
	case Operation::far_ret:
	case Operation::interrupt_ret:
	case Operation::segment_stack:
		if (!x86_canonical(cpu.gpr(x86_64::Register::rsp)))
			return x86_stack_segment_fault;
		break;
	case Operation::leave:
		if (!x86_canonical(cpu.gpr(x86_64::Register::rbp)))
			return x86_stack_segment_fault;
		break;
	default:
		break;
	}
	for (const auto *operand : {&instruction.destination,
		&instruction.source, &instruction.auxiliary}) {
		const auto &memory_operand = operand->memory;
		if (operand->kind == x86_64::OperandKind::memory &&
			!memory_operand.rip_relative &&
			memory_operand.segment == x86_64::Segment::none &&
			(memory_operand.base == 4 || memory_operand.base == 5))
			return x86_stack_segment_fault;
	}
	return x86_general_protection;
}

} // namespace uml::runtime

#endif
