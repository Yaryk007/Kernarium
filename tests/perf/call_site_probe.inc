/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Body shared by the call site probes: a loop that calls one leaf from one
 * site, so the call's callee and the return's successor never change and the
 * host predicts both.
 *
 * CALL_SITE_DIRECT 0 calls the leaf with BL and returns with RET. 1 is the
 * control: the same instructions and blocks, with the call and the return
 * replaced by direct branches, so its difference from the target is what a
 * call and a return cost over two linked branches.
 *
 * The including file defines CALL_SITE_DIRECT before this file.
 */

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 50000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mov	x20, xzr

	.p2align 5
.Lloop:
.if CALL_SITE_DIRECT
	b	.Lleaf
.else
	bl	.Lleaf
.endif
.Lback:
	subs	x19, x19, #1
	b.ne	.Lloop

	movz	x9, #(ITERATIONS & 0xffff)
	movk	x9, #((ITERATIONS >> 16) & 0xffff), lsl #16
	cmp	x20, x9
	b.ne	.Lfailure

	probe_finish

	.p2align 5
.Lleaf:
	add	x20, x20, #1
.if CALL_SITE_DIRECT
	b	.Lback
.else
	ret
.endif

	probe_end
	.section .note.GNU-stack, "", %progbits
