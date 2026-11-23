/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Same-base unscaled transfers: four LDUR-family loads at negative offsets
 * from x13, then four STUR-family stores, so every transfer after the first
 * of a loop follows one through the same unwritten base. The matched control
 * is guest_unscaled_memory_run_control_loop.S.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 100000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	sub	sp, sp, #64
	add	x13, sp, #64

	/* x13 - 16 to - 9: 00 99 88 77 66 66 55 55; x13 - 8 to - 1: x9. */
	movz	x9, #0x4444
	movk	x9, #0x3333, lsl #16
	movk	x9, #0x2222, lsl #32
	movk	x9, #0x1111, lsl #48
	movz	x10, #0x9900
	movk	x10, #0x7788, lsl #16
	movk	x10, #0x6666, lsl #32
	movk	x10, #0x5555, lsl #48
	stp	x10, x9, [sp, #48]
	stp	xzr, xzr, [sp, #32]

	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
.Lload_loop:
	ldur	x0, [x13, #-8]
	ldur	w1, [x13, #-12]
	ldurh	w2, [x13, #-14]
	ldursb	x3, [x13, #-15]
	subs	x19, x19, #1
	b.ne	.Lload_loop

	cmp	x0, x9
	b.ne	.Lfailure
	lsr	x8, x10, #32
	cmp	x1, x8
	b.ne	.Lfailure
	mov	x8, #0x7788
	cmp	x2, x8
	b.ne	.Lfailure
	cmn	x3, #103
	b.ne	.Lfailure

	movz	x4, #0x0708
	movk	x4, #0x0506, lsl #16
	movk	x4, #0x0304, lsl #32
	movk	x4, #0x0102, lsl #48
	movz	w5, #0x0c0d
	movk	w5, #0x0a0b, lsl #16
	mov	w6, #0x0e0f
	mov	w7, #0x10
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
.Lstore_loop:
	stur	x4, [x13, #-24]
	stur	w5, [x13, #-28]
	sturh	w6, [x13, #-30]
	sturb	w7, [x13, #-31]
	subs	x19, x19, #1
	b.ne	.Lstore_loop

	/* x13 - 32 to - 25: 00 10 0f 0e 0d 0c 0b 0a; x13 - 24 to - 17: x4. */
	ldp	x0, x1, [sp, #32]
	movz	x8, #0x1000
	movk	x8, #0x0e0f, lsl #16
	movk	x8, #0x0c0d, lsl #32
	movk	x8, #0x0a0b, lsl #48
	cmp	x0, x8
	b.ne	.Lfailure
	cmp	x1, x4
	b.ne	.Lfailure

	add	sp, sp, #64
	probe_finish	failure_stack=64

	probe_end

	.section .note.GNU-stack, "", %progbits
