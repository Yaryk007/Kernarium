/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux matched control for
 * guest_x86_64_call_deep_chain_loop.S: the same 128 functions, calls and
 * returns as eight chains of sixteen.
 */

#include "x86_64_probe_harness.inc"

#define CHAIN_LENGTH 16
#include "x86_64_call_chain_probe.inc"
