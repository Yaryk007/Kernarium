/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Body shared by guest_link_walk_loop.S and guest_link_walk_control_loop.S:
 * sixty-four walk blocks, each an ADD-immediate and a direct branch. That
 * branch links to a block that begins with one of eight instruction
 * families, the walk block's index modulo eight, which branches on to a
 * block that begins with the family of the index divided by eight. Every
 * walk block enters the same gadget and every hop leaves through the same
 * linked branch, so a hop's successor is the family it links to, and the
 * first hop's family leaves eight second families equally likely. A step is
 * two linked branches whose successors nothing in the host's history
 * decides.
 *
 * An order table fixed before the timed loop names the walk block of every
 * step. With WALK_RANDOM set it holds a pseudo-random sequence longer than a
 * branch predictor's history reaches, so the successors are unpredictable;
 * without it the blocks run in index order, so they are predictable, and the
 * control executes exactly the same instructions. The difference between the
 * two is the price of two linked branches mispredicting.
 *
 * The including file defines WALK_RANDOM before this file.
 */

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 40000000
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
	adr	x22, .Lblocks

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

	/* Thirty-two bytes a walk block and the two it links on to, the second
	 * of which ends the step. */
	.p2align 6
.Lblocks:
	.set walk_index, 0
	.rept WALK_BLOCKS
	.p2align 5
	add	x20, x20, #1
	b	1f
1:
	walk_family walk_index
	b	2f
2:
	walk_family (walk_index / 8)
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
