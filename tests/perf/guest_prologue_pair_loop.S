/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux prologue probe: a loop calling a leaf whose
 * 32-byte frame is pushed by STP X29, X30 pre-index and STR of one
 * register, directly followed by MOV X29, SP. prologue_probe.inc has the
 * body; guest_prologue_pair_control_loop.S is its matched control.
 */

#include "probe_harness.inc"

#define PROLOGUE_FRAME 0
#define PROLOGUE_SPLIT 0
#include "prologue_probe.inc"
