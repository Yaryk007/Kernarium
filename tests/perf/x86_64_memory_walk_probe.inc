/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Body shared by the x86-64 walk probes (guest_x86_64_load_walk_loop.S,
 * guest_x86_64_store_walk_loop.S and their _control_ twins): sixty-four walk
 * blocks, each a 64-bit load (or with WALK_STORE a store) through rbx, then
 * one register-only instruction of eight families, the block's index modulo
 * eight, then the step's loop branch. Every walk block enters the same memory
 * gadget, so the dispatch out of that gadget has eight successors, and the
 * family is the only thing that tells them apart. Everything else a step runs
 * has one successor.
 *
 * An order table the assembler builds names the walk block of every
 * step, reached by an indirect jump. With WALK_RANDOM set it holds a
 * xorshift sequence longer than a branch predictor's history reaches, so the
 * memory gadget's successor is unpredictable; without it the blocks run in
 * index order, so the successor is predictable and the control executes
 * exactly the same instructions. The difference between the two is the
 * price of a memory gadget's dispatch mispredicting.
 *
 * The including file includes x86_64_probe_harness.inc and
 * x86_64_walk_order.inc first, then defines WALK_RANDOM and WALK_STORE (0
 * or 1).
 */

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 30000000
#endif
#define WALK_BLOCKS 64
#define WALK_ORDER 16384
#define WALK_VALUE 0x5a5a

	probe_start
	lea	r14, [rip + .Lorder]
	lea	r15, [rip + .Lblocks]
	lea	rbx, [rip + .Lbuffer]

	mov	qword ptr [rbx], WALK_VALUE
	mov	r9d, WALK_VALUE
	mov	r11d, 3
	xor	r10d, r10d
	mov	r12d, BENCH_ITERATIONS
	xor	ecx, ecx
	jmp	.Lloop
.Lloop:
	movzx	eax, word ptr [r14 + rcx * 2]
	add	ecx, 1
	and	ecx, WALK_ORDER - 1
	shl	eax, 5
	add	rax, r15
	jmp	rax

.Ldone:
#if WALK_STORE
	cmp	qword ptr [rbx], WALK_VALUE
#else
	cmp	r9, WALK_VALUE
#endif
	jne	.Lfailure
	probe_finish

	/* Thirty-two bytes a walk block. */
	.p2align 5
.Lblocks:
	.set walk_index, 0
	.rept WALK_BLOCKS
	.p2align 5
#if WALK_STORE
	mov	qword ptr [rbx], r9
#else
	mov	r9, qword ptr [rbx]
#endif
	walk_family walk_index
	sub	r12, 1
	jnz	.Lloop
	jmp	.Ldone
	.set walk_index, walk_index + 1
	.endr

	probe_end

	/* The order table follows the buffer in its page. */
	.data
	.p2align 12
.Lbuffer:
	.space	64
	.p2align 4
.Lorder:
	walk_order_table WALK_ORDER, WALK_BLOCKS, WALK_RANDOM

	.section .note.GNU-stack, "", @progbits
