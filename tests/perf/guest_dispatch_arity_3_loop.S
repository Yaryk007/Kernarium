/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux dispatch arity probe, arrangement 3: nearly every
 * word has three successors, in a fixed order, so their dispatches
 * mispredict.
 * dispatch_arity_probe.inc has the loop.
 */

#include "probe_harness.inc"

#define ARRANGEMENT 3
#include "dispatch_arity_probe.inc"
