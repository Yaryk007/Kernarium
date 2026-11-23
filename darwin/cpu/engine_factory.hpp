/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "cpu/aot_selection.hpp"
#include "cpu/engine.hpp"
#include "cpu/jit_memory_status.hpp"

#include <cstddef>

namespace uml::cpu {

[[nodiscard]] std::unique_ptr<CpuEngine> make_cpu_engine(
	EngineKind kind, EngineConfiguration configuration = {});

[[nodiscard]] bool cpu_engine_available(EngineKind kind) noexcept;

/* The host an engine of `kind` presents the virtual CPU for: `host` less
 * the parts of the SME profile the engine does not implement
 * (arm64::sme_host_for_support) and less every non-streaming SVE token
 * (platform::without_sve), since no engine implements SVE. The runtime
 * configures the engine and derives the guest's HWCAPs from it. */
[[nodiscard]] HostCapabilities engine_host_capabilities(
	EngineKind kind, HostCapabilities host) noexcept;

/* Publishes the whole-process budget for translated-code caches and the
 * private per-executor cap. The first valid configuration owns process-wide
 * geometry; identical repeats succeed and incompatible later runs fail. See
 * arm64/jit/jit_cache_geometry.hpp for what the budget buys.
 */
[[nodiscard]] bool set_translation_cache_budget(std::size_t total_bytes,
	unsigned executors, std::size_t per_executor_bytes) noexcept;

/* A launch's word on ahead-of-time code (cpu/arm64/aot) and the release it
 * boots (null when the guest is not an Android release): false when it cannot
 * be kept, which is `on` for a build or a framework without the component,
 * for a release the framework was not made from (or none), or in a session
 * that already runs without it. Before each run's first executor;
 * `automatic` is on for the release the framework was made from unless the
 * environment says DARWIN_UML_AOT=0, and off for any other. A later run of
 * the process that the framework no longer fits is off, never a failure. */
[[nodiscard]] bool select_aot(AotSelection selection,
	const AotRelease *release) noexcept;

/* The end of a session of the process: its guest was released after Android's
 * clean power-off, so the next run chooses afresh, as the first run of a
 * process does. A restart inside a session is no end. */
void end_aot_session() noexcept;

/* The session's choice as it stands, and putting it back: a start that took
 * the choice and failed before its guest ran leaves it as it was. */
[[nodiscard]] int aot_choice() noexcept;
void restore_aot_choice(int choice) noexcept;

/* Serializes JIT-capable emulator runs before any guest vCPU can allocate an
 * executor slot. Other engine kinds do not take this process-wide session.
 */
[[nodiscard]] bool acquire_translation_cache_run_session() noexcept;
void release_translation_cache_run_session() noexcept;

/* Parses a byte count with an optional K/M/G suffix. Zero if unparsable. */
[[nodiscard]] std::size_t parse_translation_cache_quantity(
	const char *text) noexcept;

/* Prepares the executable memory the JIT needs on an iOS device, where a
 * debugger the user attached makes it once per process, for the whole-process
 * budget and executor count a JIT run will set, and publishes that budget
 * (set_translation_cache_budget). A product without the route answers
 * unsupported. Blocks for the seconds the debugger takes: call it from a
 * worker thread, before the run. */
[[nodiscard]] JitMemoryStatus prepare_jit_memory(std::size_t total_bytes,
	unsigned executors, std::size_t per_executor_bytes) noexcept;

/* Whether a debugger is attached to this process, and tells it to detach so a
 * process that will not use JIT does not stay under it. */
[[nodiscard]] bool jit_debugger_attached() noexcept;
void release_jit_debugger() noexcept;

/* The bytes of executable memory that preparation left wired in this process,
 * zero before one did. */
[[nodiscard]] std::size_t prepared_jit_region_bytes() noexcept;

/* Materialize any lazily resident architectural state before runtime exports
 * a guest task frame. Runtime must not know which concrete engine held it. */
void materialize_architectural_state(arm64::CpuState &cpu) noexcept;

} // namespace uml::cpu
