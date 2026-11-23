/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux dispatch arity probe, arrangement 1: every word
 * has one successor, the predictable control for arrangements 2 and 3.
 * dispatch_arity_probe.inc has the loop.
 */

#include "probe_harness.inc"

#define ARRANGEMENT 1
#include "dispatch_arity_probe.inc"
