/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux branch loop: three blocks an iteration, joined by
 * two unconditional jumps and the loop branch, each with a never-taken
 * compare-and-branch, and no memory access. It is the control of the memory
 * probes for linked branches and conditional dispatch: every successor is
 * predictable and no gadget touches guest memory.
 */

#include "x86_64_probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 60000000
#endif

	probe_start
	xor	eax, eax
	xor	ecx, ecx
	xor	edx, edx
	mov	r11, -1
	mov	r8d, BENCH_ITERATIONS
	jmp	.Lloop
.Lloop:
	add	rax, 1
	cmp	rax, r11
	je	.Lfailure
	jmp	.Lsecond

	/* The third block sits before the second, so neither jump falls
	 * through to the next instruction. */
.Lthird:
	add	rdx, 1
	cmp	rdx, rax
	jne	.Lfailure
	sub	r8, 1
	jnz	.Lloop
	jmp	.Ldone

.Lsecond:
	add	rcx, 1
	test	rcx, rcx
	jz	.Lfailure
	jmp	.Lthird

.Ldone:
	mov	r8d, BENCH_ITERATIONS
	cmp	rax, r8
	jne	.Lfailure
	cmp	rcx, r8
	jne	.Lfailure
	cmp	rdx, r8
	jne	.Lfailure

	probe_finish

	probe_end

	.section .note.GNU-stack, "", @progbits
