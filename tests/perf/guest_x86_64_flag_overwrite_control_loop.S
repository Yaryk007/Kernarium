/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Matched control for guest_x86_64_flag_overwrite_loop.S: the same producers
 * and moves alternating, so no producer is followed at once by another.
 */

#include "x86_64_probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 25000000
#endif

	probe_start
	xor	eax, eax
	xor	ebx, ebx
	xor	ecx, ecx
	xor	edx, edx
	xor	esi, esi
	xor	edi, edi
	xor	r9d, r9d
	xor	r10d, r10d
	mov	r11d, 0x55
	mov	r12d, 0xff
	mov	r13d, 7
	xor	r14d, r14d
	xor	r15d, r15d
	mov	r8d, BENCH_ITERATIONS
	jmp	.Lloop
.Lloop:
	add	rax, 1
	mov	r10, r11
	sub	rbx, 1
	mov	r14, r12
	xor	rcx, r11
	mov	r15, r13
	or	rdx, r11
	mov	r10, r11
	and	rdx, r12
	mov	r14, r12
	add	rsi, 2
	mov	r15, r13
	sub	rdi, 2
	mov	r10, r11
	xor	rcx, r11
	mov	r14, r12
	add	r9, 3
	mov	r15, r13
	sub	r9, 1
	mov	r10, r11
	add	rax, 1
	mov	r14, r12
	sub	rbx, 1
	mov	r15, r13
	sub	r8, 1
	jnz	.Lloop
	jmp	.Lresult
.Lresult:
	mov	r8d, BENCH_ITERATIONS
	lea	r12, [r8 + r8]
	cmp	rax, r12
	jne	.Lfailure
	cmp	rsi, r12
	jne	.Lfailure
	cmp	r9, r12
	jne	.Lfailure
	neg	r12
	cmp	rbx, r12
	jne	.Lfailure
	cmp	rdi, r12
	jne	.Lfailure
	test	rcx, rcx
	jnz	.Lfailure
	cmp	rdx, 0x55
	jne	.Lfailure
	cmp	r10, r11
	jne	.Lfailure
	cmp	r14d, 0xff
	jne	.Lfailure
	cmp	r15, r13
	jne	.Lfailure

	probe_finish

	probe_end

	.section .note.GNU-stack, "", @progbits
