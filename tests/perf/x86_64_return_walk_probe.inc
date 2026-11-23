/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Body shared by guest_x86_64_return_walk_loop.S and
 * guest_x86_64_return_walk_control_loop.S, the x86-64 counterpart of
 * return_walk_probe.inc: sixty-four call sites, each a direct CALL of one
 * leaf, whose RET lands on a block that begins with one of eight
 * register-only families, the site's index modulo eight, and ends the step.
 * Every call enters the same leaf and every return leaves through the same
 * RET, which finds its site in the address space's target table, so its
 * successor is the family of the block it returns to and nothing else in the
 * host's history.
 *
 * As in x86_64_link_walk_probe.inc, WALK_RANDOM orders the steps
 * pseudo-randomly, so the return's successor is unpredictable, and its
 * absence orders them by index, the control that executes exactly the same
 * instructions.
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
	lea	r15, [rip + .Lsites]

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
	shl	eax, 5
	add	rax, r15
	jmp	rax

.Ldone:
	cmp	r8, BENCH_ITERATIONS
	jne	.Lfailure
	probe_finish

.Lleaf:
	add	r8, 1
	ret

	/* Thirty-two bytes a site: the call, then the block it returns to,
	 * which ends the step. */
	.p2align 6
.Lsites:
	.set walk_index, 0
	.rept WALK_BLOCKS
	.p2align 5
	call	.Lleaf
	walk_family walk_index
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
