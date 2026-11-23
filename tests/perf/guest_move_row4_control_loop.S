/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux matched control for guest_move_row4_loop.S: the
 * same instructions, with another instruction between every two moves.
 */

#include "probe_harness.inc"

#define MOVE_ROW 4
#define MOVE_W32 0
#define MOVE_CHAIN 0
#define MOVE_SPLIT 1
#include "move_probe.inc"
