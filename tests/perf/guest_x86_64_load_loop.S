/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux load loop: sixteen 64-bit loads through one base
 * register into four destinations, then the loop branch. Every load is a
 * memory gadget whose dispatch follows it, and each one's successor is fixed,
 * so the loop measures the cost of a predicted memory gadget and its way out.
 */

#include "x86_64_probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 40000000
#endif

	probe_start
	lea	rbx, [rip + .Lbuffer]

	mov	r8d, BENCH_ITERATIONS
	jmp	.Lloop
.Lloop:
	mov	rax, qword ptr [rbx]
	mov	rcx, qword ptr [rbx + 8]
	mov	rdx, qword ptr [rbx + 16]
	mov	rsi, qword ptr [rbx + 24]
	mov	rax, qword ptr [rbx + 32]
	mov	rcx, qword ptr [rbx + 40]
	mov	rdx, qword ptr [rbx + 48]
	mov	rsi, qword ptr [rbx + 56]
	mov	rax, qword ptr [rbx + 64]
	mov	rcx, qword ptr [rbx + 72]
	mov	rdx, qword ptr [rbx + 80]
	mov	rsi, qword ptr [rbx + 88]
	mov	rax, qword ptr [rbx + 96]
	mov	rcx, qword ptr [rbx + 104]
	mov	rdx, qword ptr [rbx + 112]
	mov	rsi, qword ptr [rbx + 120]
	sub	r8, 1
	jnz	.Lloop
	jmp	.Lresult

.Lresult:
	cmp	rax, 13
	jne	.Lfailure
	cmp	rcx, 14
	jne	.Lfailure
	cmp	rdx, 15
	jne	.Lfailure
	cmp	rsi, 16
	jne	.Lfailure

	probe_finish

	probe_end

	.data
	.p2align 12
.Lbuffer:
	.quad	1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16

	.section .note.GNU-stack, "", @progbits
