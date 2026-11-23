/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux stack load loop: guest_x86_64_load_loop.S with
 * RSP as the base register, pointing into the middle of a page of its own,
 * so every load is a baked [rsp + displacement] access within the stack's
 * page. The loop pushes, pops, calls and returns nothing.
 */

#include "x86_64_probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 40000000
#endif

	probe_start
	lea	rsp, [rip + .Lstack + 2048]

	mov	r8d, BENCH_ITERATIONS
	jmp	.Lloop
.Lloop:
	mov	rax, qword ptr [rsp]
	mov	rcx, qword ptr [rsp + 8]
	mov	rdx, qword ptr [rsp + 16]
	mov	rsi, qword ptr [rsp + 24]
	mov	rax, qword ptr [rsp + 32]
	mov	rcx, qword ptr [rsp + 40]
	mov	rdx, qword ptr [rsp + 48]
	mov	rsi, qword ptr [rsp + 56]
	mov	rax, qword ptr [rsp + 64]
	mov	rcx, qword ptr [rsp + 72]
	mov	rdx, qword ptr [rsp + 80]
	mov	rsi, qword ptr [rsp + 88]
	mov	rax, qword ptr [rsp + 96]
	mov	rcx, qword ptr [rsp + 104]
	mov	rdx, qword ptr [rsp + 112]
	mov	rsi, qword ptr [rsp + 120]
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
.Lstack:
	.space	2048
	.quad	1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16
	.space	2048 - 128

	.section .note.GNU-stack, "", @progbits
