/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux probe of accesses that cross a page boundary:
 * eight 64-bit loads an iteration, each starting one to seven bytes below the
 * boundary, and eight stores of what four of them read back to the same
 * crossing addresses, so every access takes the engine's crossing path. The
 * stores keep the bytes as the data section holds them, which the final loads
 * check.
 */

#include "x86_64_probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 1000000
#endif

	probe_start
	lea	rbx, [rip + .Lpages + 4096]

	mov	r8d, BENCH_ITERATIONS
	jmp	.Lloop
.Lloop:
	mov	rax, qword ptr [rbx - 7]
	mov	rcx, qword ptr [rbx - 6]
	mov	rdx, qword ptr [rbx - 5]
	mov	rsi, qword ptr [rbx - 3]
	mov	qword ptr [rbx - 7], rax
	mov	qword ptr [rbx - 6], rcx
	mov	qword ptr [rbx - 5], rdx
	mov	qword ptr [rbx - 3], rsi
	mov	rax, qword ptr [rbx - 4]
	mov	rcx, qword ptr [rbx - 2]
	mov	rdx, qword ptr [rbx - 1]
	mov	rsi, qword ptr [rbx - 3]
	mov	qword ptr [rbx - 4], rax
	mov	qword ptr [rbx - 2], rcx
	mov	qword ptr [rbx - 1], rdx
	mov	qword ptr [rbx - 3], rsi
	sub	r8, 1
	jnz	.Lloop
	jmp	.Lresult

.Lresult:
	/* Bytes 1 to 16 lie at rbx - 8 to rbx + 7. */
	movabs	rdi, 0x0c0b0a0908070605
	cmp	rax, rdi
	jne	.Lfailure
	movabs	rdi, 0x0e0d0c0b0a090807
	cmp	rcx, rdi
	jne	.Lfailure
	movabs	rdi, 0x0f0e0d0c0b0a0908
	cmp	rdx, rdi
	jne	.Lfailure
	movabs	rdi, 0x0d0c0b0a09080706
	cmp	rsi, rdi
	jne	.Lfailure

	probe_finish

	probe_end

	.data
	.p2align 12
.Lpages:
	.space	4096 - 8
	.byte	1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16
	.space	4096 - 8

	.section .note.GNU-stack, "", @progbits
