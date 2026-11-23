/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux compare-and-branch loop: eight register compares
 * against an immediate (CMP and TEST, 32 and 64 bits, eight conditions), each
 * followed at once by a conditional branch that is never taken, and a
 * flag-neutral register move before each compare. It is bound by the
 * compare-and-branch pair: a change to how a compare and the branch after it
 * dispatch moves it. guest_x86_64_compare_branch_control_loop.S has the same
 * instructions with each move between its compare and branch.
 */

#include "x86_64_probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 30000000
#endif

	probe_start
	xor	eax, eax
	mov	ebx, 7
	mov	ecx, 100
	mov	rdx, -5
	xor	esi, esi
	mov	edi, 0x40
	mov	r9d, 3
	mov	r10d, 10
	mov	r11d, 11
	mov	r12d, 12
	mov	r13d, 13
	mov	r14d, 14
	mov	r15d, 15
	mov	r8d, BENCH_ITERATIONS
	jmp	.Lloop
.Lloop:
	mov	r10, r11
	cmp	rbx, 7
	jne	.Lfailure
	mov	r11, r12
	cmp	ecx, 100
	jb	.Lfailure
	mov	r12, r13
	cmp	rdx, -5
	jl	.Lfailure
	mov	r13, r14
	test	edi, 0x40
	jz	.Lfailure
	mov	r14, r15
	cmp	rsi, 0
	jg	.Lfailure
	mov	r15, r10
	cmp	r9, 3
	ja	.Lfailure
	mov	r10, r11
	test	rbx, 8
	jnz	.Lfailure
	mov	r11, r12
	cmp	ecx, 200
	jae	.Lfailure
	add	rax, 1
	sub	r8, 1
	jnz	.Lloop
	jmp	.Lresult
.Lresult:
	mov	r8d, BENCH_ITERATIONS
	cmp	rax, r8
	jne	.Lfailure

	probe_finish

	probe_end

	.section .note.GNU-stack, "", @progbits
