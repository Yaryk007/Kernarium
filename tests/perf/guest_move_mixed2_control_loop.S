/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux matched control for guest_move_mixed2_loop.S:
 * the same instructions, with another instruction between the two moves.
 */

#include "probe_harness.inc"

#define MOVE_ROW 2
#define MOVE_W32 2
#define MOVE_CHAIN 0
#define MOVE_SPLIT 1
#include "move_probe.inc"
