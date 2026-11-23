/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux probe: the loads of
 * guest_x86_64_tlb_alias_control_loop.S from eight pages whose page numbers,
 * the low fifteen bits xored with the next fifteen, give one value, so an
 * index that folds the page number that way holds one of them at a time and
 * one indexed by the low bits spreads them.
 */

#include "x86_64_probe_harness.inc"

#define ALIAS_PATTERN 2
#include "x86_64_tlb_alias_probe.inc"
