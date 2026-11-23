/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "runtime/execution_engine.hpp"
#include "runtime/hibernation_mode.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace uml::emulator {

inline constexpr std::size_t default_jit_executor_cache_budget =
	256U * 1024U * 1024U;
inline constexpr std::size_t maximum_jit_executor_cache_budget =
	256U * 1024U * 1024U;

/* --aot-release=ID,MANIFEST,SYSTEM: the Android release a launch boots, as its
 * own files state it (its ID and the SHA-256, in lowercase hex, of its manifest
 * and of its system partition). The launcher that attaches the release states
 * it (device/block: Storage::releaseIdentity); a launch with none boots no
 * Android release, and the ahead-of-time component stays off. */
inline constexpr std::string_view aot_release_prefix{"--aot-release="};

/* The argument that states a release; empty for an identity that is not one. */
[[nodiscard]] std::string aot_release_argument(std::string_view id,
	std::string_view manifest_sha256, std::string_view system_sha256);

/*
 * Kernel command-line policy is shared by every composition root.  Keeping
 * it outside the public C ABI makes the policy testable without constructing
 * an emulator or linking a kernel image.
 */
struct EmulatorOptions final {
	std::vector<std::string> arguments;
	::uml::runtime::ExecutionEngine engine{
		::uml::runtime::ExecutionEngine::automatic};
	/* Whole-process budget for translated-code caches. Zero selects half of
	 * guest memory, which is what the guest's own mem= argument reports;
	 * --jit-cache-budget= overrides it. Private executor caches are capped
	 * separately so the shared tier receives the remaining budget, when that
	 * is enough for one (cpu/arm64/jit/jit_cache_geometry.hpp).
	 */
	std::size_t jit_cache_budget{};
	std::size_t jit_executor_cache_budget{
		default_jit_executor_cache_budget};
	/* --gadget-cache-budget=: the process's byte budget for what the gadget
	 * engines keep per address space. Zero derives it from guest memory, the
	 * host's limit on the process and the vCPU count
	 * (runtime::set_gadget_cache_budget). */
	std::size_t gadget_cache_budget{};
	/* --memory-limit=: the footprint this launch states as the process's
	 * limit on a host that sets none (platform/host_environment.hpp), so a Mac
	 * measurement derives its budgets from the figure the iPad has. Zero
	 * states none and leaves UML_MEMORY_LIMIT_MIB, then the host's own, in
	 * force. */
	std::size_t memory_limit_bytes{};
	/* --jit-tier=: the instructions an x86-64 address space runs on the
	 * gadget engine before the x86-64 JIT takes it over. Zero is the JIT from
	 * the first instruction. */
	std::uint64_t jit_tier_instructions{};
	/* --guest-hibernation=auto|off|on: whether quiescing also writes the
	 * guest's RAM to a file and takes it out of the process
	 * (runtime/guest_hibernation.hpp). Auto is physical iOS, the one host
	 * that wires guest RAM; on forces it anywhere, for diagnosis. */
	::uml::runtime::HibernationMode guest_hibernation{
		::uml::runtime::HibernationMode::automatic};
	/* --session-record=: the file the run records how its session ended in,
	 * for the next launch (runtime/session_record.hpp). Empty keeps none. */
	std::string session_record_path;
	/* --aot, --aot=on|off|auto: ahead-of-time code for the Android image's
	 * own libraries (cpu/arm64/aot). Auto, the default, is on in a build that
	 * carries the component unless DARWIN_UML_AOT=0; on fails a launch whose
	 * build has none. */
	::uml::runtime::AotSelection aot{::uml::runtime::AotSelection::automatic};
	/* --aot-release=: the release the launch boots, when it states one. */
	bool aot_release_stated{};
	std::string aot_release_id;
	std::array<std::uint8_t, 32> aot_manifest_sha256{};
	std::array<std::uint8_t, 32> aot_system_sha256{};
	std::size_t guest_memory_bytes{};
	unsigned guest_cpus{1};
	/* Whether the engines ignore the top byte of a data address. On for every
	 * guest; --guest-no-top-byte-ignore turns it off, and the accepted
	 * --guest-top-byte-ignore turns it on, the default. */
	bool top_byte_ignore{true};
	bool informational{};
	/* uml_dir= is the host's own, so the run prepares and removes the
	 * console's location; a caller's directory is left to the caller. */
	bool host_console_directory{};
	bool valid{true};
};

/* The vCPU count the last valid ncpus= argument states, clamped to the guest
 * maximum, or the host's default when there is none: what prepare_emulator_options
 * puts in EmulatorOptions::guest_cpus. Launchers size host resources from it. */
[[nodiscard]] unsigned configured_guest_cpu_count(
	const std::vector<std::string> &arguments) noexcept;

/* The vCPU count a standalone launcher states when its command line states
 * none: the host's physical cores, at most the count the memory budget is
 * measured for (runtime::guest_memory::application_vcpu_limit, the one rule
 * the applications share). An explicit ncpus= keeps the guest maximum. */
[[nodiscard]] unsigned default_application_guest_cpu_count() noexcept;

/* The vCPU count a standalone launcher boots with: the last valid ncpus=
 * argument's, as configured_guest_cpu_count(), and
 * default_application_guest_cpu_count() when the command line has no ncpus=
 * argument, which the launcher then appends. A launcher sizes what depends
 * on the count, such as the Android timeout multiplier, from it. */
[[nodiscard]] unsigned launch_guest_cpu_count(
	const std::vector<std::string> &arguments) noexcept;

[[nodiscard]] EmulatorOptions prepare_emulator_options(
	int argc, const char *const argv[], std::string_view rootfs);

} // namespace uml::emulator
