/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux stack memory mix: guest_x86_64_memory_mix_loop.S
 * with RSP as the base register, pointing into the middle of a page of its
 * own: loads, stores, a load-op, a read-modify-write, a compare against
 * memory and a reload, all [rsp + displacement], between register
 * arithmetic. The loop pushes, pops, calls and returns nothing.
 */

#include "x86_64_probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 60000000
#endif

	probe_start
	lea	rsp, [rip + .Lstack + 2048]
	mov	qword ptr [rsp], 5
	mov	qword ptr [rsp + 16], 3
	mov	qword ptr [rsp + 24], 0
	mov	qword ptr [rsp + 40], -1
	xor	ecx, ecx
	xor	edx, edx
	xor	edi, edi

	mov	r8d, BENCH_ITERATIONS
	jmp	.Lloop
.Lloop:
	mov	rax, qword ptr [rsp]
	add	rax, rdi
	mov	qword ptr [rsp + 8], rax
	add	rcx, qword ptr [rsp + 16]
	add	qword ptr [rsp + 24], 1
	xor	rdx, rcx
	mov	qword ptr [rsp + 32], rdx
	mov	rsi, qword ptr [rsp + 8]
	cmp	rsi, qword ptr [rsp + 40]
	je	.Lfailure
	lea	rdi, [rdi + 1]
	sub	r8, 1
	jnz	.Lloop
	jmp	.Lresult

.Lresult:
	mov	r9d, BENCH_ITERATIONS
	cmp	qword ptr [rsp + 24], r9
	jne	.Lfailure
	cmp	rdi, r9
	jne	.Lfailure
	lea	r10, [r9 + r9 * 2]
	cmp	rcx, r10
	jne	.Lfailure
	lea	r10, [r9 + 4]
	cmp	qword ptr [rsp + 8], r10
	jne	.Lfailure

	probe_finish

	probe_end

	.bss
	.p2align 12
.Lstack:
	.space	4096

	.section .note.GNU-stack, "", @progbits
