/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Matched control for guest_epilogue_hint_loop.S: the same instructions,
 * with the leaf's last body instruction between its restores and its
 * AUTIASP. epilogue_probe.inc has the body.
 */

#include "probe_harness.inc"

#define EPILOGUE_FRAME 0
#define EPILOGUE_SPLIT 1
#define EPILOGUE_HINT 1
#include "epilogue_probe.inc"
