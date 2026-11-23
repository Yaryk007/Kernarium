/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux probe of a stack frame that spans a page
 * boundary: sixteen [rsp + displacement] loads an iteration, eight in RSP's
 * page and eight in the page above it. x86_64_stack_span_probe.inc has the
 * layout; guest_x86_64_stack_span_control_loop.S is its control.
 */

#include "x86_64_probe_harness.inc"

#define SPAN_BASE rsp
#define SPAN_BELOW 72
#include "x86_64_stack_span_probe.inc"
