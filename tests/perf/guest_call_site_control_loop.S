/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux control for guest_call_site_loop.S: the same loop
 * and leaf, with the call and the return replaced by direct branches.
 */

#include "probe_harness.inc"

	.equ CALL_SITE_DIRECT,	1
#include "call_site_probe.inc"
