/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux stack store loop: guest_x86_64_store_loop.S with
 * RSP as the base register, pointing into the middle of a page of its own,
 * so every store is a baked [rsp + displacement] access within the stack's
 * page. The loop pushes, pops, calls and returns nothing.
 */

#include "x86_64_probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 30000000
#endif

	probe_start
	lea	rsp, [rip + .Lstack + 2048]
	mov	eax, 1
	mov	ecx, 2
	mov	edx, 3
	mov	esi, 4

	mov	r8d, BENCH_ITERATIONS
	jmp	.Lloop
.Lloop:
	mov	qword ptr [rsp], rax
	mov	qword ptr [rsp + 8], rcx
	mov	qword ptr [rsp + 16], rdx
	mov	qword ptr [rsp + 24], rsi
	mov	qword ptr [rsp + 32], rax
	mov	qword ptr [rsp + 40], rcx
	mov	qword ptr [rsp + 48], rdx
	mov	qword ptr [rsp + 56], rsi
	mov	qword ptr [rsp + 64], rax
	mov	qword ptr [rsp + 72], rcx
	mov	qword ptr [rsp + 80], rdx
	mov	qword ptr [rsp + 88], rsi
	mov	qword ptr [rsp + 96], rax
	mov	qword ptr [rsp + 104], rcx
	mov	qword ptr [rsp + 112], rdx
	mov	qword ptr [rsp + 120], rsi
	sub	r8, 1
	jnz	.Lloop
	jmp	.Lresult

.Lresult:
	/* Slot k holds (k mod 4) + 1. */
	xor	eax, eax
.Lcheck:
	mov	ecx, eax
	and	ecx, 3
	add	ecx, 1
	cmp	qword ptr [rsp + rax * 8], rcx
	jne	.Lfailure
	add	eax, 1
	cmp	eax, 16
	jb	.Lcheck

	probe_finish

	probe_end

	.bss
	.p2align 12
.Lstack:
	.space	4096

	.section .note.GNU-stack, "", @progbits
