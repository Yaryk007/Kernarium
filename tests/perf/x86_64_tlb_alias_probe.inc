/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Body shared by the x86-64 software-TLB alias probes: eight pages of one
 * anonymous mapping, two 64-bit loads from each an iteration, into four
 * destinations, then the loop branch. ALIAS_PATTERN chooses the pages; the
 * loop is the same instructions in every probe, so the probes differ only in
 * where the pages lie.
 *
 * The mapping is 1152 MiB of address space, reserved nothing, of which the
 * probe touches eight pages. Page k is, from the mapping's first 128 MiB
 * boundary A (page number P = A >> 12, H = P >> 15):
 *
 *	0  A + 4 KiB * k: neighbours, which every index spreads (the control);
 *	1  A + 128 MiB * k: page numbers that agree in their low 15 bits;
 *	2  A + 128 MiB * k + 4 KiB * (((H + k) ^ 0x155) & 0x7fff): page numbers
 *	   whose low 15 bits xored with the next 15 are 0x155 for every k.
 *
 * The including file includes x86_64_probe_harness.inc first, then defines
 * ALIAS_PATTERN.
 */

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 4000000
#endif

#define ALIAS_NR_MMAP 9
#define ALIAS_LENGTH 0x48000000

	probe_start
	mov	eax, ALIAS_NR_MMAP
	xor	edi, edi
	mov	esi, ALIAS_LENGTH
	/* PROT_READ | PROT_WRITE; MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE. */
	mov	edx, 3
	mov	r10d, 0x4022
	mov	r8, -1
	xor	r9d, r9d
	syscall
	cmp	rax, -4096
	ja	.Lfailure
	add	rax, 0x7ffffff
	and	rax, -0x8000000
	lea	rbx, [rip + .Lpage]
	xor	ecx, ecx
.Lplace:
#if ALIAS_PATTERN == 0
	mov	rdx, rcx
	shl	rdx, 12
#else
	mov	rdx, rcx
	shl	rdx, 27
#if ALIAS_PATTERN == 2
	mov	rdi, rax
	shr	rdi, 27
	add	rdi, rcx
	xor	edi, 0x155
	and	edi, 0x7fff
	shl	rdi, 12
	add	rdx, rdi
#endif
#endif
	add	rdx, rax
	lea	rdi, [rcx + 1]
	mov	qword ptr [rdx], rdi
	add	rdi, 8
	mov	qword ptr [rdx + 8], rdi
	mov	qword ptr [rbx + rcx * 8], rdx
	add	ecx, 1
	cmp	ecx, 8
	jb	.Lplace
	mov	r8, qword ptr [rbx]
	mov	r9, qword ptr [rbx + 8]
	mov	r10, qword ptr [rbx + 16]
	mov	r11, qword ptr [rbx + 24]
	mov	r12, qword ptr [rbx + 32]
	mov	r13, qword ptr [rbx + 40]
	mov	r14, qword ptr [rbx + 48]
	mov	r15, qword ptr [rbx + 56]

	mov	ebp, BENCH_ITERATIONS
	jmp	.Lloop
.Lloop:
	mov	rax, qword ptr [r8]
	mov	rcx, qword ptr [r9]
	mov	rdx, qword ptr [r10]
	mov	rsi, qword ptr [r11]
	mov	rax, qword ptr [r12]
	mov	rcx, qword ptr [r13]
	mov	rdx, qword ptr [r14]
	mov	rsi, qword ptr [r15]
	mov	rax, qword ptr [r8 + 8]
	mov	rcx, qword ptr [r9 + 8]
	mov	rdx, qword ptr [r10 + 8]
	mov	rsi, qword ptr [r11 + 8]
	mov	rax, qword ptr [r12 + 8]
	mov	rcx, qword ptr [r13 + 8]
	mov	rdx, qword ptr [r14 + 8]
	mov	rsi, qword ptr [r15 + 8]
	sub	rbp, 1
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

	.bss
	.p2align 6
.Lpage:
	.space	64

	.section .note.GNU-stack, "", @progbits
