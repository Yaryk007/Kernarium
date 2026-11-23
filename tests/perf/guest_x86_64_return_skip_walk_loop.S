/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux return-ring miss probe: the return walk with a
 * leaf that returns one instruction past its call's return address, so a
 * return ring misses on every return and the target table finds the site.
 * x86_64_return_skip_walk_probe.inc has the layout;
 * guest_x86_64_return_skip_walk_control_loop.S is its predictable control.
 */

#include "x86_64_probe_harness.inc"
#include "x86_64_walk_order.inc"

#define WALK_RANDOM 1
#include "x86_64_return_skip_walk_probe.inc"
