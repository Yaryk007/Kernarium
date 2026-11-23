/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux dispatch arity probe, arrangement 2: about half
 * of the words have two successors.
 * dispatch_arity_probe.inc has the loop.
 */

#include "probe_harness.inc"

#define ARRANGEMENT 2
#include "dispatch_arity_probe.inc"
