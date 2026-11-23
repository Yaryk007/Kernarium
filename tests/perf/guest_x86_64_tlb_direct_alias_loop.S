/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux probe: the loads of
 * guest_x86_64_tlb_alias_control_loop.S from eight pages 128 MiB apart, whose
 * page numbers agree in their low fifteen bits, so a table indexed by those
 * bits alone holds one of them at a time and a folded index spreads them.
 */

#include "x86_64_probe_harness.inc"

#define ALIAS_PATTERN 1
#include "x86_64_tlb_alias_probe.inc"
