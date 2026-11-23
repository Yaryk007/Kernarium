/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Body shared by the x86-64 call walk probes, the counterpart of
 * call_walk_probe.inc: sixty-four call sites, each a direct CALL of one of
 * eight small leaves with a frame, whose return lands on a block that begins
 * with one of eight register-only families, the site's index modulo eight.
 * Site i calls leaf (i / 8) % 8, so each leaf returns to eight sites of eight
 * different families. Every leaf pushes a frame pointer and a callee-saved
 * register and pops them right before its RET, the shape of a small compiled
 * function.
 *
 * As in x86_64_return_walk_probe.inc, WALK_RANDOM orders the steps
 * pseudo-randomly, so both the call's and the return's successor are
 * unpredictable, and its absence orders them by index, the control that
 * executes exactly the same instructions.
 *
 * The leaves overwrite RBX and restore it, so a restore that went wrong fails
 * the probe, as does a lost call.
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
	mov	ebx, 0x2424
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
	cmp	rbx, 0x2424
	jne	.Lfailure
	probe_finish

/* Adds one to R8 through the register the frame saves. */
.macro walk_leaf index
	.p2align 5
.Lleaf\index:
	push	rbp
	mov	rbp, rsp
	push	rbx
	lea	rbx, [r8 + \index + 1]
	lea	r8, [rbx - \index]
	pop	rbx
	pop	rbp
	ret
.endm

	.irp leaf, 0, 1, 2, 3, 4, 5, 6, 7
	walk_leaf \leaf
	.endr

	/* Thirty-two bytes a site: the call, then the block it returns to,
	 * which ends the step. Site i calls leaf (i / 8) % 8, at
	 * .Lleaf0 + 32 * ((i / 8) % 8). */
	.p2align 6
.Lsites:
	.set walk_index, 0
	.rept WALK_BLOCKS
	.p2align 5
	call	.Lleaf0 + 32 * ((walk_index >> 3) & 7)
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
