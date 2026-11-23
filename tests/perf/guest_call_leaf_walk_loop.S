/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux call and return misprediction probe: sixty-four
 * call sites of eight framed leaves in a pseudo-random order, so each call's
 * callee and each return's successor family are unpredictable, and every
 * return runs an epilogue head. call_walk_probe.inc has the layout;
 * guest_call_leaf_walk_control_loop.S is its predictable control.
 */

#include "probe_harness.inc"
#include "walk_order.inc"

	.equ WALK_RANDOM,	1
#include "call_walk_probe.inc"
	.section .note.GNU-stack, "", %progbits
