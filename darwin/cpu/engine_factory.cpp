/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "engine_factory.hpp"

#include "cpu/arm64/engine_adapters.hpp"
#include "cpu/arm64/gadgets/sme_support.hpp"
#include "cpu/arm64/fp_environment.hpp"
#include "cpu/arm64/sme_profile.hpp"
#if UML_ARM64_JIT_ENGINE
#include "cpu/arm64/jit/jit_cache_geometry.hpp"
#include "cpu/arm64/jit/jit_memory_preparation.hpp"
#include "cpu/arm64/jit/jit_sme_support.hpp"
#endif

#include <cctype>
#include <cstdint>
#include <utility>

namespace uml::cpu {

/* The guest-SME policy is on by default (platform/apple/host_features.hpp),
 * so a guest on an SME host is told of every form of the profile but SME2's,
 * which neither engine may then take as illegal. */
static_assert(arm64::gadgets::sme_support >= arm64::SmeSupport::complete);
#if UML_ARM64_JIT_ENGINE
static_assert(arm64::jit::sme_support >= arm64::SmeSupport::complete);
#endif

std::unique_ptr<CpuEngine> make_cpu_engine(
	EngineKind kind, EngineConfiguration configuration)
{
	switch (kind) {
	case EngineKind::gadgets:
		return arm64::detail::make_gadget_engine(configuration);
	case EngineKind::jit:
#if UML_ARM64_JIT_ENGINE
		return arm64::detail::make_jit_engine(configuration);
#else
		return nullptr;
#endif
	}
	return nullptr;
}

bool cpu_engine_available(EngineKind kind) noexcept
{
	switch (kind) {
	case EngineKind::gadgets:
		return true;
	case EngineKind::jit:
#if UML_ARM64_JIT_ENGINE
		return arm64::detail::native_jit_engine_available();
#else
		return false;
#endif
	}
	return false;
}

HostCapabilities engine_host_capabilities(
	[[maybe_unused]] EngineKind kind, HostCapabilities host) noexcept
{
#if UML_ARM64_JIT_ENGINE
	const bool jit = kind == EngineKind::jit;
	const auto support = jit ? arm64::jit::sme_support :
				   arm64::gadgets::sme_support;
#else
	const auto support = arm64::gadgets::sme_support;
#endif
	return ::uml::platform::without_sve(
		arm64::sme_host_for_support(std::move(host), support));
}

void materialize_architectural_state(arm64::CpuState &cpu) noexcept
{
	arm64::restore_native_fp_environment(cpu);
}

#if UML_ARM64_JIT_ENGINE
bool set_translation_cache_budget(std::size_t total_bytes,
	unsigned executors, std::size_t per_executor_bytes) noexcept
{
	return arm64::set_jit_cache_budget(
		total_bytes, executors, per_executor_bytes);
}

bool acquire_translation_cache_run_session() noexcept
{
	return arm64::acquire_jit_cache_run_session();
}

void release_translation_cache_run_session() noexcept
{
	arm64::release_jit_cache_run_session();
}

#ifndef UML_APP_STORE_RUNTIME
JitMemoryStatus prepare_jit_memory(std::size_t total_bytes,
	unsigned executors, std::size_t per_executor_bytes) noexcept
{
	return arm64::prepare_jit_memory(
		total_bytes, executors, per_executor_bytes);
}

bool jit_debugger_attached() noexcept
{
	return arm64::jit_debugger_attached();
}

void release_jit_debugger() noexcept
{
	arm64::release_jit_debugger();
}

std::size_t prepared_jit_region_bytes() noexcept
{
	return arm64::prepared_jit_region_bytes();
}
#endif
#else
/* No JIT engine, so no translated-code cache to size or to serialize. */
bool set_translation_cache_budget(std::size_t, unsigned,
	std::size_t) noexcept
{
	return false;
}

bool acquire_translation_cache_run_session() noexcept
{
	return false;
}

void release_translation_cache_run_session() noexcept {}
#endif

std::size_t parse_translation_cache_quantity(const char *text) noexcept
{
	if (text == nullptr || *text == '\0')
		return 0;
	std::uint64_t value = 0;
	const char *cursor = text;
	for (; *cursor >= '0' && *cursor <= '9'; ++cursor) {
		const auto digit =
			static_cast<std::uint64_t>(*cursor - '0');
		if (value > (~std::uint64_t{} - digit) / 10U)
			return 0;
		value = value * 10U + digit;
	}
	if (cursor == text)
		return 0;
	std::uint64_t multiplier = 1;
	switch (std::tolower(static_cast<unsigned char>(*cursor))) {
	case 'k':
		multiplier = 1024U;
		++cursor;
		break;
	case 'm':
		multiplier = 1024U * 1024U;
		++cursor;
		break;
	case 'g':
		multiplier = 1024U * 1024U * 1024U;
		++cursor;
		break;
	default:
		break;
	}
	if (*cursor != '\0')
		return 0;
	if (value > ~std::uint64_t{} / multiplier)
		return 0;
	return static_cast<std::size_t>(value * multiplier);
}

} // namespace uml::cpu
