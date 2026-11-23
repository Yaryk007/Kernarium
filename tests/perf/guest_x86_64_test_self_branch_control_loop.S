/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Matched control for guest_x86_64_test_self_branch_loop.S: the same
 * instructions with each flag-neutral register move between its compare and
 * the never-taken conditional branch, which still reads the compare's flags
 * across the move, so no compare is followed at once by its branch.
 */

#include "x86_64_probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 30000000
#endif

	probe_start
	xor	eax, eax
	mov	ebx, 7
	mov	ecx, 7
	mov	edx, 100
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
	test	esi, esi
	mov	r10, r11
	jnz	.Lfailure
	test	rdi, rdi
	mov	r11, r12
	jz	.Lfailure
	test	ebx, ebx
	mov	r12, r13
	js	.Lfailure
	test	rdx, rdx
	mov	r13, r14
	jle	.Lfailure
	test	r9d, r9d
	mov	r14, r15
	jl	.Lfailure
	test	rcx, rcx
	mov	r15, r10
	je	.Lfailure
	test	edi, edi
	mov	r10, r11
	jo	.Lfailure
	test	rsi, rsi
	mov	r11, r12
	jg	.Lfailure
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
