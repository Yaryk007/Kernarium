/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux control for guest_link_walk_loop.S: the same
 * blocks and instructions, walked in index order, so the branches that end
 * the blocks link to predictable successors.
 */

#include "probe_harness.inc"
#include "walk_order.inc"

	.equ WALK_RANDOM,	0
#include "link_walk_probe.inc"
