/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux matched control for
 * guest_x86_64_return_conflict_loop.S: the same sites and instructions with
 * return addresses that fold to eight target-table entries, so every return
 * is found there.
 */

#include "x86_64_probe_harness.inc"
#include "x86_64_walk_order.inc"

#define RETURN_CONFLICT 0
#include "x86_64_return_conflict_probe.inc"
