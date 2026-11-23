/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux move-row probe: rows of four 64-bit register
 * moves, fed by the instruction before them and read by the one behind.
 * move_probe.inc has the body; guest_move_row4_control_loop.S is its
 * matched control.
 */

#include "probe_harness.inc"

#define MOVE_ROW 4
#define MOVE_W32 0
#define MOVE_CHAIN 0
#define MOVE_SPLIT 0
#include "move_probe.inc"
