// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>

namespace uml::gpu::testing {

// Available only from the test-owned transport compiled with
// UML_VHOST_USER_WAIT_TESTING=1.

/* Replaces the clock that times the polls of a backend wait and of a frame in
 * it; null restores std::chrono::steady_clock. */
void setBackendWaitClock(
    std::chrono::steady_clock::time_point (*now)() noexcept) noexcept;

/* Replaces the budget of a backend wait and the interval of its polls (the
 * stop interval of the receives inside it); zero restores a value. */
void setBackendWaitTiming(
    std::chrono::milliseconds bound,
    int pollIntervalMilliseconds) noexcept;

} // namespace uml::gpu::testing
