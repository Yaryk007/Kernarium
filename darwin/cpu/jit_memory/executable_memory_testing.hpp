// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

/* Steers the debugger requests of the region test variant
 * (UML_JIT_MEMORY_REGION_TEST), which never reaches a product: only that
 * variant defines these, so a product that names one fails to link. By default a
 * request is answered at once and P_TRACED reads as set, which is what the
 * region's own tests need; a test of the request itself makes requests real,
 * so that the breakpoint runs in a process no debugger services. */
namespace uml::cpu::jit_memory::testing {

/* Whether a request executes `brk #0xf00d` (true) or is answered at once. */
void send_real_requests(bool send) noexcept;

/* The function that answers "is a debugger attached" in place of P_TRACED; none
 * (nullptr) reads as attached. */
void set_traced_probe(bool (*probe)() noexcept) noexcept;

/* One request through the production path, with the command in x16: 0
 * answered, 1 no debugger, 2 timed out, 3 unanswered while a debugger is still
 * attached. */
[[nodiscard]] int request(std::uint64_t command) noexcept;

/* The breakpoints the trap guard has stepped over. */
[[nodiscard]] unsigned unanswered_requests_seen() noexcept;

} // namespace uml::cpu::jit_memory::testing
