/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Body shared by the x86-64 stack frame probes: an iteration opens a frame as
 * compiled code does -- two pushes and a subtraction from RSP -- spills an
 * argument and a register, stores a 32-bit local, reloads the spills into a
 * load-op chain, stores to the red zone below RSP and reloads a byte of it,
 * compares a slot above the frame with a never-equal value and counts in a
 * slot of the caller's frame, then closes the frame with an addition and two
 * pops.
 *
 * FRAME_BASE names the base register of every access but the pushes and pops:
 * rsp for the target, r9 for the control, which points to a buffer laid out
 * like the frame and keeps every instruction of the target, the pushes, the
 * pops and the RSP arithmetic included.
 *
 * The including file includes x86_64_probe_harness.inc first, then defines
 * FRAME_BASE.
 */

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 30000000
#endif

	probe_start
	lea	rsp, [rip + .Lstack + 2048]
	lea	r9, [rip + .Lstack + 2048 + 1024]
	mov	edi, 1
	xor	ecx, ecx
	xor	r10d, r10d

	mov	r8d, BENCH_ITERATIONS
	jmp	.Lloop
.Lloop:
	push	rbp
	push	rbx
	sub	rsp, 24
	mov	qword ptr [FRAME_BASE + 8], rdi
	mov	qword ptr [FRAME_BASE], rcx
	mov	dword ptr [FRAME_BASE + 16], 7
	add	rdi, 1
	mov	rax, qword ptr [FRAME_BASE + 8]
	add	rax, qword ptr [FRAME_BASE]
	add	eax, dword ptr [FRAME_BASE + 16]
	mov	qword ptr [FRAME_BASE - 8], rax
	movzx	ecx, byte ptr [FRAME_BASE - 8]
	cmp	qword ptr [FRAME_BASE + 40], rdi
	je	.Lfailure
	add	qword ptr [FRAME_BASE + 48], 1
	add	rsp, 24
	pop	rbx
	pop	rbp
	sub	r8, 1
	jnz	.Lloop
	jmp	.Lresult

.Lresult:
	mov	r11d, BENCH_ITERATIONS
	lea	rdx, [r11 + 1]
	cmp	rdi, rdx
	jne	.Lfailure
	/* RSP as the loop body left it, for the target's caller slot. */
	sub	rsp, 40
	cmp	qword ptr [FRAME_BASE + 48], r11
	jne	.Lfailure

	probe_finish

	probe_end

	.bss
	.p2align 12
.Lstack:
	.space	4096

	.section .note.GNU-stack, "", @progbits
