/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux register loop: sixteen register-only arithmetic,
 * logical, LEA and move instructions, then the loop branch. No gadget touches
 * guest memory, so this is the control of the memory probes: a change to
 * what a memory gadget does on its way out leaves it alone. Every final
 * register value is checked.
 */

#include "x86_64_probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 80000000
#endif

	probe_start
	xor	eax, eax
	xor	ecx, ecx
	xor	edx, edx
	xor	esi, esi
	xor	edi, edi
	xor	ebx, ebx
	xor	r9d, r9d
	xor	r10d, r10d
	mov	r11d, 0x55
	mov	r13d, 1
	xor	r14d, r14d
	xor	r15d, r15d

	mov	r8d, BENCH_ITERATIONS
	jmp	.Lloop
.Lloop:
	add	rax, 1
	sub	rcx, 1
	lea	rdx, [rdx + 2]
	xor	rsi, r11
	mov	rdi, rax
	add	rdi, rcx
	or	r9, r11
	and	r9, r11
	add	r10, 3
	sub	r10, 1
	neg	r13
	neg	r13
	lea	r14, [r14 + 1]
	mov	r15, r14
	sub	r15, rax
	add	rbx, r15
	sub	r8, 1
	jnz	.Lloop
	jmp	.Lresult

.Lresult:
	/* BENCH_ITERATIONS is even, so the XOR of rsi cancels. */
	mov	r8d, BENCH_ITERATIONS
	cmp	rax, r8
	jne	.Lfailure
	cmp	r14, r8
	jne	.Lfailure
	neg	r8
	cmp	rcx, r8
	jne	.Lfailure
	neg	r8
	lea	r12, [r8 + r8]
	cmp	rdx, r12
	jne	.Lfailure
	cmp	r10, r12
	jne	.Lfailure
	test	rsi, rsi
	jnz	.Lfailure
	test	rdi, rdi
	jnz	.Lfailure
	test	r15, r15
	jnz	.Lfailure
	test	rbx, rbx
	jnz	.Lfailure
	cmp	r9, r11
	jne	.Lfailure
	cmp	r13, 1
	jne	.Lfailure

	probe_finish

	probe_end

	.section .note.GNU-stack, "", @progbits
