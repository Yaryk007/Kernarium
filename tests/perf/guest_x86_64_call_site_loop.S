/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux call site probe: one leaf called from one site
 * in a loop, so the host predicts the call and the return.
 * x86_64_call_site_probe.inc has the layout;
 * guest_x86_64_call_site_control_loop.S is its control.
 */

#include "x86_64_probe_harness.inc"

#define CALL_SITE_DIRECT 0
#include "x86_64_call_site_probe.inc"
