/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux probe of a mispredicted dispatch out of a load:
 * sixty-four blocks in a pseudo-random order, each entering the same load
 * gadget, whose successor is one of eight families.
 * x86_64_memory_walk_probe.inc has the layout;
 * guest_x86_64_load_walk_control_loop.S is its predictable control.
 */

#include "x86_64_probe_harness.inc"
#include "x86_64_walk_order.inc"

#define WALK_RANDOM 1
#define WALK_STORE 0
#include "x86_64_memory_walk_probe.inc"
