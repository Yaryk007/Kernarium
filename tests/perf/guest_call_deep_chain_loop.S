/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux probe of a call chain deeper than a host
 * return-address stack: 128 framed functions, each calling the next from its
 * own site. call_chain_probe.inc has the layout;
 * guest_call_deep_chain_control_loop.S is its shallow control.
 */

#include "probe_harness.inc"

	.equ CHAIN_LENGTH,	128
#include "call_chain_probe.inc"
