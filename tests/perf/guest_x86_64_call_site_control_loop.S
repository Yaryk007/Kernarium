/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux matched control for
 * guest_x86_64_call_site_loop.S: the same loop and leaf with the call and
 * the return replaced by direct jumps.
 */

#include "x86_64_probe_harness.inc"

#define CALL_SITE_DIRECT 1
#include "x86_64_call_site_probe.inc"
