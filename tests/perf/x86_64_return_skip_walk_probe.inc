/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Body shared by guest_x86_64_return_skip_walk_loop.S and
 * guest_x86_64_return_skip_walk_control_loop.S: x86_64_return_walk_probe.inc
 * with a leaf that adds four to its return address before its RET, so every
 * return lands one four-byte instruction past its call's return address, as
 * a callee that consumes inline arguments does. A return ring that pairs the
 * return with its call misses on every return, and the address space's
 * target table finds the site. The skipped instruction adds to the call
 * count, so a return that did not skip it fails the probe.
 *
 * WALK_RANDOM orders the steps as in x86_64_return_walk_probe.inc.
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
	add	qword ptr [rsp], 4
	ret

	/* Thirty-two bytes a site: the call, the four-byte instruction its
	 * return skips, then the block it returns to, which ends the step. */
	.p2align 6
.Lsites:
	.set walk_index, 0
	.rept WALK_BLOCKS
	.p2align 5
	call	.Lleaf
	.byte	0x49, 0x83, 0xc0, 0x01	/* add r8, 1, in four bytes */
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
