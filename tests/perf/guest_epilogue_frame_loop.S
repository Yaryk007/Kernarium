/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux epilogue probe: a loop calling a leaf whose
 * 96-byte frame is popped by six LDP at offsets and ADD SP, directly
 * followed by its RET. epilogue_probe.inc has the body;
 * guest_epilogue_frame_control_loop.S is its matched control.
 */

#include "probe_harness.inc"

#define EPILOGUE_FRAME 1
#define EPILOGUE_SPLIT 0
#include "epilogue_probe.inc"
