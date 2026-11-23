/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Body shared by guest_x86_64_return_conflict_loop.S and
 * guest_x86_64_return_conflict_control_loop.S: eight call sites of one leaf,
 * each on a page of its own, called in a fixed cycle. The block a site's
 * return lands on runs one of eight register-only families, the site's
 * index, and jumps straight to the next site, so the leaf's RET is the only
 * indirect branch and the host predicts every successor.
 *
 * The target table a RET consults folds a return address to
 * (address ^ address >> 12) modulo 4096. With RETURN_CONFLICT 1 the sites
 * are 0x1001 bytes apart from a 32 KiB boundary, so the eight return
 * addresses fold to one entry, and each return finds there the site the
 * previous return trained; with 0 they are 0x1000 apart and fold to eight
 * entries, the control that executes exactly the same instructions.
 *
 * The including file includes x86_64_probe_harness.inc and
 * x86_64_walk_order.inc first, then defines RETURN_CONFLICT (0 or 1).
 */

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 2000000
#endif
#if RETURN_CONFLICT
#define CONFLICT_STRIDE 0x1001
#else
#define CONFLICT_STRIDE 0x1000
#endif

	probe_start
	mov	r11d, 3
	xor	r10d, r10d
	xor	r8d, r8d
	mov	r12d, BENCH_ITERATIONS
	jmp	.Lsites + 3

.Ldone:
	cmp	r8, BENCH_ITERATIONS
	jne	.Lfailure
	probe_finish

.Lleaf:
	add	r8, 1
	ret

	/* A site's CALL starts three bytes into its slot, so its return
	 * address is eight bytes in. */
	.p2align 15
.Lsites:
	.irp site, 0, 1, 2, 3, 4, 5, 6, 7
	.org	.Lsites + \site * CONFLICT_STRIDE + 3, 0xcc
	call	.Lleaf
	walk_family \site
	sub	r12, 1
	jz	.Ldone
	jmp	.Lsites + ((\site + 1) & 7) * CONFLICT_STRIDE + 3
	.endr

	probe_end

	.section .note.GNU-stack, "", @progbits
