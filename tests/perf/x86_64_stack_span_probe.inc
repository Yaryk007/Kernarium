/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Body shared by the x86-64 page-spanning stack probes: sixteen 64-bit loads
 * an iteration at SPAN_BASE + 8 to SPAN_BASE + 128, into four destinations,
 * then the loop branch. SPAN_BASE points SPAN_BELOW bytes below a page
 * boundary, so the loads at displacements up to SPAN_BELOW lie in the page
 * below it and the others in the page above.
 *
 * With SPAN_BASE rsp, RSP's page is the lower one: SPAN_BELOW 72 is a frame
 * whose first eight slots share RSP's page and whose last eight lie in the
 * next, and SPAN_BELOW 8 puts every load in the page above RSP's. With
 * SPAN_BASE rbx, the control, the same loads go through another base
 * register.
 *
 * The including file includes x86_64_probe_harness.inc first, then defines
 * SPAN_BASE and SPAN_BELOW.
 */

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 30000000
#endif

	probe_start
	lea	SPAN_BASE, [rip + .Lpages + 4096 - SPAN_BELOW]

	mov	r8d, BENCH_ITERATIONS
	jmp	.Lloop
.Lloop:
	mov	rax, qword ptr [SPAN_BASE + 8]
	mov	rcx, qword ptr [SPAN_BASE + 16]
	mov	rdx, qword ptr [SPAN_BASE + 24]
	mov	rsi, qword ptr [SPAN_BASE + 32]
	mov	rax, qword ptr [SPAN_BASE + 40]
	mov	rcx, qword ptr [SPAN_BASE + 48]
	mov	rdx, qword ptr [SPAN_BASE + 56]
	mov	rsi, qword ptr [SPAN_BASE + 64]
	mov	rax, qword ptr [SPAN_BASE + 72]
	mov	rcx, qword ptr [SPAN_BASE + 80]
	mov	rdx, qword ptr [SPAN_BASE + 88]
	mov	rsi, qword ptr [SPAN_BASE + 96]
	mov	rax, qword ptr [SPAN_BASE + 104]
	mov	rcx, qword ptr [SPAN_BASE + 112]
	mov	rdx, qword ptr [SPAN_BASE + 120]
	mov	rsi, qword ptr [SPAN_BASE + 128]
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
.Lpages:
	.space	4096 - SPAN_BELOW + 8
	.quad	1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16
	.space	4096 + SPAN_BELOW - 8 - 128

	.section .note.GNU-stack, "", @progbits
