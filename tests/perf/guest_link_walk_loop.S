/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux linked-branch misprediction probe: sixty-four
 * small blocks in a pseudo-random order, so the direct branches that end
 * them link to unpredictable successor families. link_walk_probe.inc has the
 * layout; guest_link_walk_control_loop.S is its predictable control.
 */

#include "probe_harness.inc"
#include "walk_order.inc"

	.equ WALK_RANDOM,	1
#include "link_walk_probe.inc"
