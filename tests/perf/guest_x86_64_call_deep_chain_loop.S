/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux deep call chain probe: 128 framed functions,
 * each calling the next from its own site, a call stack deeper than a return
 * ring or a host return-address stack holds.
 * x86_64_call_chain_probe.inc has the layout;
 * guest_x86_64_call_deep_chain_control_loop.S is its shallow control.
 */

#include "x86_64_probe_harness.inc"

#define CHAIN_LENGTH 128
#include "x86_64_call_chain_probe.inc"
