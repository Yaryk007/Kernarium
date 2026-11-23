/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux return misprediction probe: sixty-four call sites
 * of one leaf in a pseudo-random order, so its return, found on the return
 * ring, links to an unpredictable successor family. return_walk_probe.inc
 * has the layout; guest_return_walk_control_loop.S is its predictable
 * control.
 */

#include "probe_harness.inc"
#include "walk_order.inc"

	.equ WALK_RANDOM,	1
	.equ WALK_SKIP,		0
#include "return_walk_probe.inc"
