/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Body shared by the x86-64 call chain probes, the counterpart of
 * call_chain_probe.inc: 128 functions, 32 bytes apart, each with a frame
 * pointer. A function adds one to R8 and calls the next one from a site of
 * its own, except the last function of a chain, which returns.
 *
 * CHAIN_LENGTH 128 makes one chain of all 128, a call stack deeper than a
 * return ring or a host's return-address stack holds, so its innermost
 * returns run while the outermost calls are long gone from it. CHAIN_LENGTH
 * 16 makes eight chains of sixteen, called one after the other, the control:
 * the same functions, frames, calls and returns, each chain shallow enough to
 * be held whole.
 *
 * The including file includes x86_64_probe_harness.inc first, then defines
 * CHAIN_LENGTH.
 */

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 500000
#endif
#define CHAIN_FUNCTIONS 128
#define CHAIN_COUNT (CHAIN_FUNCTIONS / CHAIN_LENGTH)

	probe_start
	mov	r12d, BENCH_ITERATIONS
	xor	r8d, r8d
	mov	ebp, 0x5555
	jmp	.Lloop

.Lloop:
	.set chain_index, 0
	.rept CHAIN_COUNT
	call	.Lchains + 32 * CHAIN_LENGTH * chain_index
	.set chain_index, chain_index + 1
	.endr
	sub	r12, 1
	jnz	.Lloop
	jmp	.Lresult

.Lresult:
	mov	eax, BENCH_ITERATIONS * CHAIN_FUNCTIONS
	cmp	r8, rax
	jne	.Lfailure
	cmp	rbp, 0x5555
	jne	.Lfailure
	probe_finish

	/* Every function in a 32-byte slot of its own, function k at
	 * .Lchains + 32 * k, so a call names the next one by its slot. */
	.p2align 6
.Lchains:
	.set chain_function, 0
	.rept CHAIN_COUNT
	.rept CHAIN_LENGTH - 1
	.p2align 5
	push	rbp
	mov	rbp, rsp
	add	r8, 1
	call	.Lchains + 32 * (chain_function + 1)
	pop	rbp
	ret
	.set chain_function, chain_function + 1
	.endr
	.p2align 5
	push	rbp
	mov	rbp, rsp
	add	r8, 1
	pop	rbp
	ret
	.set chain_function, chain_function + 1
	.endr

	probe_end

	.section .note.GNU-stack, "", @progbits
