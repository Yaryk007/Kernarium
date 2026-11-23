/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Same-base transfers between ALU work: four loads through x13, each followed by
 * an ALU instruction that consumes the loaded value, then four stores through
 * x13 likewise. The matched control is guest_gpr_memory_mixed_control_loop.S.
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

	mov	x9, #1
	mov	x10, #2
	mov	x11, #3
	mov	x12, #4
	stp	x9, x10, [x13]
	stp	x11, x12, [x13, #16]
	mov	x4, #0
	mov	x5, #0
	mov	x6, #0
	mov	x7, #0

	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
.Lload_loop:
	ldr	x0, [x13]
	add	x4, x4, x0
	ldr	x1, [x13, #8]
	add	x5, x5, x1
	ldr	x2, [x13, #16]
	add	x6, x6, x2
	ldr	x3, [x13, #24]
	add	x7, x7, x3
	subs	x19, x19, #1
	b.ne	.Lload_loop

	/* Each sum is its value times the iteration count. */
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	cmp	x4, x19
	b.ne	.Lfailure
	add	x8, x19, x19
	cmp	x5, x8
	b.ne	.Lfailure

	mov	x4, #5
	mov	x5, #6
	mov	x6, #7
	mov	x7, #8
.Lstore_loop:
	str	x4, [x13]
	add	x4, x4, #1
	str	x5, [x13, #8]
	add	x5, x5, #1
	str	x6, [x13, #16]
	add	x6, x6, #1
	str	x7, [x13, #24]
	add	x7, x7, #1
	subs	x19, x19, #1
	b.ne	.Lstore_loop

	/* The last store of each wrote its start plus the count minus one. */
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	ldr	x0, [x13]
	add	x8, x19, #4
	cmp	x0, x8
	b.ne	.Lfailure
	ldr	x3, [x13, #24]
	add	x8, x19, #7
	cmp	x3, x8
	b.ne	.Lfailure

	add	sp, sp, #64
	probe_finish	failure_stack=64

	probe_end

	.section .note.GNU-stack, "", %progbits
