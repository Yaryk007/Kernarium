/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cpu/arm64/engine_adapters.hpp"

#include "cpu/arm64/engine_execution.hpp"
#include "cpu/arm64/gadgets/executor.hpp"
#if UML_AOT
#include "cpu/arm64/aot/aot_runtime.hpp"
#endif

#include <memory>

namespace uml::cpu::arm64::detail {
namespace {

#if UML_AOT
/* A64 blocks with the ahead-of-time step in front of each
 * (GadgetExecutor::run_aot_block). */
struct AotEntry : A64Entry {
	template<typename Executor>
	[[nodiscard]] static EngineEvent run_block(
		Executor &executor, CpuState &cpu,
		::uml::memory::StaticGuestMemory &memory,
		unsigned long generation, std::size_t &instructions)
	{
		return executor.run_aot_block(
			cpu, memory, generation, instructions);
	}
};
#endif

class GadgetEngine final : public CpuEngine {
public:
	explicit GadgetEngine(EngineConfiguration configuration)
		: executor_{gadgets::BuildOptions{
			.host_capabilities = configuration.host_capabilities,
		}}
	{
	}

	[[nodiscard]] EngineKind kind() const noexcept override
	{
		return EngineKind::gadgets;
	}

	[[nodiscard]] ExecutionResult run(ExecutionRequest &request) noexcept override
	{
		try {
#if UML_AOT
			/* Chosen when the executor was made, once per entry: the
			 * block loop of either path has no test of it. */
			if (executor_.aot_selected())
				return run_translated_entry<AotEntry>(executor_, request);
#endif
			return run_translated(executor_, request);
		} catch (...) {
			return {.reason = ExitReason::fatal};
		}
	}

	[[nodiscard]] ExecutionResult run_aarch32(
		ExecutionRequest &request) noexcept override
	{
		try {
			return run_translated_aarch32_blocks(executor_, request);
		} catch (...) {
			return {.reason = ExitReason::fatal};
		}
	}

private:
	gadgets::GadgetExecutor executor_;
};

} // namespace

std::unique_ptr<CpuEngine> make_gadget_engine(EngineConfiguration configuration)
{
	return std::make_unique<GadgetEngine>(configuration);
}

bool select_aot(AotSelection selection, const AotRelease *release) noexcept
{
#if UML_AOT
	return arm64::aot::select(selection, release);
#else
	(void)release;
	return selection != AotSelection::on;
#endif
}

void end_aot_session() noexcept
{
#if UML_AOT
	arm64::aot::end_session();
#endif
}

int aot_choice() noexcept
{
#if UML_AOT
	return arm64::aot::choice();
#else
	return 0;
#endif
}

void restore_aot_choice(int choice) noexcept
{
#if UML_AOT
	arm64::aot::restore_choice(choice);
#else
	(void)choice;
#endif
}

} // namespace uml::cpu::arm64::detail
