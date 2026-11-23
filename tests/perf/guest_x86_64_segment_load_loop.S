/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding x86-64/Linux probe of loads through FS and GS: the sixteen
 * loads of guest_x86_64_load_loop.S an iteration, eight with an FS override
 * and eight with a GS override, each base set by arch_prctl to one half of
 * the buffer. A segment override takes the ordinary bank's general address
 * path, which adds the segment base before translating.
 */

#include "x86_64_probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 10000000
#endif

#define SEGMENT_NR_ARCH_PRCTL 158
#define SEGMENT_ARCH_SET_GS 0x1001
#define SEGMENT_ARCH_SET_FS 0x1002

	probe_start
	lea	rbx, [rip + .Lbuffer]
	mov	eax, SEGMENT_NR_ARCH_PRCTL
	mov	edi, SEGMENT_ARCH_SET_FS
	mov	rsi, rbx
	syscall
	test	rax, rax
	jne	.Lfailure
	mov	eax, SEGMENT_NR_ARCH_PRCTL
	mov	edi, SEGMENT_ARCH_SET_GS
	lea	rsi, [rbx + 64]
	syscall
	test	rax, rax
	jne	.Lfailure

	mov	r8d, BENCH_ITERATIONS
	jmp	.Lloop
.Lloop:
	mov	rax, qword ptr fs:[0]
	mov	rcx, qword ptr fs:[8]
	mov	rdx, qword ptr fs:[16]
	mov	rsi, qword ptr fs:[24]
	mov	rax, qword ptr fs:[32]
	mov	rcx, qword ptr fs:[40]
	mov	rdx, qword ptr fs:[48]
	mov	rsi, qword ptr fs:[56]
	mov	rax, qword ptr gs:[0]
	mov	rcx, qword ptr gs:[8]
	mov	rdx, qword ptr gs:[16]
	mov	rsi, qword ptr gs:[24]
	mov	rax, qword ptr gs:[32]
	mov	rcx, qword ptr gs:[40]
	mov	rdx, qword ptr gs:[48]
	mov	rsi, qword ptr gs:[56]
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
.Lbuffer:
	.quad	1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16

	.section .note.GNU-stack, "", @progbits
