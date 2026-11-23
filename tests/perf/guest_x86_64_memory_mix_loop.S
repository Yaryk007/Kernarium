/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux memory mix: loads, stores, a load-op, a
 * read-modify-write, a compare against memory with a never-taken branch, a
 * load of a value stored in the same iteration, and register arithmetic
 * between them, as compiled x86 code interleaves them. Seven of the thirteen
 * instructions of an iteration are memory gadgets.
 */

#include "x86_64_probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 60000000
#endif

	probe_start
	lea	rbx, [rip + .Lbuffer]
	mov	qword ptr [rbx], 5
	mov	qword ptr [rbx + 16], 3
	mov	qword ptr [rbx + 24], 0
	mov	qword ptr [rbx + 40], -1
	xor	ecx, ecx
	xor	edx, edx
	xor	edi, edi

	mov	r8d, BENCH_ITERATIONS
	jmp	.Lloop
.Lloop:
	mov	rax, qword ptr [rbx]
	add	rax, rdi
	mov	qword ptr [rbx + 8], rax
	add	rcx, qword ptr [rbx + 16]
	add	qword ptr [rbx + 24], 1
	xor	rdx, rcx
	mov	qword ptr [rbx + 32], rdx
	mov	rsi, qword ptr [rbx + 8]
	cmp	rsi, qword ptr [rbx + 40]
	je	.Lfailure
	lea	rdi, [rdi + 1]
	sub	r8, 1
	jnz	.Lloop
	jmp	.Lresult

.Lresult:
	mov	r9d, BENCH_ITERATIONS
	cmp	qword ptr [rbx + 24], r9
	jne	.Lfailure
	cmp	rdi, r9
	jne	.Lfailure
	lea	r10, [r9 + r9 * 2]
	cmp	rcx, r10
	jne	.Lfailure
	lea	r10, [r9 + 4]
	cmp	qword ptr [rbx + 8], r10
	jne	.Lfailure

	probe_finish

	probe_end

	.bss
	.p2align 12
.Lbuffer:
	.space	64

	.section .note.GNU-stack, "", @progbits
