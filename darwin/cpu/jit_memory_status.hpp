// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace uml::cpu {

/* How the JIT's executable memory stands on an iOS device, where a debugger
 * prepares it once per process. Only the JIT engine and its executable-memory
 * owner know how; the runtime and the applications know this answer. */
enum class JitMemoryStatus : std::uint8_t {
	/* The region holds every arena the budget's geometry needs, and the
	 * budget is the process's. */
	ready = 0,
	/* This product has no such route: macOS, the Simulator, an App Store
	 * runtime. */
	unsupported = 1,
	/* No debugger is attached to the process. */
	no_debugger = 2,
	/* The region could not be prepared. */
	failed = 3,
	/* An earlier JIT run of this process published another budget and
	 * executor count, and the region was prepared for that one. */
	other_settings = 4,
};

} // namespace uml::cpu
