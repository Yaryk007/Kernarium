/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux control for guest_call_deep_chain_loop.S: the same
 * 128 framed functions, calls and returns, as eight chains of sixteen called
 * one after the other.
 */

#include "probe_harness.inc"

	.equ CHAIN_LENGTH,	16
#include "call_chain_probe.inc"
