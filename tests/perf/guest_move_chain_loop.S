/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux move-row probe: rows of three 64-bit moves that
 * each read the previous one's destination, so the ordinary words forward
 * a register into, through and out of the row. move_probe.inc has the
 * body; guest_move_chain_control_loop.S is its matched control.
 */

#include "probe_harness.inc"

#define MOVE_ROW 3
#define MOVE_W32 0
#define MOVE_CHAIN 1
#define MOVE_SPLIT 0
#include "move_probe.inc"
