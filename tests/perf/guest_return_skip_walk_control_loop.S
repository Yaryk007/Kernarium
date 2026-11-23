/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux control for guest_return_skip_walk_loop.S: the
 * same call sites and instructions, called in index order, so the return
 * found in the target table links to a predictable successor.
 */

#include "probe_harness.inc"
#include "walk_order.inc"

	.equ WALK_RANDOM,	0
	.equ WALK_SKIP,		1
#include "return_walk_probe.inc"
