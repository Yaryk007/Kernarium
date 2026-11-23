/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Same-base SIMD&FP pairs: four LDP of Q, D, S and Q registers through x13,
 * then four STP through x14, so every pair after the first of a loop follows
 * one through the same unwritten base. The matched control is
 * guest_vector_pair_memory_run_control_loop.S.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 50000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	sub	sp, sp, #256
	mov	x13, sp
	add	x14, sp, #128

	movi	v24.4s, #1
	movi	v25.4s, #2
	stp	q24, q25, [x13]
	mov	x9, #3
	mov	x10, #4
	stp	x9, x10, [x13, #32]
	mov	w9, #5
	mov	w10, #6
	stp	w9, w10, [x13, #48]
	movi	v24.4s, #7
	movi	v25.4s, #8
	stp	q24, q25, [x13, #64]

	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
.Lload_loop:
	ldp	q0, q1, [x13]
	ldp	d2, d3, [x13, #32]
	ldp	s4, s5, [x13, #48]
	ldp	q6, q7, [x13, #64]
	subs	x19, x19, #1
	b.ne	.Lload_loop

	umov	w0, v0.s[0]
	cmp	w0, #1
	b.ne	.Lfailure
	umov	w0, v1.s[3]
	cmp	w0, #2
	b.ne	.Lfailure
	fmov	x0, d2
	cmp	x0, #3
	b.ne	.Lfailure
	fmov	x0, d3
	cmp	x0, #4
	b.ne	.Lfailure
	fmov	w0, s4
	cmp	w0, #5
	b.ne	.Lfailure
	fmov	w0, s5
	cmp	w0, #6
	b.ne	.Lfailure
	umov	w0, v6.s[3]
	cmp	w0, #7
	b.ne	.Lfailure
	umov	w0, v7.s[0]
	cmp	w0, #8
	b.ne	.Lfailure

	movi	v16.4s, #9
	movi	v17.4s, #10
	mov	x12, #11
	fmov	d18, x12
	mov	x12, #12
	fmov	d19, x12
	mov	w12, #13
	fmov	s20, w12
	mov	w12, #14
	fmov	s21, w12
	movi	v22.4s, #15
	movi	v23.4s, #16
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
.Lstore_loop:
	stp	q16, q17, [x14]
	stp	d18, d19, [x14, #32]
	stp	s20, s21, [x14, #48]
	stp	q22, q23, [x14, #64]
	subs	x19, x19, #1
	b.ne	.Lstore_loop

	ldr	w0, [x14]
	cmp	w0, #9
	b.ne	.Lfailure
	ldr	w0, [x14, #28]
	cmp	w0, #10
	b.ne	.Lfailure
	ldr	x0, [x14, #32]
	cmp	x0, #11
	b.ne	.Lfailure
	ldr	x0, [x14, #40]
	cmp	x0, #12
	b.ne	.Lfailure
	ldr	w0, [x14, #48]
	cmp	w0, #13
	b.ne	.Lfailure
	ldr	w0, [x14, #52]
	cmp	w0, #14
	b.ne	.Lfailure
	ldr	w0, [x14, #76]
	cmp	w0, #15
	b.ne	.Lfailure
	ldr	w0, [x14, #80]
	cmp	w0, #16
	b.ne	.Lfailure

	add	sp, sp, #256
	probe_finish	failure_stack=256

	probe_end

	.section .note.GNU-stack, "", %progbits
