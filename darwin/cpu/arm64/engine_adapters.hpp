/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "cpu/aot_selection.hpp"
#include "cpu/engine.hpp"

#include <memory>

namespace uml::cpu::arm64::detail {

[[nodiscard]] std::unique_ptr<CpuEngine> make_gadget_engine(
	EngineConfiguration configuration);
[[nodiscard]] std::unique_ptr<CpuEngine> make_jit_engine(
	EngineConfiguration configuration);
[[nodiscard]] bool native_jit_engine_available() noexcept;
/* A launch's word on the ahead-of-time component for the gadget engine and the
 * release it boots, before its first executor; false when it cannot be kept
 * (cpu/arm64/aot). A build without the component keeps `off` and `automatic`
 * and refuses `on`. */
[[nodiscard]] bool select_aot(AotSelection selection,
	const AotRelease *release) noexcept;
/* The end of a session of the process (its guest released after a clean
 * power-off): the next run chooses again (cpu/arm64/aot). */
void end_aot_session() noexcept;
/* The session's choice as it stands, and putting it back (cpu/arm64/aot). */
[[nodiscard]] int aot_choice() noexcept;
void restore_aot_choice(int choice) noexcept;

} // namespace uml::cpu::arm64::detail
