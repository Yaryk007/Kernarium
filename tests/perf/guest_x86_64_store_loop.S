/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux store loop: sixteen 64-bit stores from four
 * registers through one base register, then the loop branch. Every store is
 * a memory gadget that leaves behind a store still on its way to memory, and
 * each one's successor is fixed. guest_x86_64_load_loop.S is the same loop
 * with loads.
 */

#include "x86_64_probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 30000000
#endif

	probe_start
	lea	rbx, [rip + .Lbuffer]
	mov	eax, 1
	mov	ecx, 2
	mov	edx, 3
	mov	esi, 4

	mov	r8d, BENCH_ITERATIONS
	jmp	.Lloop
.Lloop:
	mov	qword ptr [rbx], rax
	mov	qword ptr [rbx + 8], rcx
	mov	qword ptr [rbx + 16], rdx
	mov	qword ptr [rbx + 24], rsi
	mov	qword ptr [rbx + 32], rax
	mov	qword ptr [rbx + 40], rcx
	mov	qword ptr [rbx + 48], rdx
	mov	qword ptr [rbx + 56], rsi
	mov	qword ptr [rbx + 64], rax
	mov	qword ptr [rbx + 72], rcx
	mov	qword ptr [rbx + 80], rdx
	mov	qword ptr [rbx + 88], rsi
	mov	qword ptr [rbx + 96], rax
	mov	qword ptr [rbx + 104], rcx
	mov	qword ptr [rbx + 112], rdx
	mov	qword ptr [rbx + 120], rsi
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
	cmp	qword ptr [rbx + rax * 8], rcx
	jne	.Lfailure
	add	eax, 1
	cmp	eax, 16
	jb	.Lcheck

	probe_finish

	probe_end

	.bss
	.p2align 12
.Lbuffer:
	.space	128

	.section .note.GNU-stack, "", @progbits
