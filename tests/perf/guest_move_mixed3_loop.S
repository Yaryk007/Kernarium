/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux move-row probe: rows of two 64-bit moves and
 * then a 32-bit one. move_probe.inc has the body;
 * guest_move_mixed3_control_loop.S is its matched control.
 */

#include "probe_harness.inc"

#define MOVE_ROW 3
#define MOVE_W32 4
#define MOVE_CHAIN 0
#define MOVE_SPLIT 0
#include "move_probe.inc"
