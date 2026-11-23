/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Matched control for guest_narrow_memory_run_loop.S: the same byte,
 * halfword and sign-extending loads, then the same stores, at the same
 * offsets in the same buffer, but each through a base register of its own
 * (x13 to x16, all pointing at the buffer), so none follows another transfer
 * through the same base.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 100000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	sub	sp, sp, #64
	mov	x13, sp
	mov	x14, sp
	mov	x15, sp
	mov	x16, sp

	/* Bytes 0-7: 01 00 02 03 80 00 00 00; bytes 8-11: fe ff ff ff. */
	movz	x9, #0x0001
	movk	x9, #0x0302, lsl #16
	movk	x9, #0x0080, lsl #32
	movz	x10, #0xfffe
	movk	x10, #0xffff, lsl #16
	stp	x9, x10, [x13]
	stp	xzr, xzr, [x13, #16]

	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
.Lload_loop:
	ldrb	w0, [x13]
	ldrh	w1, [x14, #2]
	ldrsb	x2, [x15, #4]
	ldrsw	x3, [x16, #8]
	subs	x19, x19, #1
	b.ne	.Lload_loop

	cmp	x0, #1
	b.ne	.Lfailure
	cmp	x1, #0x302
	b.ne	.Lfailure
	cmn	x2, #128
	b.ne	.Lfailure
	cmn	x3, #2
	b.ne	.Lfailure

	mov	w4, #0x11
	mov	w5, #0x2233
	mov	w6, #0x44
	mov	w7, #0x5566
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
.Lstore_loop:
	strb	w4, [x13, #16]
	strh	w5, [x14, #18]
	strb	w6, [x15, #20]
	strh	w7, [x16, #22]
	subs	x19, x19, #1
	b.ne	.Lstore_loop

	/* Bytes 16-23: 11 00 33 22 44 00 66 55. */
	ldr	x0, [x13, #16]
	movz	x8, #0x0011
	movk	x8, #0x2233, lsl #16
	movk	x8, #0x0044, lsl #32
	movk	x8, #0x5566, lsl #48
	cmp	x0, x8
	b.ne	.Lfailure

	add	sp, sp, #64
	probe_finish	failure_stack=64

	probe_end

	.section .note.GNU-stack, "", %progbits
