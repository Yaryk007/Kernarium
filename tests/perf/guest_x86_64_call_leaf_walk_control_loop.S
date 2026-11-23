/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux matched control for
 * guest_x86_64_call_leaf_walk_loop.S: the same call sites called in index
 * order, so every call and return is predictable.
 */

#include "x86_64_probe_harness.inc"
#include "x86_64_walk_order.inc"

#define WALK_RANDOM 0
#include "x86_64_call_walk_probe.inc"
