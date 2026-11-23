/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux link misprediction probe: sixty-four small
 * blocks in a pseudo-random order, two direct jumps a step, each linking to
 * a successor that begins with one of eight families.
 * x86_64_link_walk_probe.inc has the layout;
 * guest_x86_64_link_walk_control_loop.S is its predictable control.
 */

#include "x86_64_probe_harness.inc"
#include "x86_64_walk_order.inc"

#define WALK_RANDOM 1
#include "x86_64_link_walk_probe.inc"
