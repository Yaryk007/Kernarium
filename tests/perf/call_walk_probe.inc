/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Body shared by the call walk probes: sixty-four call sites, each a BL to
 * one of eight small leaves with a frame, whose return lands on a block that
 * begins with one of eight instruction families, the site's index modulo
 * eight. Site i calls leaf (i / 8) % 8, so each leaf returns to eight sites of
 * eight different families. Every leaf pushes a frame of two pairs, as its
 * prologue head stores it, and pops it right before its RET, as its epilogue
 * head loads it: the shape of a small compiled function.
 *
 * As in return_walk_probe.inc, WALK_RANDOM orders the steps pseudo-randomly,
 * so both the call's and the return's successor are unpredictable, and its
 * absence orders them by index, the control that executes exactly the same
 * instructions.
 *
 * The leaves overwrite X24 and X25 and restore them, so a restore that went
 * wrong fails the probe, as does a lost call.
 *
 * The including file defines WALK_RANDOM before this file.
 */

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 20000000
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
	mov	x24, #0x24
	mov	x25, #0x25
	walk_family_inputs
	adr	x22, .Lsites

.Lloop:
	ldrh	w9, [x23, x21, lsl #1]
	add	w21, w21, #1
	and	w21, w21, #(WALK_ORDER - 1)
	add	x9, x22, x9, lsl #5
	br	x9

.Ldone:
	movz	x9, #(ITERATIONS & 0xffff)
	movk	x9, #((ITERATIONS >> 16) & 0xffff), lsl #16
	cmp	x20, x9
	b.ne	.Lfailure
	cmp	x24, #0x24
	b.ne	.Lfailure
	cmp	x25, #0x25
	b.ne	.Lfailure

	probe_finish

/* Adds one to X20 through the two registers the frame saves. */
.macro walk_leaf index
	.p2align 5
.Lleaf\index:
	stp	x29, x30, [sp, #-32]!
	stp	x25, x24, [sp, #16]
	mov	x29, sp
	add	x24, x20, #(\index + 1)
	sub	x20, x24, #\index
	ldp	x25, x24, [sp, #16]
	ldp	x29, x30, [sp], #32
	ret
.endm

	.irp leaf, 0, 1, 2, 3, 4, 5, 6, 7
	walk_leaf \leaf
	.endr

.macro walk_call index
.if ((\index >> 3) & 7) == 0
	bl	.Lleaf0
.elseif ((\index >> 3) & 7) == 1
	bl	.Lleaf1
.elseif ((\index >> 3) & 7) == 2
	bl	.Lleaf2
.elseif ((\index >> 3) & 7) == 3
	bl	.Lleaf3
.elseif ((\index >> 3) & 7) == 4
	bl	.Lleaf4
.elseif ((\index >> 3) & 7) == 5
	bl	.Lleaf5
.elseif ((\index >> 3) & 7) == 6
	bl	.Lleaf6
.else
	bl	.Lleaf7
.endif
.endm

	/* Thirty-two bytes a site: the call, then the block it returns to,
	 * which ends the step. */
	.p2align 6
.Lsites:
	.set walk_index, 0
	.rept WALK_BLOCKS
	.p2align 5
	walk_call walk_index
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
