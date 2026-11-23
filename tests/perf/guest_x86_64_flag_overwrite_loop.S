/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux flag-overwrite loop: twelve register-only
 * arithmetic and logical instructions in a row, each of whose flags the next
 * one replaces before anything reads them, then twelve flag-neutral register
 * moves. It is bound by what a flag producer records: a change to the record
 * a producer writes for flags nobody reads moves it.
 * guest_x86_64_flag_overwrite_control_loop.S has the same instructions with a
 * move after each producer.
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
	sub	rbx, 1
	xor	rcx, r11
	or	rdx, r11
	and	rdx, r12
	add	rsi, 2
	sub	rdi, 2
	xor	rcx, r11
	add	r9, 3
	sub	r9, 1
	add	rax, 1
	sub	rbx, 1
	mov	r10, r11
	mov	r14, r12
	mov	r15, r13
	mov	r10, r11
	mov	r14, r12
	mov	r15, r13
	mov	r10, r11
	mov	r14, r12
	mov	r15, r13
	mov	r10, r11
	mov	r14, r12
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
