/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux control for guest_call_leaf_walk_loop.S: the same
 * call sites and instructions, called in index order, so each call and each
 * return has a predictable successor.
 */

#include "probe_harness.inc"
#include "walk_order.inc"

	.equ WALK_RANDOM,	0
#include "call_walk_probe.inc"
	.section .note.GNU-stack, "", %progbits
