/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux control for guest_x86_64_stack_frame_loop.S: the
 * same instructions, pushes and pops included, with every other access
 * through r9 instead of RSP. x86_64_stack_frame_probe.inc has the layout.
 */

#include "x86_64_probe_harness.inc"

#define FRAME_BASE r9
#include "x86_64_stack_frame_probe.inc"
