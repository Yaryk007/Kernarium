/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Body shared by the call chain probes: 128 functions, 32 bytes apart, each
 * with a one-pair frame, as its prologue and epilogue heads store and load
 * it. A function adds one to X20 and calls the next one from a site of its
 * own, except the last function of a chain, which returns.
 *
 * CHAIN_LENGTH 128 makes one chain of all 128, a call stack deeper than a
 * host's return-address stack holds, so its innermost returns run while the
 * outermost calls are long gone from it. CHAIN_LENGTH 16 makes eight chains of
 * sixteen, called one after the other, the control: the same functions, frames,
 * calls and returns, each chain shallow enough to be held whole.
 *
 * The including file defines CHAIN_LENGTH before this file.
 */

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 500000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS
	.equ CHAIN_FUNCTIONS,	128
	.equ CHAIN_COUNT,	CHAIN_FUNCTIONS / CHAIN_LENGTH

	probe_start
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mov	x20, xzr

.Lloop:
	.set chain_index, 0
	.rept CHAIN_COUNT
	bl	.Lchains + 32 * CHAIN_LENGTH * chain_index
	.set chain_index, chain_index + 1
	.endr
	subs	x19, x19, #1
	b.ne	.Lloop

	movz	x9, #((ITERATIONS * CHAIN_FUNCTIONS) & 0xffff)
	movk	x9, #(((ITERATIONS * CHAIN_FUNCTIONS) >> 16) & 0xffff), lsl #16
	cmp	x20, x9
	b.ne	.Lfailure

	probe_finish

	/* Every function in a 32-byte slot of its own, so the chains' layout
	 * does not depend on where they end. */
	.p2align 6
.Lchains:
	.rept CHAIN_COUNT
	.rept CHAIN_LENGTH - 1
	.p2align 5
	stp	x29, x30, [sp, #-16]!
	mov	x29, sp
	add	x20, x20, #1
	bl	2f
	ldp	x29, x30, [sp], #16
	ret
	.p2align 5
2:
	.endr
	stp	x29, x30, [sp, #-16]!
	mov	x29, sp
	add	x20, x20, #1
	ldp	x29, x30, [sp], #16
	ret
	.p2align 5
	.endr

	probe_end
	.section .note.GNU-stack, "", %progbits
