// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "platform/host_network_path.hpp"

#include <memory>

namespace uml::platform {

/* Asked on the monitor's own thread before it calls the system's creation of
 * a path monitor. False has the call fail as it does when the system cannot
 * make one (it returns null); true goes on to the system. A function of
 * nothing, so that a test makes the failure at a moment of its choosing and
 * sees which thread asks. */
using HostNetworkPathAttempt = bool (*)();

/* makeAppleHostNetworkPathMonitor with each attempt asked first (none asks
 * nothing). Only the platform library's test variant defines it; the product
 * library has no such function. */
[[nodiscard]] std::unique_ptr<HostNetworkPathMonitor>
makeAppleHostNetworkPathMonitor(HostNetworkPathState &state,
				HostNetworkPathAttempt attempt);

} // namespace uml::platform
