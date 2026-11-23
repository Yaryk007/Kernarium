/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux move-row probe: rows of a 64-bit move and then
 * a 32-bit one, the commonest mixed row. move_probe.inc has the body;
 * guest_move_mixed2_control_loop.S is its matched control.
 */

#include "probe_harness.inc"

#define MOVE_ROW 2
#define MOVE_W32 2
#define MOVE_CHAIN 0
#define MOVE_SPLIT 0
#include "move_probe.inc"
