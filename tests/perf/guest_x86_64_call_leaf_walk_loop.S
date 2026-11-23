/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux leaf call probe: eight framed leaves called from
 * sixty-four sites in a pseudo-random order, so both the callee and the
 * return's successor are unpredictable.
 * x86_64_call_walk_probe.inc has the layout;
 * guest_x86_64_call_leaf_walk_control_loop.S is its predictable control.
 */

#include "x86_64_probe_harness.inc"
#include "x86_64_walk_order.inc"

#define WALK_RANDOM 1
#include "x86_64_call_walk_probe.inc"
