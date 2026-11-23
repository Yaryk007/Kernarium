/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux probe of a predicted call: one BL site in a loop
 * calls one leaf, which returns with RET, so the host predicts the callee and
 * the return's successor. call_site_probe.inc has the layout;
 * guest_call_site_control_loop.S is its matched control.
 */

#include "probe_harness.inc"

	.equ CALL_SITE_DIRECT,	0
#include "call_site_probe.inc"
