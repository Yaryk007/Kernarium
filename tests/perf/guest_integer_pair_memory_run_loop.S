/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Same-base GPR pairs: four LDP and LDPSW through x13, then four STP through
 * x14, so every pair after the first of a loop follows one through the same
 * unwritten base. The matched control is
 * guest_integer_pair_memory_run_control_loop.S.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 100000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	sub	sp, sp, #128
	mov	x13, sp
	add	x14, sp, #64

	mov	x9, #1
	mov	x10, #2
	stp	x9, x10, [x13]
	mov	x9, #3
	mov	x10, #4
	stp	x9, x10, [x13, #16]
	mov	w9, #5
	mov	w10, #6
	stp	w9, w10, [x13, #32]
	mov	w9, #-7
	mov	w10, #-8
	stp	w9, w10, [x13, #40]

	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
.Lload_loop:
	ldp	x0, x1, [x13]
	ldp	x2, x3, [x13, #16]
	ldp	w4, w5, [x13, #32]
	ldpsw	x6, x7, [x13, #40]
	subs	x19, x19, #1
	b.ne	.Lload_loop

	cmp	x0, #1
	b.ne	.Lfailure
	cmp	x1, #2
	b.ne	.Lfailure
	cmp	x2, #3
	b.ne	.Lfailure
	cmp	x3, #4
	b.ne	.Lfailure
	cmp	x4, #5
	b.ne	.Lfailure
	cmp	x5, #6
	b.ne	.Lfailure
	cmn	x6, #7
	b.ne	.Lfailure
	cmn	x7, #8
	b.ne	.Lfailure

	mov	x20, #9
	mov	x21, #10
	mov	x22, #11
	mov	x23, #12
	mov	x24, #13
	mov	x25, #14
	mov	x26, #15
	mov	x27, #16
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
.Lstore_loop:
	stp	x20, x21, [x14]
	stp	x22, x23, [x14, #16]
	stp	w24, w25, [x14, #32]
	stp	x26, x27, [x14, #48]
	subs	x19, x19, #1
	b.ne	.Lstore_loop

	ldp	x0, x1, [x14]
	cmp	x0, #9
	b.ne	.Lfailure
	cmp	x1, #10
	b.ne	.Lfailure
	ldp	x0, x1, [x14, #16]
	cmp	x0, #11
	b.ne	.Lfailure
	cmp	x1, #12
	b.ne	.Lfailure
	ldp	w0, w1, [x14, #32]
	cmp	w0, #13
	b.ne	.Lfailure
	cmp	w1, #14
	b.ne	.Lfailure
	ldp	x0, x1, [x14, #48]
	cmp	x0, #15
	b.ne	.Lfailure
	cmp	x1, #16
	b.ne	.Lfailure

	add	sp, sp, #128
	probe_finish	failure_stack=128

	probe_end

	.section .note.GNU-stack, "", %progbits
