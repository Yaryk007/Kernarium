/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux return misprediction probe: sixty-four call
 * sites of one leaf in a pseudo-random order, so its return, found in the
 * target table, links to an unpredictable successor family.
 * x86_64_return_walk_probe.inc has the layout;
 * guest_x86_64_return_walk_control_loop.S is its predictable control.
 */

#include "x86_64_probe_harness.inc"
#include "x86_64_walk_order.inc"

#define WALK_RANDOM 1
#include "x86_64_return_walk_probe.inc"
