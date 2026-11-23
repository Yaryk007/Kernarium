/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux probe of a mispredicted dispatch out of a store:
 * as guest_x86_64_load_walk_loop.S, but every walk block enters the same
 * store gadget. guest_x86_64_store_walk_control_loop.S is its predictable
 * control.
 */

#include "x86_64_probe_harness.inc"
#include "x86_64_walk_order.inc"

#define WALK_RANDOM 1
#define WALK_STORE 1
#include "x86_64_memory_walk_probe.inc"
