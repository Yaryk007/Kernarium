/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux stack frame probe: a frame opened and closed
 * every iteration, with its spills, reloads, red-zone store and caller-slot
 * counter all [rsp + displacement] within the stack's page.
 * x86_64_stack_frame_probe.inc has the layout;
 * guest_x86_64_stack_frame_control_loop.S is its control.
 */

#include "x86_64_probe_harness.inc"

#define FRAME_BASE rsp
#include "x86_64_stack_frame_probe.inc"
