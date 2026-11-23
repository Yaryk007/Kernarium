// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <Network/Network.h>

namespace uml::platform {

/* Whether a path with this status is carried by an interface: only a
 * satisfied path is. One that needs a connection first is not yet, and an
 * unsatisfied one never is. */
[[nodiscard]] bool hostNetworkPathIsCarried(nw_path_status_t status) noexcept;

/* Whether a path with this status and reason is an outage: nothing to use.
 * Only an unsatisfied path is one. A path that needs a connection first is
 * not (the attempt itself brings it up), and neither is one whose only
 * denial is the local network, which leaves every other address reachable.
 * A function of the two values so that a test gives it each of them; the
 * monitor passes what the system reports. */
[[nodiscard]] bool hostNetworkPathIsOutage(
	nw_path_status_t status, nw_path_unsatisfied_reason_t reason) noexcept;

} // namespace uml::platform
