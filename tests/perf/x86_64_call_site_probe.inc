/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Body shared by the x86-64 call site probes, the counterpart of
 * call_site_probe.inc: a loop that calls one leaf from one site, so the
 * call's callee and the return's successor never change and the host
 * predicts both.
 *
 * CALL_SITE_DIRECT 0 calls the leaf with CALL and returns with RET. 1 is the
 * control: the same blocks, with the call and the return replaced by direct
 * jumps, so its difference from the target is what a call and a return cost
 * over two linked jumps, the stack store and load included.
 *
 * The including file includes x86_64_probe_harness.inc first, then defines
 * CALL_SITE_DIRECT.
 */

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 50000000
#endif

	probe_start
	mov	r12d, BENCH_ITERATIONS
	xor	r8d, r8d
	jmp	.Lloop

	.p2align 5
.Lloop:
#if CALL_SITE_DIRECT
	jmp	.Lleaf
#else
	call	.Lleaf
#endif
.Lback:
	sub	r12, 1
	jnz	.Lloop
	jmp	.Lresult

.Lresult:
	cmp	r8, BENCH_ITERATIONS
	jne	.Lfailure
	probe_finish

	.p2align 5
.Lleaf:
	add	r8, 1
#if CALL_SITE_DIRECT
	jmp	.Lback
#else
	ret
#endif

	probe_end

	.section .note.GNU-stack, "", @progbits
