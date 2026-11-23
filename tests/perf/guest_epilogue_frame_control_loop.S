/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Matched control for guest_epilogue_frame_loop.S: the same instructions,
 * with the leaf's last body instruction between its ADD SP and its RET.
 * epilogue_probe.inc has the body.
 */

#include "probe_harness.inc"

#define EPILOGUE_FRAME 1
#define EPILOGUE_SPLIT 1
#include "epilogue_probe.inc"
