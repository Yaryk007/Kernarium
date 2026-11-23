/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Body shared by guest_x86_64_link_walk_loop.S and
 * guest_x86_64_link_walk_control_loop.S, the x86-64 counterpart of
 * link_walk_probe.inc: sixty-four walk blocks, each an ADD-immediate and a
 * direct JMP. That jump links to a block that begins with one of eight
 * register-only families, the walk block's index modulo eight, which jumps on
 * to a block that begins with the family of the index divided by eight and
 * ends the step. Every walk block enters the same gadget and every hop leaves
 * through the same linked jump, so a hop's successor is the family it links
 * to, and the first hop's family leaves eight second families equally likely.
 * A step is two linked jumps whose successors nothing in the host's history
 * decides.
 *
 * An order table the assembler builds names the walk block of every
 * step, reached by an indirect jump. With WALK_RANDOM set it holds a
 * xorshift sequence longer than a branch predictor's history reaches, so the
 * jumps' successors are unpredictable; without it the blocks run in index
 * order, so they are predictable and the control executes exactly the same
 * instructions. The difference between the two is the price of two linked
 * jumps mispredicting.
 *
 * The including file includes x86_64_probe_harness.inc and
 * x86_64_walk_order.inc first, then defines WALK_RANDOM (0 or 1).
 */

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 20000000
#endif
#define WALK_BLOCKS 64
#define WALK_ORDER 16384

	probe_start
	lea	r14, [rip + .Lorder]
	lea	r15, [rip + .Lblocks]

	mov	r11d, 3
	xor	r10d, r10d
	xor	r8d, r8d
	mov	r12d, BENCH_ITERATIONS
	xor	ecx, ecx
	jmp	.Lloop
.Lloop:
	movzx	eax, word ptr [r14 + rcx * 2]
	add	ecx, 1
	and	ecx, WALK_ORDER - 1
	shl	eax, 6
	add	rax, r15
	jmp	rax

.Ldone:
	cmp	r8, BENCH_ITERATIONS
	jne	.Lfailure
	probe_finish

	/* Sixty-four bytes a walk block and the two it links on to, the second
	 * of which ends the step. */
	.p2align 6
.Lblocks:
	.set walk_index, 0
	.rept WALK_BLOCKS
	.p2align 6
	add	r8, 1
	jmp	1f
1:
	walk_family walk_index
	jmp	2f
2:
	walk_family (walk_index / 8)
	sub	r12, 1
	jnz	.Lloop
	jmp	.Ldone
	.set walk_index, walk_index + 1
	.endr

	probe_end

	.data
	.p2align 4
.Lorder:
	walk_order_table WALK_ORDER, WALK_BLOCKS, WALK_RANDOM

	.section .note.GNU-stack, "", @progbits
