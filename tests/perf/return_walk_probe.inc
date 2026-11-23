/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Body shared by the return walk probes: sixty-four call sites, each a BL to
 * one leaf function, whose return lands on a block that begins with one of
 * eight instruction families, the site's index modulo eight. Every call
 * returns through the same RET, so its successor is the family of the block
 * it returns to and nothing else in the host's history.
 *
 * With WALK_SKIP set the leaf returns one instruction past the address the
 * call left in X30, as code that reads an inline argument after its call
 * does, so the return never matches the record the call pushed on the
 * return ring and is found in the address space's target table instead.
 * Without it every return matches the ring.
 *
 * As in link_walk_probe.inc, WALK_RANDOM orders the steps pseudo-randomly,
 * so the return's successor is unpredictable, and its absence orders them by
 * index, the control that executes exactly the same instructions.
 *
 * The including file defines WALK_RANDOM and WALK_SKIP before this file.
 */

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 30000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS
	.equ WALK_BLOCKS,	64
	.equ WALK_ORDER,	16384

	probe_start
	adrp	x23, .Lorder
	add	x23, x23, :lo12:.Lorder
	walk_order_fill x23, WALK_ORDER, WALK_BLOCKS, WALK_RANDOM

	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mov	x20, xzr
	mov	w21, wzr
	walk_family_inputs
	adr	x22, .Lsites

.Lloop:
	ldrh	w9, [x23, x21, lsl #1]
	add	w21, w21, #1
	and	w21, w21, #(WALK_ORDER - 1)
	add	x9, x22, x9, lsl #5
	br	x9

.Ldone:
	movz	x24, #(ITERATIONS & 0xffff)
	movk	x24, #((ITERATIONS >> 16) & 0xffff), lsl #16
	cmp	x20, x24
	b.ne	.Lfailure

	probe_finish

.Lleaf:
	add	x20, x20, #1
.if WALK_SKIP
	add	x30, x30, #4
.endif
	ret

	/* Thirty-two bytes a site: the call, then the block it returns to,
	 * which ends the step. */
	.p2align 6
.Lsites:
	.set walk_index, 0
	.rept WALK_BLOCKS
	.p2align 5
	bl	.Lleaf
.if WALK_SKIP
	brk	#1
.endif
	walk_family walk_index
	subs	x19, x19, #1
	b.ne	.Lloop
	b	.Ldone
	.set walk_index, walk_index + 1
	.endr

	probe_end

	.bss
	.p2align 4
.Lorder:
	.space	WALK_ORDER * 2
