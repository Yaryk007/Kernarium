/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux return probe whose returns miss the target
 * table: eight call sites of one leaf whose return addresses fold to one
 * table entry, called in a fixed cycle, so every return finds another site
 * there. x86_64_return_conflict_probe.inc has the layout;
 * guest_x86_64_return_conflict_control_loop.S is its control.
 */

#include "x86_64_probe_harness.inc"
#include "x86_64_walk_order.inc"

#define RETURN_CONFLICT 1
#include "x86_64_return_conflict_probe.inc"
