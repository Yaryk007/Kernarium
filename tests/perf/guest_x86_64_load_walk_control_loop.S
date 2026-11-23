/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux matched control for
 * guest_x86_64_load_walk_loop.S: the same blocks and instructions walked in
 * index order, so the load gadget's successor is predictable.
 */

#include "x86_64_probe_harness.inc"
#include "x86_64_walk_order.inc"

#define WALK_RANDOM 0
#define WALK_STORE 0
#include "x86_64_memory_walk_probe.inc"
