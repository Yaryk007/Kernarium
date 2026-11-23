/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux return misprediction probe whose returns miss the
 * return ring: as guest_return_walk_loop.S, but the leaf returns one
 * instruction past its call's return address, so every return is found in
 * the address space's target table. guest_return_skip_walk_control_loop.S
 * is its predictable control.
 */

#include "probe_harness.inc"
#include "walk_order.inc"

	.equ WALK_RANDOM,	1
	.equ WALK_SKIP,		1
#include "return_walk_probe.inc"
