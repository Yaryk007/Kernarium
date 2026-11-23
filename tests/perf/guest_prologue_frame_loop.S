/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux prologue probe: a loop calling a leaf whose
 * 96-byte frame is stored by six STP at offsets below a SUB SP, directly
 * followed by ADD X29, SP. prologue_probe.inc has the body;
 * guest_prologue_frame_control_loop.S is its matched control.
 */

#include "probe_harness.inc"

#define PROLOGUE_FRAME 1
#define PROLOGUE_SPLIT 0
#include "prologue_probe.inc"
