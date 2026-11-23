/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux probe: sixteen loads an iteration from eight
 * neighbouring pages, two from each, which a direct-mapped software TLB holds
 * in eight entries whatever its index. The matched control of
 * guest_x86_64_tlb_direct_alias_loop.S and guest_x86_64_tlb_fold_alias_loop.S;
 * x86_64_tlb_alias_probe.inc has the pages and the loop.
 */

#include "x86_64_probe_harness.inc"

#define ALIAS_PATTERN 0
#include "x86_64_tlb_alias_probe.inc"
