/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux control for guest_x86_64_stack_span_loop.S and
 * guest_x86_64_stack_off_page_loop.S: the same sixteen loads across a page
 * boundary through RBX. x86_64_stack_span_probe.inc has the layout.
 */

#include "x86_64_probe_harness.inc"

#define SPAN_BASE rbx
#define SPAN_BELOW 72
#include "x86_64_stack_span_probe.inc"
