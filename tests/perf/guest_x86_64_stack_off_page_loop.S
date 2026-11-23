/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux probe of stack accesses outside RSP's page:
 * sixteen [rsp + displacement] loads an iteration, every one in the page
 * above RSP's. x86_64_stack_span_probe.inc has the layout;
 * guest_x86_64_stack_span_control_loop.S is its control.
 */

#include "x86_64_probe_harness.inc"

#define SPAN_BASE rsp
#define SPAN_BELOW 8
#include "x86_64_stack_span_probe.inc"
