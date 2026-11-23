/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux epilogue probe: a loop calling a leaf whose
 * two-pair frame is popped by LDP at an offset and LDP X29, X30 post-index,
 * directly followed by its RET. epilogue_probe.inc has the body;
 * guest_epilogue_pair_control_loop.S is its matched control.
 */

#include "probe_harness.inc"

#define EPILOGUE_FRAME 0
#define EPILOGUE_SPLIT 0
#include "epilogue_probe.inc"
