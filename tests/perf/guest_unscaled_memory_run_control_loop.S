/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Matched control for guest_unscaled_memory_run_loop.S: the same LDUR-family
 * loads, then the same STUR-family stores, at the same addresses, but each
 * through a base register of its own (x13 to x16, all pointing at the end of
 * the buffer), so none follows another transfer through the same base.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 100000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	sub	sp, sp, #64
	add	x13, sp, #64
	add	x14, sp, #64
	add	x15, sp, #64
	add	x16, sp, #64

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
	ldur	w1, [x14, #-12]
	ldurh	w2, [x15, #-14]
	ldursb	x3, [x16, #-15]
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
	stur	w5, [x14, #-28]
	sturh	w6, [x15, #-30]
	sturb	w7, [x16, #-31]
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
