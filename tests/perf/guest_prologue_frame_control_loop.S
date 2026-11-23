/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Matched control for guest_prologue_frame_loop.S: the same instructions,
 * with the leaf's first body instruction between its stores and its
 * ADD X29, SP. prologue_probe.inc has the body.
 */

#include "probe_harness.inc"

#define PROLOGUE_FRAME 1
#define PROLOGUE_SPLIT 1
#include "prologue_probe.inc"
