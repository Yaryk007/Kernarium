/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux epilogue probe: guest_epilogue_pair_loop.S's leaf
 * built for pointer authentication, so AUTIASP stands between its restores
 * and its RET. epilogue_probe.inc has the body;
 * guest_epilogue_hint_control_loop.S is its matched control.
 */

#include "probe_harness.inc"

#define EPILOGUE_FRAME 0
#define EPILOGUE_SPLIT 0
#define EPILOGUE_HINT 1
#include "epilogue_probe.inc"
